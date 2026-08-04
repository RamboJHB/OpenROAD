# fillerRepair — checker-guided filler VT repair

Updated: 2026-08-04.

## Purpose

When opto proposes a same-size VT master change for one standard cell, nearby
fillers may leave an illegal implant width or spacing. This module searches for
same-size filler master swaps and accepts a set only when the existing
`ImplantLayerChecker` verifies the complete overlay.

The module is pre-commit and non-mutating. It returns
`ipl::FillerChanges`/`CellChangeRecord`; opto and infrastructure own commit.

## Runtime boundary

The caller reaches repair through the existing checker:

```cpp
bool ImplantLayerChecker::check(
    const Node* node,
    GridX x,
    GridY y,
    const PhysOrientation& orient,
    std::vector<CellChangeRecord>& filler_changes) const;
```

The checker runs `checkDirect()` first. On failure it constructs a fresh
engine, initializes it from the current committed placement, and calls:

```cpp
FillerRepairEngine(Grid*, Network*);
bool init(PhysDesMgr*, const fillerSetting&);
RepairOutcome repair(const ipl::CheckRequest&);
```

The fresh engine prevents stale placement state after an earlier opto overlay
or commit. There is no public adapter, raw-UDM repair overload, update method,
or global precheck API.

## Ownership and responsibilities

`FillerRepairEngine` owns its runtime state directly:

- `PlacementSnapshot`: committed UDM/Grid/Network data projected into planner
  types;
- `FillerCandidateCatalog`: compatible replacement ids, precomputed once;
- one private `ImplantLayerChecker`, used directly as the legality oracle.

`internal::RepairPlanner` owns only the search:

1. normalize the initial violations;
2. construct L0 and adaptive-L1 windows;
3. generate same-size, different-known-VT swaps;
4. rank fillers while retaining every compatible master in each domain;
5. enumerate atomic subsets under budgets;
6. apply the baseline-delta gate to ordered checker results;
7. return the first clean overlay or a safe empty failure.

The planner sees exactly two seams:

- `PlacementView` for rows, instances, masters and change records;
- `RepairOracle` for batched overlay checks.

`RepairTypes.h` remains the shared leaf vocabulary. It also references the one
infrastructure-owned `CellChangeRecord`; the module does not define a second
change representation.

## Master and filler contract

Infrastructure must register masters with its real edge table before repair.
The engine never creates a Network Master.

During `init()`, `ensureMasterRegistered()` performs only:

```cpp
Master* master = network->getMaster(lib_cell_id);
if (master != nullptr) {
  master->setFiller(true);
}
```

A configured master missing from Network is a fatal initialization error.
The target master id in `CheckRequest` must also resolve consistently in
Network and in the engine snapshot.

Candidate identity comes from `fillerSetting::getFillerPhysCells()`; placed
identity comes from `Node::isFiller()`. UDM macro-type filler flags do not
override either source.

## Placement and geometry contract

- Row id and column are the Grid frame used by the checker.
- The committed physical master is read from `PhysCell`, then mapped back to
  Network. A transient candidate already stored on `Node` is not copied into
  the committed snapshot.
- Base row height is the smallest positive non-pad Grid row-frame height.
  Taller rows must be integer multiples of that base.
- Repair supports only the same row, x, orientation, width and height.
- The regional placement gate requires each legal Grid segment in affected
  rows to be covered exactly once. Legal blockages/halos/fragment gaps are not
  required coverage.

The horizontal guard includes the exact checker reach. Adaptive growth also
supports sparse filler layouts: it skips standard cells, reaches a filler
within oracle reach, and seeds fillers reported as blocking participants. A
filler outside current reach can still enter through later adaptive levels.

## Correctness gate

For one guard, the engine first records a baseline result. Each candidate must:

- eliminate every original violation;
- introduce no violation inside the editable window;
- introduce no new violation in the guard related to the target or its swaps;
- preserve only unrelated pre-existing guard findings;
- satisfy checker result-count/order and legality/violation consistency.

Baseline and candidate findings are compared as multisets. Cache keys are
canonicalized filler-change sets. Budget exhaustion, invalid protocol, missing
metadata, or no solution always returns empty changes; partial repair is never
published.

## Source layout

| File | Role |
|---|---|
| `FillerRepairEngine.{h,cpp}` | runtime snapshot, validation, candidate catalog and checker boundary |
| `RepairPlanner.{h,cpp}` | deterministic adaptive search and oracle gate |
| `PlacementView.h` | read-only placement seam |
| `RepairOracle.h` | ordered batch checker seam |
| `RepairTypes.h` | shared planner geometry/model/wire helpers |
| `Debug.h` | opt-in `[fr][stage]` logging |
| `test/RepairPlannerTest.cpp` | 93 planner GoogleTests |
| `test/FillerRepairCheckerE2ETest.cpp` | 83 real-checker GoogleTests |

`fillerRepair2/` is the copy-only runtime directory. Its eight runtime files
are byte-identical to this directory and guarded by a configure-time SHA-256
check.

## Build

The destination supplies one dependency target, adds this directory, and links
the module target:

```cmake
add_library(dpl2_filler_repair_deps INTERFACE)
target_link_libraries(dpl2_filler_repair_deps
  INTERFACE <udm> <infrastructure-and-checker>)
add_subdirectory(<srcroot>/fillerRepair fillerRepair)
target_link_libraries(<owner> PRIVATE dpl2::fillerRepair)
```

`dpl2::fillerRepair` is C++20. The separate
`dpl2::fillerRepairPlanner` compile target is held to C++17 as a layering gate.
All module and test targets use `-Wall -Wextra -Werror`.

## Debugging and tuning

Logging is off by default. Set `FR_VERBOSE=1` to print stage, window, candidate,
batch, cache, budget and diagnostic details. `setDebugLogging()` is available
for a scoped override in a harness.

The default budgets and batch size bound effort only. Do not tune them from
synthetic checker timings; collect real-design checker calls, wall time and
adaptive-level histograms first.

## Verification

Current local total: 287/287 (93 planner, 83 checker, 111 fake-UDM chain).

```sh
ALL=1 src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address ALL=1 src/dpl2/test/local/run_fake_udm_e2e.sh
src/dpl2/test/local/run_migration_gate.sh
SANITIZE=address src/dpl2/test/local/run_migration_gate.sh
```

See `test/README.md` for case coverage and `../../HandOff.md` for destination
wiring and remaining integration checks.
