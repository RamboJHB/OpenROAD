# ImplantLayerChecker ↔ fillerRepair contract

Updated: 2026-07-20.

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
planner oracle requestId/status values from ordered results. Missing or extra
ordered results invalidate the entire batch and stop the search. It does not
convert the change list: planner requests, checker calls and `RepairOutcome`
all carry the same `ipl::FillerChanges`/`FillerCellRecord` records.

The planner-only protocol is owned by `OracleGate.h` and is named
`PlannerOracle` plus `OracleRequest`/`OracleResult`/`OracleStatus`; these names
are intentionally distinct from final-checker `ImplantLayerChecker` and
`ipl::CheckResult`. Shared planner geometry/model types remain in standalone
`Types.h`. This is a fillerRepair-only ownership cleanup; checker source and
DRC behavior are unchanged.

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
Network must contain every placed/fixed physical instance that can intersect
the core, including hard macros. Placement blockages remain Grid state and are
not Network Nodes. The engine borrows the initialized Grid/Network, registers all
`getFillerMasters()` candidates, then constructs the checker. If repair first
sees an uninstantiated target master, it validates target/type/dimensions before
registering that master in Network and rebuilding its private checker/snapshot.
Rejected requests leave the master registry unchanged.

After a same-instance-set placement/master commit, `update()` refreshes every
existing Network Node from PhysDesMgr and atomically replaces the engine's
private checker/planner/precheck snapshot. New/deleted instances or changes to
rows/blockages require the infrastructure owner to rebuild Grid/Network first.

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

## Checker RD request (implementation held)

fillerRepair currently classifies persistent checker initialization diagnostics
using status/message content. An unrecognized diagnostic is treated as blocking,
which makes `FillerRepairEngine::init()` or `update()` fail closed; it does not
reject Grid, Network or the surrounding OpenROAD initialization.

Checker RD should expose a structured initialization disposition, for example a
severity/category field or an `isReady()` plus per-diagnostic blocking flag. It
must distinguish structural failures from approved persistent warnings such as
unsupported physical status and missing rules on implant layers unused by all
Network masters. Until RD supplies and approves that API, checker code and DRC
behavior remain unchanged and the existing fail-closed translation stays in the
engine.

## Verified test boundary

All portable final-checker calls/assertions live in
`fillerRepair/test/FillerRepairCheckerE2ETest.cpp`. They construct dense
`ImplantInput` directly through `ImplantLayerCheckerHelper` and exercise the
final checker plus `FillerRepairPlanner`; no destination fixture provider or
DEF/LEF reader is required. Public `FillerRepairEngine` and real-UDM engine
coverage remains in the repository-local suite under `src/dpl2/test/local`.
All test doubles and UDM-compatible test data stay outside the migration
payload. The runtime engine contains no snapshot builder or test
conditional. The 2026-07-20 normal build passes 198/198 with
`-Wall -Wextra -Werror`; ASan has not been rerun after the update changes.

Future checker API or semantic changes must be recorded here before engine
changes are merged.
