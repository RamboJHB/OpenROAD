# ImplantLayerChecker ↔ fillerRepair contract

Updated: 2026-07-18.

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
`checkOverlayRegion` result -- and folded into `isLegal`. The engine boundary
therefore strips EVERY leading repetition of the `getDiags()` sequence from a
result before classifying it; a count-based single-prefix strip is wrong.

## IDs and wire

- checker `InstanceId`: `Node::getId()`;
- checker `MasterId`: `Master::getId()`;
- physical cell/master handles: `LeafCellID` / `LibCellID`;
- swap record:
  `FillerCellRecord{Replace, cell_id_, origin_x_, origin_y_,
  orig_lib_cell_, new_lib_cell_}`;
- relationship: `IntraRow` or `InterRow`.

`FillerRepairEngine` privately converts checker and planner types and
synthesizes planner requestId/status values from ordered results.

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
rebuilds its private checker/view before issuing the overlay query. Construct a
new engine after commit.

The engine serializes its own checker calls because the current const overlay
path updates internal counters. The checker is engine-owned, so cross-engine
checker aliasing is no longer possible through the production API.

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

All final E2E calls/assertions live in the provider-neutral
`fillerRepair/test/e2e_cases.cpp` and use production Grid, Network, final
checker, FillerRepairEngine and planner. Destination real UDM and repository-
local fake UDM only provide fixture data through `E2ETestProvider`; the fake
tree is outside the migration payload. The production engine contains no
snapshot builder or test conditional. Normal and ASan local builds pass with
`-Wall -Wextra -Werror`.

Future checker API or semantic changes must be recorded here before engine
changes are merged.
