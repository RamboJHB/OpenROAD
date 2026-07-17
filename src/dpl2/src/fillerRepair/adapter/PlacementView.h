// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// THE single boundary between the repair planner and the infrastructure.
//
// One class replaces the former FillerVtRepair / InfrastructurePlacementView
// / CheckerOracleAdapter trio. It depends on Grid + Network (the same
// dependencies as the final ipl::ImplantLayerChecker) plus the PhysDesMgr
// the checker was initialized from, and calls the checker DIRECTLY -- there
// is no separate oracle-adapter object.
//
// Roles of this one object:
//  - fillerRepair::PlacementView: placement/master/candidate queries for the
//    planner, derived from Network/Grid/PhysDesMgr with the checker's exact
//    id and coordinate conventions (InstanceId = Node::getId(), MasterId =
//    Master::getId(), RowId = PhysRow iteration index, x relative to the
//    row origin, colId = x / siteWidth);
//  - fillerRepair::ImplantOverlayChecker: converts planner wire types to the
//    FINAL checker contract -- checkPlaceWithOverlays(request, guard,
//    vector<FillerChanges>) where one candidate is a FillerChanges list of
//    FillerCellRecord{op=Replace, cell_id_(LeafCellID),
//    new_lib_cell_(LibCellID)} -- and back (Relationship has only
//    IntraRow/InterRow);
//  - repair entry: repair(targetCell, newMaster) runs snapshot -> engine ->
//    result, returning ipl::FillerChanges (checker/commit wire form).
//
// Final-contract note (supersedes the old list-only contract): the checker
// computes its own empty-overlay baseline per batch and filters "old"
// violations internally; results are the blocking list per candidate,
// correlated by order. The engine's own baseline-delta gate stays sound
// because snapshot, baseline and candidates all go through this same API.
//
// Thread model: immutable snapshot after construction; const queries safe
// for concurrent readers (coverage cache behind std::call_once). Concurrent
// repairs use one FillerRepairEngine per thread sharing this object; checker
// calls are serialized here (the checker's const overlay path mutates
// internal counters and is NOT thread-safe).
//
// Rebuild after any commit so Grid/Network/checker/this view describe one
// design state.

#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <phys/physDesMgr.hh>

#include "../CheckerApi.h"
#include "../FillerRepairEngine.h"
#include "../Log.h"
#include "../PlacementView.h"
#include "drc/ImplantLayerChecker.h"
#include "infrastructure/fillerSetting.h"
#include "infrastructure/network.h"

namespace dpl2 {
class Grid;

namespace fillerRepair {
namespace adapter {

// Result of one repair in the checker/commit wire form. Commit stays with
// the infrastructure; op is always Replace at this stage (swap-only).
struct RepairOutcome
{
  bool hasSolution = false;
  ipl::FillerChanges changes;
  std::vector<Diagnostic> diagnostics;
};

class PlacementView final : public fillerRepair::PlacementView,
                            public fillerRepair::ImplantOverlayChecker
{
 public:
  struct Config
  {
    RepairConfig repair;
    // Initial-snapshot guard halo in DBU per side; <= 0 derives
    // 2x max(implant WIDTH/SPACING) from the tech (the rule radii the
    // checker builds its rules from). Not correctness-critical: the engine
    // re-derives windows/guards from the snapshot violations.
    DbCoord snapshotHaloX = 0;
    int snapshotHaloRows = 1;
    bool verbose = false;
  };

  // `checker` must have been initialized from the SAME desMgr/network state.
  PlacementView(eUNL::PhysDesMgr* desMgr,
                Grid* grid,
                Network* network,
                const ipl::ImplantLayerChecker* checker,
                const dpl2::fillerSetting* fillerSetting,
                Config config);
  // Default-config overload (a nested-class default argument cannot use the
  // default member initializers inside the enclosing class body).
  PlacementView(eUNL::PhysDesMgr* desMgr,
                Grid* grid,
                Network* network,
                const ipl::ImplantLayerChecker* checker,
                const dpl2::fillerSetting* fillerSetting);

  // False when the infrastructure snapshot failed validation; repair() then
  // refuses with the setup diagnostics.
  bool isReady() const;
  const std::vector<Diagnostic>& setupDiagnostics() const
  {
    return setup_diagnostics_;
  }

  // `targetCell` = the opto-changed std cell; `newMaster` = its proposed new
  // master. Pre-commit semantics: the new master rides as an overlay, the
  // committed design may still hold the old one. Pure -- no design mutation.
  RepairOutcome repair(eUNL::LeafCellID targetCell,
                       const eLIB::PhysLibCell& newMaster);

  // --- fillerRepair::PlacementView -----------------------------------------
  const std::vector<RowId>& rows() const override { return row_list_; }
  XInterval rowLegalSpan(RowId rowId) const override;
  DbCoord siteWidth() const override { return site_width_; }
  const std::vector<PlacedInstance>& instancesInRow(RowId rowId) const override;
  const PlacedInstance* instance(InstanceId id) const override;
  const MasterInfo* masterInfo(MasterId id) const override;
  const std::vector<MasterId>& fillerMasterIds() const override
  {
    return filler_master_ids_;
  }
  SiteCoverageResult checkSiteCoverage(const DebugLog& log) const override;

  // --- fillerRepair::ImplantOverlayChecker (direct ipl checker calls) ------
  CheckResult checkPlaceWithOverlay(const OverlayCheckRequest& request) override;
  std::vector<CheckResult> checkPlaceWithOverlays(
      const std::vector<OverlayCheckRequest>& requests) override;

  DbCoord rowHeight() const { return row_height_; }

 private:
  void addProblem(Severity severity,
                  const std::string& code,
                  const std::string& message);
  ipl::CheckRequest toCheckRequest(const TargetPlace& place) const;
  ::Rect toGuardRect(const Region& region) const;
  Violation toPlannerViolation(const ipl::Violation& v,
                               InstanceId targetInstance) const;
  FillerCellRecord toFillerCellRecord(const FillerChange& change) const;
  Region snapshotGuard(const TargetPlace& target) const;

  Network* network_ = nullptr;
  const ipl::ImplantLayerChecker* checker_ = nullptr;
  Config config_;
  DebugLog log_;

  DbCoord site_width_ = 0;
  DbCoord row_height_ = 0;
  DbCoord default_halo_x_ = 0;
  std::map<RowId, XInterval> row_spans_;
  std::vector<RowId> row_list_;  // non-pad rows, ascending
  std::map<MasterId, MasterInfo> masters_;
  std::map<InstanceId, PlacedInstance> instances_;
  // Per instance: UDM handles + origin needed for FillerCellRecord.
  struct UdmRef
  {
    eUNL::LeafCellID cellId;
    eLIB::LibCellID libCellId;   // current master
    eUTL::UvDist originX;
    eUTL::UvDist originY;
  };
  std::map<InstanceId, UdmRef> udm_refs_;
  std::map<MasterId, eLIB::LibCellID> master_lib_ids_;
  std::map<RowId, std::vector<PlacedInstance>> by_row_;
  std::vector<MasterId> filler_master_ids_;
  std::vector<Diagnostic> setup_diagnostics_;

  mutable std::once_flag coverage_once_;
  mutable SiteCoverageResult coverage_cache_;
  // Serializes ipl checker calls (its const overlay path is not
  // thread-safe); planner side stays lock-free.
  mutable std::mutex checker_mutex_;
};

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
