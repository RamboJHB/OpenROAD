// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "FillerVtRepair.h"

#include <algorithm>

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

FillerVtRepair::FillerVtRepair(const eUNL::PhysDesMgr& desMgr,
                               const dpl2::Network& network,
                               const ipl::ImplantLayerChecker& checker,
                               const dpl2::fillerSetting& fillerSetting,
                               const FillerVtRepairConfig& config)
    : config_(config), log_(config.verbose)
{
  config_.repair.verbose = config_.repair.verbose || config_.verbose;

  view_ = std::make_unique<InfrastructurePlacementView>(
      desMgr, network, checker, fillerSetting, log_);
  setup_diagnostics_ = view_->setupDiagnostics();
  if (!view_->isValid()) {
    return;
  }
  oracle_ = std::make_unique<CheckerOracleAdapter>(
      checker, *view_, view_->rowHeight(), log_);

  // Default snapshot halo: 2x the max rule query radius (the checker's own
  // neighbor-search radius: max of minValue / |prl| / length per rule).
  ipl::Dbu maxRadius = 0;
  for (const ipl::Rule& rule : checker.rules()) {
    ipl::Dbu radius = rule.minValue;
    if (rule.prl.has_value()) {
      const ipl::Dbu prl = *rule.prl < 0 ? -*rule.prl : *rule.prl;
      radius = std::max(radius, prl);
    }
    if (rule.length.has_value()) {
      radius = std::max(radius, *rule.length);
    }
    maxRadius = std::max(maxRadius, radius);
  }
  default_halo_x_ = static_cast<DbCoord>(2 * maxRadius);

  ready_ = true;
  log_.msg("adapter",
           cat("FillerVtRepair ready: defaultHaloX=", default_halo_x_,
               " siteWidth=", view_->siteWidth(),
               " rowHeight=", view_->rowHeight()));
}

Region FillerVtRepair::snapshotGuard(const TargetPlace& target) const
{
  const MasterInfo* master = view_->masterInfo(target.masterId);
  const DbCoord width = master != nullptr ? master->width : 0;
  const DbCoord heightRows = master != nullptr ? std::max<DbCoord>(master->height, 1) : 1;
  const DbCoord halo =
      config_.snapshotHaloX > 0 ? config_.snapshotHaloX : default_halo_x_;

  Region guard;
  guard.x = XInterval{target.x - halo, target.x + width + halo};

  const std::vector<RowId> rows = view_->rows();
  const RowId minRow = rows.empty() ? 0 : rows.front();
  const RowId maxRow = rows.empty() ? 0 : rows.back();
  guard.rowLo = std::max<RowId>(
      minRow, target.rowId - static_cast<RowId>(config_.snapshotHaloRows));
  guard.rowHi = std::min<RowId>(
      maxRow,
      target.rowId + static_cast<RowId>(heightRows) - 1
          + static_cast<RowId>(config_.snapshotHaloRows));
  return guard;
}

FillerVtRepairResult FillerVtRepair::repair(eUNL::LeafCellID targetCell,
                                            const eLIB::PhysLibCell& newMaster)
{
  FillerVtRepairResult result;
  if (!ready_) {
    result.diagnostics = setup_diagnostics_;
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "AdapterNotReady",
        "infrastructure/checker validation failed -> repair refused"));
    return result;
  }

  // 1) Coverage gate over the same immutable infrastructure snapshot used
  // for candidates and checker-id validation.
  const SiteCoverageResult coverage = view_->checkSiteCoverage(log_);
  if (!coverage.isFullUtility) {
    result.diagnostics = coverage.diagnostics;
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "NonFullUtility",
        cat(coverage.issues.size(),
            " coverage issue(s) -> placement precondition failed")));
    return result;
  }

  // 2) Resolve the target into the checker frame.
  const InstanceId targetId = view_->instanceIdOf(targetCell);
  if (targetId < 0) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "UnknownTarget",
        cat("leaf cell ", static_cast<int>(targetCell.getIndexValue()),
            " is not in the checker's placed set")));
    return result;
  }
  const MasterId newMasterId = view_->masterIdOf(newMaster);
  if (newMasterId < 0) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "TargetMasterNotModeled",
        "the target's new master carries no implant shapes -> the checker "
        "cannot model this place"));
    return result;
  }
  const PlacedInstance* inst =
      view_->instance(static_cast<InstanceId>(targetId));
  if (inst == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "TargetNotPlaced",
        cat("checker instance ", targetId, " missing from the view")));
    return result;
  }

  const MasterInfo* oldMaster = view_->masterInfo(inst->masterId);
  const MasterInfo* replacement = view_->masterInfo(newMasterId);
  if (oldMaster == nullptr || replacement == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "TargetMasterNotModeled",
        "target current or replacement master is absent from the view"));
    return result;
  }
  const DbCoord replacementWidth = newMaster.getWidth().getStorage();
  const DbCoord replacementHeight = std::max<DbCoord>(
      (newMaster.getHeight().getStorage() + view_->rowHeight() - 1)
          / view_->rowHeight(),
      1);
  if (inst->isFiller || oldMaster->isFiller
      || newMaster.getType().isCoreFiller()) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "TargetNotStdCell",
        "target and replacement master must both be standard cells"));
    return result;
  }
  if (oldMaster->width != replacementWidth
      || oldMaster->height != replacementHeight
      || replacement->width != replacementWidth
      || replacement->height != replacementHeight) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "TargetSizeMismatch",
        "target VT replacement must preserve infrastructure width and height"));
    return result;
  }

  TargetPlace target;
  target.instanceId = inst->id;
  target.masterId = static_cast<MasterId>(newMasterId);
  target.rowId = inst->rowId;
  target.x = inst->x;
  target.orientation = inst->orientation;

  // 3) Initial snapshot: the new target place overlaid with ZERO filler
  // changes. Snapshot and all later engine baselines go through the same
  // adapter (duplicate contract).
  OverlayCheckRequest snapshotRequest;
  snapshotRequest.requestId = 0;
  snapshotRequest.targetPlace = target;
  snapshotRequest.guardRegion = snapshotGuard(target);
  log_.msg("adapter",
           cat("snapshot: target inst=", target.instanceId, " newMaster=",
               target.masterId, " guard=", show(snapshotRequest.guardRegion)));
  const CheckResult snapshot = oracle_->checkPlaceWithOverlay(snapshotRequest);
  if (snapshot.status != CheckStatus::Checked) {
    result.diagnostics = snapshot.diagnostics;
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "SnapshotFailed",
        "checker rejected the target-place snapshot request"));
    return result;
  }
  if (snapshot.violations.empty()) {
    result.hasSolution = true;  // legal as-is: empty change list
    result.diagnostics.push_back(makeDiag(
        Severity::Info, "NoRepairNeeded",
        "the new target place is already legal -> no filler changes"));
    return result;
  }

  // 4) Pure planner.
  FillerRepairRequest request;
  request.targetPlace = target;
  request.violations = snapshot.violations;
  FillerRepairEngine engine(*view_, *oracle_, config_.repair);
  FillerRepairResult planned = engine.repair(request);

  result.hasSolution = planned.hasSolution;
  result.diagnostics.insert(result.diagnostics.end(),
                            planned.diagnostics.begin(),
                            planned.diagnostics.end());

  // 5) Map accepted swaps back to UDM handles.
  for (const FillerChange& change : planned.changes) {
    UdmFillerChange out;
    out.instanceId = change.instanceId;
    out.newMasterId = change.newMasterId;
    out.cellId = view_->leafCellOf(change.instanceId);
    out.newMaster = view_->physLibCellOf(change.newMasterId);
    if (!out.cellId.isValid() || out.newMaster == nullptr) {
      // A solution we cannot express in UDM terms is no solution.
      result.hasSolution = false;
      result.changes.clear();
      result.diagnostics.push_back(makeDiag(
          Severity::Fatal, "InfrastructureMappingLost",
          cat("accepted change (inst=", change.instanceId, " -> master=",
              change.newMasterId, ") has no UDM mapping")));
      return result;
    }
    result.changes.push_back(out);
  }
  return result;
}

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
