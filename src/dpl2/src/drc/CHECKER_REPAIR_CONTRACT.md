# ImplantLayerChecker ↔ fillerRepair contract

Updated: 2026-07-23.

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

## 2026-07-20 checker entry wiring

The reserved block in `ImplantLayerChecker::check()` now invokes the owned
`FillerRepairEngine` with the exact `ipl::CheckRequest` built by that method.
This is wiring only; no rule, shape, scan or blocking-violation algorithm was
changed.

The caller constructs one checker and calls `initFillerRepair()`. Before a
target mutation it calls `precheckFillerRepair()`. After `check()` returns true,
`getFillerChanges()` contains either the complete atomic repair or an empty
list when no repair was needed. Every `check()` clears the previous changes
and diagnostics first, so a failed check cannot expose a partial or stale
repair. The caller must read/commit the result before the next check.

After infrastructure synchronizes Network with committed UDM,
`updateFillerRepair()` refreshes both checker snapshots: the engine's private
oracle and the caller-facing checker. It never changes Network Nodes. The
engine retains a direct UDM-handle overload for focused regression tests, but
normal placement checking enters only through `ImplantLayerChecker::check()`.

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

## 2026-07-21 checker metadata refactor

The checker metadata model now uses accessor-based `Layer` and `Rule` classes.
`ImplantLayer`, namespace-level `Family` and namespace-level `Polarity` are
gone; their replacements are `Layer`, `Layer::Vt` and `Layer::Polar`.
`Layer` also carries the originating `TechLayerRelativeID`. `CheckerRect`
remains the checker-internal DBU rectangle, and `ViolationType` is available
for checker-side classification. The violation/result wire consumed by the
planner is unchanged.

fillerRepair now joins physical implant shapes to checker layers with
`Layer::getTechLayerId()` and reads VT/polarity through accessors. It no longer
reconstructs that join from layer names. Portable checker fixtures construct
the new classes and all local checker/engine cases exercise the new boundary.
This is a data-model alignment only; no DRC rule evaluation, scan behavior or
planner algorithm changed.

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
`getFillerPhysCells()` candidates, then constructs the checker. The checker path
uses a request master already present in Network and rebuilds its private
snapshot if that master was added after init. The direct UDM-handle test
overload may validate and register an uninstantiated master. Requests rejected
by target/type/size validation or placement precheck leave the master registry
unchanged. Once registration starts, a later oracle-rebuild failure leaves the
engine fail-closed; Network currently has no transactional master rollback.

After a same-instance-set placement/master commit, infrastructure must
synchronize every affected Network Node from PhysDesMgr. `update()` then
validates that shared revision and atomically replaces only the engine's
private checker/planner/precheck snapshot. New/deleted instances or changes to
rows/blockages require the infrastructure owner to rebuild Grid/Network first.
The snapshot update is mandatory before the next query: local precheck reads
current geometry for indexed nodes, but it cannot discover an object moved in
from a different snapshot row. An unsynchronized Network is rejected and
leaves the engine fail-closed.

The engine serializes its private oracle calls because the current const
overlay path updates internal counters. Calls on the caller-facing checker and
reads of its last result must remain sequential.

## Internal precheck scope

The public `precheckFillerRepair()` is a whole-design gap/overlap gate for
opto. Inside `repair()`, the initial target influence is checked before target
master registration. If adaptive search proposes filler changes in farther
rows, each affected request expands that influence and is checked before the
checker batch; an illegal request does not invalidate legal peers in the same
batch. Legal spans come from the cached Grid domain and placed spans are read
from PhysDesMgr for the snapshot nodes in those rows, keeping work proportional
to the rows touched by repair. Multi-row objects contribute coverage to every
vertically overlapped row. A defect in checked rows returns `PrecheckFailed`
with empty changes; defects elsewhere remain the public global gate's job.

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
DEF/LEF reader is required. The 82 database-free planner tests and their
synthetic doubles are same-level sources under `fillerRepair/test` and migrate with the
feature. Runtime engine coverage, UDM-compatible test data and its provider
remain in the repository-local suite under `src/dpl2/test/local`. The runtime
engine contains no snapshot builder or test conditional. The portable package
passes 116/116; the full 2026-07-21 normal and ASan builds both pass 207/207,
with `-Wall -Wextra -Werror` clean.

Future checker API or semantic changes must be recorded here before engine
changes are merged.
