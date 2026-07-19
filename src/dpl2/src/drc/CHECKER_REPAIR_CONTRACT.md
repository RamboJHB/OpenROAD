# ImplantLayerChecker ↔ fillerRepair contract

Updated: 2026-07-19.

## Final checker API

```cpp
using FillerChanges = std::vector<FillerCellRecord>;

std::vector<CheckResult> checkPlaceWithOverlays(
    const CheckRequest& target,
    const eUTL::Rect& guard,
    const std::vector<FillerChanges>& candidates) const;
```

One `FillerChanges` is one atomic candidate. Results correlate by input order;
result count must equal candidate count. The checker wire has no request ID or
status enum.

For each batch the checker computes the empty-overlay baseline, evaluates each
candidate, and retains violations touching the target or not contained by the
baseline. `isLegal` is true only when that blocking list and request/overlay
diagnostics are empty. Invalid candidates are isolated.

Persistent init diagnostics (`getDiags()`) are copied into every result --
once by `checkPlaceWithOverlay` and once more inside the embedded
`checkOverlayRegion` result -- and folded into `isLegal`. Before accepting the
checker, the engine classifies that sequence: `skipped_phys_status` and missing
rule parameters on implant layers unused by every Network master are
non-blocking; every other init diagnostic makes `FillerRepairEngine::init()`
fail closed. Once initialization succeeds, the engine strips every leading
repetition of the approved sequence from each result before request-level
classification; a count-based single-prefix strip is wrong.

## IDs and wire

- checker `InstanceId`: `Node::getId()`;
- checker `MasterId`: `Master::getId()`;
- physical cell/master handles: `LeafCellID` / `LibCellID`;
- swap record:
  `FillerCellRecord{Replace, cell_id_, origin_x_, origin_y_,
  orig_lib_cell_, new_lib_cell_}`;
- relationship: `IntraRow` or `InterRow`.

`FillerRepairEngine` converts violation/diagnostic types and synthesizes
planner requestId/status values from ordered results. It does not convert the
change list: planner requests, checker calls and `RepairOutcome` all carry the
same `ipl::FillerChanges`/`FillerCellRecord` records.

## 2026-07-19 fillerRepair wire simplification

No checker source or DRC behavior changed. fillerRepair removed its private,
reduced change-record representation. `PlannerDataSource` now materializes the
exact `FillerCellRecord` above when an overlay is created; the engine
passes that record unchanged to `checkPlaceWithOverlays()` and returns the
accepted records unchanged to opto. This pins all three boundaries to
`new_lib_cell_` and prevents mapping drift between checked and returned data.

## Row/column frames

The checker internally uses TWO frames: `init(desMgr)` and the track pattern
index rows by PhysRow ITERATION order (pad rows included) with x relative to
the row origin, while `scanOverlaySnapshot` resolves committed neighbours and
swapped fillers through `Grid::gridSnapDownY`/`gridX` (non-pad rows sorted by
y, x relative to the core edge). `CheckRequest.rowId/colId` must be supplied
in the iteration frame (it feeds the track pattern and the footprint index).

The chain is consistent only when both frames coincide for every placed node:
no pad row before a standard row, y-sorted row iteration, and the shared row
origin X equal to the core left edge. `FillerRepairEngine::init()` validates
this per node (`RowFrameMismatch`/`ColFrameMismatch` are Fatal) so a design
outside that envelope fails loudly instead of being checked in mixed frames.

## Shared-state requirements

PhysDesMgr, Grid, Network and one engine must describe one design revision.
The engine borrows the initialized Grid/Network, registers all
`getFillerMasters()` candidates, then constructs the checker. If repair first
sees an uninstantiated target master, it registers that master in Network and
rebuilds its private checker/snapshot before issuing the overlay query. Construct a
new engine after commit.

The engine serializes its own checker calls because the current const overlay
path updates internal counters. The checker is engine-owned, so cross-engine
checker aliasing is no longer possible through the runtime API.

## Repair acceptance

The checker returns blocking findings; the engine still applies its
baseline-delta gate. Snapshot, empty-overlay baseline and candidates all use
the same engine/checker path, so this remains consistent. The engine never
reimplements DRC.

## 2026-07-18 checker compatibility edits

No DRC rule or scan behavior changed. Warning-clean integration required only:

- make relationship-name literals `const char*`;
- explicitly mark the currently unused footprint orientation/check y inputs;
- remove an unused local column calculation;
- handle the sentinel `RuleSource::Count` in diagnostic printing.

## Verified test boundary

All portable final-checker calls/assertions live in
`fillerRepair/test/FillerRepairCheckerE2ETest.cpp`. They construct dense
`ImplantInput` directly through `ImplantLayerCheckerHelper` and exercise the
final checker plus `FillerRepairPlanner`; no destination fixture provider or
DEF/LEF reader is required. Public `FillerRepairEngine` and real-UDM engine
coverage remains in the repository-local suite under `src/dpl2/test/local`.
All test doubles and UDM-compatible test data stay outside the migration
payload. The runtime engine contains no snapshot builder or test
conditional. Normal and ASan local builds pass with `-Wall -Wextra -Werror`.

Future checker API or semantic changes must be recorded here before engine
changes are merged.
