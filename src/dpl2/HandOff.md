# Filler repair handoff

This is the only migration and validation document for filler repair. The
behavioral contract is `docs/filler_vt_overlay_repair_spec.md`.

## 1. What to copy

Recommended runtime payload:

```text
src/dpl2/src/fillerRepair2/*
    -> <destination>/src/dpl2/src/fillerRepair/*
```

`fillerRepair2` contains only runtime headers, two source files, and a small
CMake target. It excludes repository-local tests and test-only engine entry
points. `src/dpl2/src/fillerRepair` remains the complete verification source
and contains the portable tests.

For dump-based debugging also copy:

```text
src/dpl2/dpl2ui/FillerRepairDumpReplay.{hh,cc}
src/dpl2/dpl2ui/testFillerRepairCmd.{hh,cc}
```

Do not copy `src/dpl2/test/local`: it is the repository-local fake-UDM
harness, not a runtime dependency.

## 2. Required destination contracts

The payload assumes the following existing dpl2/checker behavior. Port the
matching changes when the destination does not already contain them.

### Checker

- `DRCChecker` exposes the five-argument `check(..., fcRecord)` virtual.
- `ImplantLayerChecker` exposes `checkPlaceWithOverlays(...)` and returns one
  `CheckResult` per candidate in input order.
- Ordinary checker instances default filler repair on; checker-helper-only
  instances disable it.
- `ImplantLayerChecker` stores a non-owning engine pointer and calls it only
  after direct DRC fails. It appends successful records to the caller vector.
- The checker obtains `PhysDesMgr` from its Grid, not global Session state.
- Overlay checks are const/non-mutating, do not re-enter repair, and support
  concurrent reads after initialization.
- Checker master metadata may be lazily completed for a registered request
  master; shared tables must not be observed half-built.

Relevant files:

```text
src/dpl2/src/drc/DRCChecker.h
src/dpl2/src/drc/ImplantLayerChecker.{h,cpp}
```

### Shared records and filler classification

- `CellChangeRecord` lives in `infrastructure/Objects.h` and contains
  operation, cell handle, x/y, original/new library-cell IDs, and orientation.
- `ipl::FillerChanges` is a vector of that same record.
- `Master::isFiller()` and `Node::isFiller()` are the downstream filler
  authority. `Node::isStdCell()` excludes fillers.
- `Network` non-owningly stores the active `fillerSetting`.
- `Network::addNode`/`updateNode` fail closed on missing manager, invalid
  physical mapping, or unregistered master; `updateNode` preserves physical
  orientation and refreshes node type.

Relevant files:

```text
src/dpl2/src/infrastructure/Objects.h
src/dpl2/src/infrastructure/Object.cpp
src/dpl2/src/infrastructure/network.{h,cpp}
```

### Grid and DePlace

- Grid retains `PhysDesMgr`, exposes the row/column helpers used by the
  checker, and bounds-checks unallocated/out-of-range pixels.
- Grid occupancy includes fillers and all other non-terminal site occupants.
- Logical row/site extents, not stale backing-vector capacity, define
  `isFullUtil()`.
- DePlace binds its active `fillerSetting` to Network.
- Before checker/engine construction,
  `DePlace::registerFillerRepairMasters()` registers every configured master
  with the real edge table and refreshes matching Nodes as fillers.

Relevant files:

```text
src/dpl2/src/infrastructure/Grid.{h,cpp}
src/dpl2/src/DePlace.cpp
src/dpl2/include/dpl2/DePlace.h
```

The engine never calls `Network::addMaster`; it lacks the real edge table.
Missing configured masters therefore make initialization fail instead of
creating incomplete metadata.

## 3. Build wiring

Define one parent-owned interface target before adding the payload:

```cmake
add_library(dpl2_filler_repair_deps INTERFACE)
target_link_libraries(dpl2_filler_repair_deps
  INTERFACE <udm-targets> <dpl2-infrastructure> <implant-checker>)

add_subdirectory(<destination>/src/dpl2/src/fillerRepair fillerRepair)
target_link_libraries(<owning-target> PRIVATE dpl2::fillerRepair)
```

The runtime payload requires C++20. Its CMake lists `RepairPlanner.cpp` and
`FillerRepairEngine.cpp`; the destination should link the target rather than
repeat that source list.

The complete verification directory additionally exposes:

| Target | Purpose | Standard |
|---|---|---:|
| `dpl2::fillerRepair` | planner plus runtime engine | C++20 |
| `dpl2::fillerRepairPlanner` | database-free planner boundary | C++17 |

When the complete directory is built standalone, UDM can be supplied through
`DPL2_UDM_INCLUDE_DIRS` and `DPL2_UDM_LIBRARIES`; destination builds should
prefer `dpl2_filler_repair_deps`.

## 4. Initialization and caller flow

Initialization order is part of the contract:

1. Load/synchronize UDM, Grid, and Network for one design revision.
2. Configure `fillerSetting` and bind it to Network.
3. Register configured filler masters through DePlace's real edge table.
4. Construct `ImplantLayerChecker(grid, network)`.
5. Construct `FillerRepairEngine(grid, network)` and call `init(checker)`.
6. Bind the initialized engine with `checker.setFillerRepairEngine(&engine)`.
7. Start read-only worker calls.

Example caller:

```cpp
std::vector<CellChangeRecord> changes;
const bool legal = checker.check(node, x, y, orient, changes);
if (legal) {
  commitTargetAndFillerChanges(changes);
}
```

The caller owns both objects and the result vector. Repair never commits. If
initialization fails, do not bind the engine. If the design revision changes,
stop workers and rebuild the checker/engine pair; there is no reset/context
API.

## 5. Destination assumptions to verify

Before enabling repair on a real design, confirm:

- Grid, Network, checker, engine, `fillerSetting`, and UDM describe the same
  revision and outlive all worker calls.
- Network includes every placed/fixed physical object intersecting the core,
  including hard macros; blockages remain Grid state.
- configured filler masters are present in Network with real edge data;
- `Node::isFiller()` is correct for placed fillers and replacement masters
  come from the configured filler list;
- request row/column use the same frame as Grid and checker snapshots;
- row iteration is y ordered, row origins agree with the core frame, and
  supported orientations are R0/R180/MX/MY;
- checker rule reach returned by `getMaxRuleValue()` is in sites;
- overlay result count and ordering exactly match candidate input;
- no Grid, Network, UDM, or filler-setting mutation overlaps worker checks.

Any failure must be reported and return no filler changes. Do not weaken a
fatal initialization diagnostic merely to enable the feature.

## 6. Portable verification

Use the complete `src/dpl2/src/fillerRepair` directory for tests:

```sh
cmake -S <srcroot>/fillerRepair -B build-fr \
  -DDPL2_FILLER_REPAIR_BUILD_TESTS=ON \
  -DDPL2_UDM_INCLUDE_DIRS='<include dirs>' \
  -DDPL2_UDM_LIBRARIES='<libraries or targets>' \
  -DDPL2_RUNTIME_LIBRARIES='<existing infra/checker targets>'
cmake --build build-fr
ctest --test-dir build-fr --output-on-failure
```

Run the same suite with ASan and with `-Wall -Wextra -Werror`. The portable
suite contains:

- database-free planner GoogleTests using seam doubles;
- checker/planner E2E GoogleTests using the real `ImplantLayerChecker` and
  helper-built data, without constructing UDM objects.

Repository-local full regression:

```sh
src/dpl2/test/build_all.sh
```

This adds fake-UDM engine/infrastructure cases. It validates the local boundary
but does not replace a build and smoke test against the destination's real UDM
and infrastructure.

## 7. Dump replay

`ImplantLayerCheckerHelper::dump()` writes gzip dump v5. It preserves checker
state, row base polarity, and the serializable filler-setting projection.
Loading remains backward-compatible, but filler repair replay requires a dump
that contains configured filler master IDs.

Run a full replay:

```text
test_filler_repair -load <checker.dump.gz>
```

Run one dumped proposal:

```text
test_filler_repair -load <checker.dump.gz> -inst <node-id> -master <master-id>
```

IDs in load mode are numeric IDs stored in the dump. Replay reconstructs the
helper Grid, Network, and checker, then drives the pure planner through the
real overlay API. It does not construct `FillerRepairEngine`, require a loaded
Design, or mutate the reconstructed placement.

## 8. Final real-design sign-off

Portable tests prove the planner and checker protocol, not the destination's
UDM import. Final sign-off requires one real-design run that confirms:

1. engine initialization succeeds with the expected rows, site width,
   configured masters, and non-empty compatible catalog;
2. a direct legal proposal returns no filler changes;
3. a repairable illegal proposal returns only same-footprint filler
   replacements and the checker accepts the complete overlay;
4. an unrepairable or budget-truncated proposal returns no partial changes;
5. UDM, Grid, and Network are unchanged before caller commit;
6. concurrent read-only checks are clean under the destination sanitizer and
   race-detection setup;
7. post-commit infrastructure synchronization followed by pair reconstruction
   sees the new revision.

Remaining integration risks are limited to destination-specific UDM status
values, Grid occupancy semantics for obstruction-bearing masters, row/frame
conventions, checker registration in the destination's DRC dispatcher, and
real-checker call cost used to tune search budgets.
