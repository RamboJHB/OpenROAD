# fillerRepair/adapter — UDM / infra integration layer

Copy-paste-ready integration of the pure repair planner
(`src/dpl2/src/fillerRepair/`) with the real `ipl::ImplantLayerChecker`
(final list-only contract, AGENTS D21) and the dpl2 infrastructure
(`fillerSetting`). **These files are NOT compiled in this repo** — they
include real UDM headers (`phys/physDesMgr.hh`, …) that only exist in the
integration environment. Every point that could not be verified locally is
marked `[VERIFY-UDM]` in the sources and listed below.

## Files / roles

| File | Role |
|---|---|
| `UdmIdBridge` | Replays `initFromUDM`'s enumeration to reconstruct the checker's sequential `InstanceId`/`MasterId` maps (LeafCellID ↔ InstanceId, PhysLibCell/LibCellID ↔ MasterId), row spans, siteWidth/rowHeight, coverage extras. `validate()` cross-checks the reconstruction against `checker.masters()/placedInsts()`. |
| `CheckerPlacementView` | Immutable `PlacementView` snapshot from the checker tables + bridge. Checker frame (x = colId·siteWidth, relative to row origin). VT = implant layer `Family`. Coverage extras appear as synthetic negative-id non-filler instances. |
| `UdmMasterCandidateProvider` | `FillerMasterCandidateProvider` = checker filler masters with known VT, optionally ∩ `fillerSetting::getFillerCells()`. |
| `CheckerOracleAdapter` | `ImplantOverlayChecker` over `checkPlaceWithOverlays` (list-only): DBU↔site conversion, row-region→`Rect` guard, violation enrichment, order-based requestId echo, InvalidOverlay mapping. |
| `UdmPrecheck` | Cached 100%-utility gate (spec #12): same-size swaps never change coverage, so one result survives a whole repair campaign; bump `coverageRevision` only on geometry-changing edits. |
| `FillerVtRepair` | Entry point wiring all of the above; see usage below. |

## Wiring

```cpp
using namespace dpl2::fillerRepair;

// checker: initialized via initFromUDM from the CURRENT design state.
adapter::FillerVtRepairConfig cfg;
cfg.fillerSetting = &ecoFillerSetting;   // optional allow list; nullptr ok
cfg.sharedPrecheck = &campaignPrecheck;  // optional; survives rebuilds
cfg.coverageRevision = coverageRev;      // bump on geometry edits ONLY
cfg.verbose = true;                      // [fr] transcript while debugging

adapter::FillerVtRepair repair(desMgr, checker, cfg);
if (!repair.isReady()) {
  // Bridge fell out of lockstep with initFromUDM -- do not proceed.
  report(repair.setupDiagnostics());
}

// Per opto target, BEFORE committing the target's master change:
auto result = repair.repair(targetLeafCellId, newPhysLibCell);
if (result.hasSolution) {
  // Commit the target change + result.changes atomically (infrastructure
  // owns commit), then update/rebuild the checker and rebuild FillerVtRepair.
}
```

Key semantics:

- **Pre-commit target**: the new master is passed as an overlay; the design
  may still hold the old one. Snapshot, baseline and all candidate checks go
  through the same adapter → the engine's one-to-one multiset delta is safe
  against the checker's per-band/direction duplicates (D21).
- **Rebuild after any commit**: bridge/view/oracle snapshot one design state.
  The shared `UdmPrecheck` is the only thing meant to outlive a rebuild.
- **No design mutation anywhere** in this layer; the result is a list of
  `(LeafCellID, PhysLibCell*)` swaps for the infrastructure to commit.

## [VERIFY-UDM] checklist (for the compiling environment)

1. **Id replay lockstep** (`UdmIdBridge.cpp`): the bridge mirrors
   `initFromUDM` steps 3/5a/5b/6 (same iteration order, same filters,
   including the `hasRect` master filter and *keeping* non-site-aligned
   instances). Run `validate()` on a real design first thing; any report
   means the mirror drifted. Long-term fix: have the checker expose its
   `masterToId` / instance maps and delete the replay.
2. **Row span** (`UdmIdBridge.cpp` rows loop): `[0, bbox width)` from
   `row.getBbox()`; confirm the accessor (`getBbox` vs `getWidth`/site
   count) and that x=0 corresponds to the row origin.
3. **Layer identity by NAME** (`isImplantLayerName`): a tech layer counts as
   an implant layer iff its name appears in `checker.layers()`. Breaks if
   two tech layers share a name — switch to relative-id resolution then.
4. **Per-row origin X**: the checker frame is per-row-relative; the planner
   assumes one shared frame. `validate()` reports when row origins differ.
5. **`eUTL::UvDist` construction** (`CheckerOracleAdapter::toGuardRect`):
   `UvDist(int64_t)` mirrors the checker's own usage; confirm.
6. **Snapshot halo** (`FillerVtRepair`): default = 2× max rule query radius
   (minValue/|prl|/length). Not correctness-critical (the engine re-derives
   windows), but validate against real rule decks; `cfg.snapshotHaloX`
   overrides.
7. **Macros / non-core cells**: coverage extras only cover CORE cells.
   `rowLegalSpan` is the full row bbox, so a macro overlapping a row will
   precheck as a Gap. If real designs have macro cutouts, the row spans need
   refinement (subtract blockages) before this precheck is authoritative.
8. **Exotic orientations**: cells with orientations outside R0/R180/MX/MY
   are skipped by `initFromUDM` and by the bridge → they surface as precheck
   Gaps. That fatal refusal is deliberate (unmodelable occupant), but the
   Gap wording is misleading; improve if it bites.
9. **`LeafCellID().isValid()`**: the bridge returns a default-constructed
   `LeafCellID` for unknown instances and `FillerVtRepair` tests
   `isValid()`; confirm a default id is invalid.
10. **Accessor spellings**: `master.getWidth()/getHeight().getStorage()`,
    `row.getSite().getIsPad()/getWidth()/getHeight()`,
    `physCell.getStatus()/getOrigin()/getOrient()`,
    `setting->getFillerCells()` — all copied from
    `ImplantLayerChecker.cpp` / `fillerSetting.cpp` idioms; fix spellings on
    first compile.
11. **`coverageRevision` source**: use a PhysDesMgr edit counter if one
    exists (over-invalidation is safe); otherwise keep an integration-side
    counter that is NOT bumped by same-size swap commits.
12. **Avoid-abut patterns** (`fillerSetting::isAvoidAbutPattern`): NOT
    applied by the candidate provider — legality is the checker's job. If
    the patterns encode constraints the checker does not model, apply them
    at commit time.

## Build integration

Add to the dpl2 build (real environment only):

```
fillerRepair/adapter/UdmIdBridge.cpp
fillerRepair/adapter/CheckerPlacementView.cpp
fillerRepair/adapter/UdmMasterCandidateProvider.cpp
fillerRepair/adapter/CheckerOracleAdapter.cpp
fillerRepair/adapter/UdmPrecheck.cpp
fillerRepair/adapter/FillerVtRepair.cpp
```

plus the pure planner sources (`fillerRepair/*.cpp`) and the checker.
The planner and its tests keep compiling without UDM — do not add adapter
files to the local test targets.
