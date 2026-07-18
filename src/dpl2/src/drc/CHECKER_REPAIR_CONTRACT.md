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

## Shared-state requirements

PhysDesMgr, Network, Grid, checker and engine must describe one design
revision. Production `RepairInfrastructure` registers every placed master,
the proposed target master and all `getFillerMasters()` masters before checker
construction. Rebuild the snapshot after commit.

The engine serializes its own checker calls because the current const overlay
path updates internal counters; that mutex is per engine, so pair each engine
with its own checker instance.

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

The final E2E uses fake UDM as data only and production Grid, Network,
RepairInfrastructure, final checker, FillerRepairEngine and planner. Normal
and ASan builds pass with `-Wall -Wextra -Werror`.

Future checker API or semantic changes must be recorded here before engine
changes are merged.
