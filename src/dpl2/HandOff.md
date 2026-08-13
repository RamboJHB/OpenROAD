# Filler repair handoff

This is the only migration and validation document for filler repair. The
behavioral contract is `docs/filler_vt_overlay_repair_spec.md`.

## Current status

Implemented and verified in this branch:

- DePlace as the sole Design/Grid/Network/filler-setting and repair-chain
  owner, a checker that borrows that object set, and a revision-scoped repair
  engine that borrows only the checker; concurrent checks keep all request
  state local;
- atomic, pre-commit `Replace`/`Delete`/`Add` filler transactions with no UDM,
  Grid, Network, or filler-setting mutation;
- same-footprint VT repair, exact filler insertion after std-cell Delete, and
  filler removal/collateral refill before a new buffer Add;
- one- and two-row filler masters, Grid-authoritative Add orientation,
  adaptive-L1/subset search, deterministic ranking, cache and fail-closed
  budgets;
- a complete GoogleTest source tree, a compact `fillerRepair2` migration
  payload, dump replay, and repository-local OpenROAD/ODB command wiring;
- 344 normal regression tests, the same 344 under ASan, 12 focused ThreadSanitizer
  concurrency/binding tests, strict-warning compilation of the
  `fillerRepair2` migration payload, and repeatable local ODB smoke runs.

The separately imported direct-rule `checker-simple` golden suite is not part
of those 344 tests: 9/15 cases pass and six existing violation-count goldens
still disagree with this branch's checker output. No golden was changed for
the DePlace integration.

The remaining risks are destination sign-off and bounded-search behavior, not
missing runtime plumbing:

- the local ODB smoke projects ODB into test-only UDM and therefore does not
  prove destination UDM status values, handles, or lifecycle behavior;
- row/frame conventions, obstruction-aware Grid occupancy, configured-master
  registration with real edge data, and DRC dispatcher registration must be
  verified in the destination;
- exact-cover enumeration can safely miss a later tiling after 16 solutions
  or 100000 states, and adaptive/subset budgets can safely return no solution
  outside the explored space; neither path returns partial changes;
- `findLegal` safely stops after 65536 examined Grid sites or 4096 checker
  candidates, so a legal new-buffer site beyond those bounds is not promised;
- masters taller than two rows and multi-target transactions are unsupported;
- checker-backed combinatorial search is the measured latency risk: the
  adversarial three-swap case uses 1147 checker requests, while ordinary
  repairs remain sub-millisecond in the local Release baseline;
- opto worker concurrency must be tuned with the checker's internal TBB width,
  and every placement commit requires workers to stop and the checker/engine
  pair to be reconstructed for the new revision.

Sections 5, 8, and 9 are the authoritative destination checklist, performance
procedure, and final sign-off; Section 10 of the specification records the
measured baseline and optimization order.

## 1. What to copy

Recommended runtime payload:

```text
src/dpl2/src/fillerRepair2/*
    -> <destination>/src/dpl2/src/fillerRepair/*
```

`fillerRepair2` contains only runtime headers, three source files, and a small
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
- `ImplantLayerChecker(Grid*, Design*, Network*)` borrows the object set
  initialized by DePlace and exposes that same set to filler repair.
- `ImplantLayerChecker` exposes `checkPlaceWithOverlays(...)` and returns one
  `CheckResult` per candidate in input order.
- Ordinary checker instances default filler repair on; checker-helper-only
  instances disable it.
- DePlace creates `PlacementDRC`, registers the implant checker there, owns the
  engine directly, and atomically publishes that one ready non-owning engine
  pointer; null/unready/replacement binding is rejected. Node-facing
  `check()` calls repair only for failed same-footprint Replace/rotation.
  Explicit std-cell Add/Delete uses `checker.repair(targetChange, changes)`.
- The checker obtains `PhysDesMgr` from the explicit Design, not global Session
  state. DePlace owns object-set consistency; checker and engine do not compare
  design identities.
- Overlay checks are const/non-mutating, do not re-enter repair, and support
  concurrent reads after initialization.
- Overlay validation accepts one atomic mix of filler `Replace`, `Delete`, and
  request-local `Add`, validates complete one/two-row rectangles and row/site
  orientation, and requires every filler under a target Add to be deleted.
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
- Added filler names extend dpl's coordinate rule as
  `prefix + "_FR_" + row + "_" + startColumn + "_W" + width + "_H" +
  height + "_" + addIndex`, without spaces and with
  `fillerSetting::getPrefix()` as `prefix`; `width` and `height` are physical
  DBU dimensions.
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
- DePlace retains the active Design and owns Grid, Network, `fillerSetting`,
  PlacementDRC, and the repair engine; PlacementDRC owns the checker.
- `DePlace::initializeFillerRepair(targetMasters)` registers every configured
  filler master plus the complete std-cell target-master universe using the
  real edge table, refreshes matching Nodes as fillers, and publishes the
  complete checker/engine chain before workers start.

Relevant files:

```text
src/dpl2/src/infrastructure/Grid.{h,cpp}
src/dpl2/src/DePlace.cpp
src/dpl2/include/dpl2/DePlace.h
```

The engine never calls `Network::addMaster`; it lacks the real edge table.
Missing configured filler masters therefore make initialization fail, and an
unregistered/post-init target master makes its request fail, instead of either
path creating incomplete metadata.

## 3. Build wiring

Define one parent-owned interface target before adding the payload:

```cmake
add_library(dpl2_filler_repair_deps INTERFACE)
target_link_libraries(dpl2_filler_repair_deps
  INTERFACE <udm-targets> <dpl2-infrastructure> <implant-checker>)

add_subdirectory(<destination>/src/dpl2/src/fillerRepair fillerRepair)
target_link_libraries(<owning-target> PRIVATE dpl2::fillerRepair)
```

The runtime payload requires C++20. Its CMake lists `FillerRetiler.cpp`,
`RepairPlanner.cpp`, and `FillerRepairEngine.cpp`; the destination should link
the target rather than repeat that source list.

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
3. Collect the complete std-cell master universe opto may propose.
4. Call `DePlace::initializeFillerRepair(targetMasters)`. It registers filler
   and target masters with the real edge table, creates `PlacementDRC`, checker,
   and engine, validates readiness, binds the engine, and registers the checker.
5. Start read-only worker calls. Repeated initialization is accepted only when
   the setting and Network master revision are unchanged.

Example caller:

```cpp
// Fixed-origin master swap: request-local candidate, no infrastructure writes.
std::vector<CellChangeRecord> fillerChanges;
if (deplace.isLegal(cellId, newMasterId, fillerChanges)) {
  commitAtomically(targetReplace, fillerChanges);
}

// Opto removes an existing std cell. Repair fills its complete old footprint.
CellChangeRecord targetDelete{OpType::Delete, CellData{cellId}, x, y,
                              oldMasterId, oldMasterId, oldOrientation};
fillerChanges.clear();
if (deplace.repairFillers(targetDelete, fillerChanges)) {
  commitAtomically(targetDelete, fillerChanges);
}

// Opto proposes a new buffer. findLegal chooses a pose, removes covered
// fillers, and refills only the uncovered parts of their old footprints.
CellChangeRecord targetAdd{OpType::Add, CellData{"opto_buffer"}, x, y,
                           invalidLibCell, bufferMasterId, orientation};
fillerChanges.clear();
if (deplace.findLegal(targetAdd, searchDiameter, fillerChanges)) {
  commitAtomically(targetAdd, fillerChanges);
}
```

The checker loads its atomic enable state and published engine pointer once at
request entry. Engine `repair()` methods are const and share state access, so
workers do not serialize; only repository-local snapshot/debug reconfiguration
takes exclusive access.

The input x/y are absolute physical coordinates. Delete/Replace must exactly
match the engine snapshot; `findLegal` may update an Add's x/y/orientation.
Replace
may rotate and/or swap a master only when the footprint is unchanged. Add
deletes all covered fillers and refills any uncovered remainder of their old
footprints; Delete fills the complete old std-cell footprint. Every target
master must be registered before construction. Repair never temporarily
changes the Network Node. The caller owns the target record and result vector
and commits them atomically. If
initialization fails, the engine prints `[fr][engine]` diagnostics by default
and the caller must not bind it. If the design revision changes, stop workers
and rebuild the checker/engine pair; there is no reset/context API.

## 5. Destination assumptions to verify

Before enabling repair on a real design, confirm:

- DePlace's Design, Grid, Network, and `fillerSetting` describe one revision;
  DePlace, checker, and engine outlive all worker calls.
- Network includes every placed/fixed physical object intersecting the core,
  including hard macros; blockages remain Grid state.
- configured filler masters and every opto target-master candidate are present
  in Network with real edge data before checker/engine construction;
- configured target/filler widths are site-aligned and supported target/filler
  heights are one or two logical rows;
- `Node::isFiller()` is correct for placed fillers and replacement masters
  come from the configured filler list;
- every configured filler master exposes its site name and Grid can return the
  orientation for that site at the proposed row/column;
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

Run the same suite with ASan and with `-Wall -Wextra -Werror`; run the shared
checker/engine cases under ThreadSanitizer for destination sign-off. The
portable suite contains:

- database-free planner GoogleTests using seam doubles;
- checker/planner E2E GoogleTests using the real `ImplantLayerChecker` and
  helper-built data, without constructing UDM objects.
- destination checker direct-rule GoogleTests covering one-/two-/three-row
  masters, six implant layers, width/spacing boundaries, and inter-layer
  spacing.

Repository-local full regression:

```sh
ALL=1 src/dpl2/test/build_all.sh
ALL=1 SANITIZE=address src/dpl2/test/build_all.sh
```

This adds fake-UDM engine/infrastructure cases. It validates the local boundary
but does not replace a build and smoke test against the destination's real UDM
and infrastructure.

### Loaded-design command

After loading and importing a fully filled design, the local
`test_filler_repair` command exercises the same checker-facing call boundary
used by opto:

```text
# bounded same-footprint Replace sweep
test_filler_repair

# master swap and/or rotation at the existing origin
test_filler_repair -operation replace -inst <instance> -master <master> \
                   [-orient R0|R180|MX|MY]

# opto removes an existing std cell; repair returns filler Adds
test_filler_repair -operation delete -inst <instance>

# opto adds a buffer at its selected Grid location; repair returns covered
# filler Deletes and any collateral filler Adds/Replaces
test_filler_repair -operation add -master <master> -row <row> -col <column> \
                   [-orient R0|R180|MX|MY]
```

Omitting `-operation` keeps the old `-inst/-master` Replace form. Add derives
orientation from the Grid's row/site data when `-orient` is omitted. Each
targeted invocation validates output record shape and compares pre/post
fingerprints of Network, UDM physical cells, and Grid; any mutation, partial
failure result, or missing required Add/Delete makes the command fail. The
command never commits its target or returned filler records.

## 7. Dump replay

`ImplantLayerCheckerHelper::dump()` writes gzip dump v6. It preserves checker
state, row base polarity, the serializable filler-setting projection, and
master site names for Add-orientation replay. Versions 1-5 remain readable.
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

## 8. Destination performance sign-off

The reproducible repository baseline and its hardware are recorded in
Section 10 of the specification. Do not tune from Debug, ASan, a transcript
enabled with `FR_VERBOSE=1`, or one fresh process per proposal. Build Release,
initialize one checker/engine pair, warm it once, and measure read-only calls
before any commit.

Collect separate p50/p95/p99 distributions for:

- direct legal proposals returning no changes;
- one- and multi-filler VT replacements;
- target Add room creation and target Delete refill, including collateral fill;
- budget-truncated and definitive no-solution cases;
- representative sparse and dense filler rows.

For each slow call retain the structured `[fr]` transcript fields `checker
requests`, `batches`, `cache hits`, adaptive `Grown repair window LN`,
enumeration `coverage`, and retiler `search states`; wall time alone cannot
distinguish checker cost from candidate growth. Section separators identify
each initialization, snapshot, adaptive window, checker gate, and result;
repeated data is rendered as wrapped tables rather than long single lines.
Measure again at 1, 2, 4, and 8 outer workers because the checker also
parallelizes candidates internally. Choose the outer-worker count at the
throughput knee rather than assuming one worker per core is optimal.

Tune in this order: `FR_VERBOSE=0`, caller reuse of the pair, `batchSize`,
per-window/per-repair budgets, then adaptive/subset caps. Change one setting at
a time and rerun determinism, no-partial-result, ASan, and concurrency tests.
Budget reductions may increase safe failures but must never change a failure
into a partial repair. Do not add a cache whose lifetime crosses a database
commit.

The current review found no reason to optimize the exact-cover retiler or
planner container choices first. The measured outlier is the checker-backed
adaptive three-swap stress case, where 1147 candidate checks dominate 84 ms;
planner-only searches remain tens of microseconds. If real designs reproduce
that request pattern, evaluate staged low-order enumeration followed by early
directional growth, with exhaustive fallback preserved for definitive search.

## 9. Final real-design sign-off

Portable tests prove the planner and checker protocol, not the destination's
UDM import. Final sign-off requires one real-design run that confirms:

1. engine initialization succeeds with the expected rows, site width,
   configured masters, and non-empty compatible catalog;
2. a direct legal proposal returns no filler changes;
3. a same-footprint Replace returns checker-accepted filler `Replace` records;
4. deleting a one/two-row std cell returns checker-accepted filler Adds;
5. adding a one/two-row buffer returns all required filler Deletes plus any
   collateral filler Adds with correct orientation;
6. an unrepairable or budget-truncated proposal returns no partial changes;
7. UDM, Grid, and Network are unchanged before caller commit;
8. concurrent read-only checks are clean under the destination sanitizer and
   race-detection setup;
9. post-commit infrastructure synchronization followed by pair reconstruction
   sees the new revision.

Remaining integration risks are limited to destination-specific UDM status
values, Grid occupancy semantics for obstruction-bearing masters, row/frame
conventions, checker registration in the destination's DRC dispatcher, and
real-checker call cost used to tune search budgets.
