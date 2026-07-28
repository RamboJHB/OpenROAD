# ImplantLayerChecker ↔ fillerRepair contract

Updated: 2026-07-24.

## Final checker API

```cpp
// infrastructure/Objects.h, namespace dpl2
enum class OpType : uint8_t { Replace = 0, Delete = 1, Add = 2 };
struct FillerCellRecord;

// drc/ImplantLayerChecker.h, namespace dpl2::ipl
using FillerChanges = std::vector<FillerCellRecord>;

std::vector<CheckResult> checkPlaceWithOverlays(
    const CheckRequest& target,
    const eUTL::Rect& guard,
    const std::vector<FillerChanges>& candidates) const;
```

One `FillerChanges` is one atomic candidate. Results correlate by input order;
result count must equal candidate count. `OpType` and `FillerCellRecord` are
owned by `infrastructure/Objects.h`; the checker owns the `FillerChanges`
alias and overlay API. The shared wire has no request ID or status enum.

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
- shared infrastructure record:
  `FillerCellRecord{Replace, cell_id_, origin_x_, origin_y_,
  orig_lib_cell_, new_lib_cell_}`;
- relationship: `IntraRow` or `InterRow`.

`FillerRepairEngine` converts violation/diagnostic types and synthesizes
planner oracle requestId/status values from ordered results. Missing or extra
ordered results invalidate the entire batch and stop the search. It does not
convert the change list: planner requests, checker calls and `RepairOutcome`
all carry the same `ipl::FillerChanges` containing
`dpl2::FillerCellRecord` records.

The planner-only protocol is owned by `OracleGate.h` and is named
`RepairOracle` plus `OracleRequest`/`OracleResult`/`OracleStatus`; these names
are intentionally distinct from final-checker `ImplantLayerChecker` and
`ipl::CheckResult`. Shared planner geometry/model types remain in standalone
`Types.h`. Shared edit records remain in `infrastructure/Objects.h`; checker
source owns only its vector alias and API. Checker DRC behavior is unchanged.

## 2026-07-19 fillerRepair wire simplification

No checker source or DRC behavior changed. fillerRepair removed its private,
reduced change-record representation. `PlacementView` now materializes the
exact infrastructure-owned `FillerCellRecord` above when an overlay is created; the engine
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

## 2026-07-24 engine initialization alignment

No checker source changed. `FillerRepairEngine` now chooses the smallest
non-pad PhysRow site height as its base height and accepts other non-pad site
heights only when they are integer multiples of that base. Placed-instance
filler identity comes from `Node::isFiller()`; the engine's replacement
candidate universe comes from `fillerSetting::getFillerPhysCells()`. A
disagreeing UDM macro-type filler flag therefore no longer blocks engine
initialization.

The checker retains its existing validation. In particular, it may still emit
`replacement_master_not_filler` for a candidate its own metadata rejects. The
engine treats that as a blocking result and returns no partial repair; it does
not reinterpret or bypass checker legality.

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
final checker plus `RepairPlanner`; no destination fixture provider or
DEF/LEF reader is required. The 82 database-free planner tests and their
synthetic doubles are same-level sources under `fillerRepair/test` and migrate with the
feature. Runtime engine coverage, UDM-compatible test data and its provider
remain in the repository-local suite under `src/dpl2/test/local`. The runtime
engine contains no snapshot builder or test conditional. The portable package
passes 153/153; the full 2026-07-24 normal and ASan builds both pass 254/254,
with `-Wall -Wextra -Werror` clean.

Future checker API or semantic changes must be recorded here before engine
changes are merged.

## 2026-07-27 contract: opto-owned fcRecord, lazy engine

Supersedes the 2026-07-20 checker-entry wiring above where they differ.

- The caller (opto) owns a `std::vector<FillerCellRecord>` and passes it by
  reference through `DePlace::isLegal -> checkDRC ->
  ImplantLayerChecker::check(node, x, y, orient, fcRecord)`. On a DRC-illegal
  candidate the checker consults the repair engine; when a checker-verified
  swap set exists, `check()` returns true and APPENDS the records to fcRecord.
  The checker stores no filler-change member; `getFillerChanges()`,
  `initFillerRepair()`, `updateFillerRepair()` and `precheckFillerRepair()`
  no longer exist.
- The engine is created and initialized LAZILY on the first failing check.
  Context: desMgr is remembered by the checker's own `init()`; the
  fillerSetting comes from `setFillerRepairContext()` (harnesses) or the
  provider `DePlace` registers via `setFillerRepairSettingProvider()`
  (dependency inversion: the checker never names DePlace, so builds without
  it still link). A failed lazy init fails closed for the checker's lifetime
  or until a new context is set.
- Global placement precheck is gone from the engine; infrastructure owns
  whole-design placement legality. The engine keeps only the regional
  gap/overlap gate over rows a repair can edit, with per-row legal spans
  derived lazily from Grid pixels.
- Trust-infra: the engine consumes Grid/Network as-is. RowId = Grid row,
  x core-left-relative (the frame `check()` builds requests in). The former
  per-node Network<->UDM cross-validation and PhysRow/Grid frame gates were
  removed: with lazy init the engine runs mid-check while the candidate Node
  already carries its proposed master ahead of the pending UDM commit.
- Filler authority is the UDM master type (checker `isCoreFiller`, Network's
  aligned predicate). The former "Node/allow-list overrides physical type"
  doctrine is gone; `fillerSetting` remains only the replacement-candidate
  allow list.
- [fillerRepair-fix] `checkDirect()` lazily extends `masterItems_` when the
  request master was registered in Network after checker init (the
  `DePlace::isLegal` addMaster-then-check flow); both builders are idempotent
  by master id.

## Verified test boundary (2026-07-28)

Portable planner 82 + portable checker E2E 59 build and run in BOTH harness
modes (fake-UDM and the real-UDM-mode migration gate: 141/141). The fixture
invariants they depend on -- rule/layer ids as container indices, the
band-polarity model, the `maxRuleValue_`-sized snapshot window and min width
-- are documented in `fillerRepair/README.md`; the spacing scenarios were
re-derived against that window (neighbour run within reach, one editable
bridge filler across a sub-minimum gap). Local fake-UDM engine regression:
62 cases. Full local suite 203/203 normal + ASan, gate 141/141 normal + ASan.
