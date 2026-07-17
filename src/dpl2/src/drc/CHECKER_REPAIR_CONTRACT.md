# ImplantLayerChecker ↔ fillerRepair contract

Updated: 2026-07-18. This supersedes the retired list-only/raw contract.

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

## IDs and wire

- checker `InstanceId`: `Node::getId()`;
- checker `MasterId`: `Master::getId()`;
- physical cell/master handles: `LeafCellID` / `LibCellID`;
- swap record:
  `FillerCellRecord{Replace, cell_id_, origin_x_, origin_y_,
  orig_lib_cell_, new_lib_cell_}`;
- relationship: `IntraRow` or `InterRow`.

The unified adapter explicitly converts checker and planner types and
synthesizes planner requestId/status values from ordered results.

## Shared-state requirements

PhysDesMgr, Network, Grid, checker and adapter must describe one design
revision. Production `RepairInfrastructure` registers every placed master,
the proposed target master and all `getFillerMasters()` masters before checker
construction. Rebuild the snapshot after commit.

The adapter serializes checker calls because the current const overlay path
updates internal counters.

## Repair acceptance

The checker returns blocking findings; the engine still applies its
baseline-delta gate. Snapshot, empty-overlay baseline and candidates all use
the same adapter/checker path, so this remains consistent. The engine never
reimplements DRC.

## 2026-07-18 checker compatibility edits

No DRC rule or scan behavior changed. Warning-clean integration required only:

- make relationship-name literals `const char*`;
- explicitly mark the currently unused footprint orientation/check y inputs;
- remove an unused local column calculation;
- handle the sentinel `RuleSource::Count` in diagnostic printing.

## Verified test boundary

The final E2E uses fake UDM as data only and production Grid, Network,
RepairInfrastructure, final checker, unified adapter and planner. Grid stubs,
RD Helper injection and fake PlacementDRC are not linked. Normal and ASan
builds pass with `-Wall -Wextra -Werror`.

Future checker API or semantic changes must be recorded here before adapter
changes are merged.
