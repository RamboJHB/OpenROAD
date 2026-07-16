// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "FakeImplantChecker.h"

#include <algorithm>

#include "../Log.h"

namespace dpl2::fillerRepair {

namespace {

ViolationParticipant participantOf(const PlacementView& view,
                                   const PlacedInstance& inst,
                                   const TargetPlace& target)
{
  ViolationParticipant p;
  p.instanceId = inst.id;
  p.masterId = inst.masterId;
  p.rowId = inst.rowId;
  p.xRange = instanceSpan(view, inst);
  p.isFiller = inst.isFiller;
  p.isTarget = inst.id == target.instanceId;
  return p;
}

// Collected iff it intersects the guard region (any touched row inside the
// row range, and x windows overlapping).
bool inGuardRegion(const Violation& v, const Region& region)
{
  if (!v.xWindow.overlaps(region.x)) {
    return false;
  }
  for (const RowId row : v.rowIds) {
    if (region.containsRow(row)) {
      return true;
    }
  }
  return false;
}

}  // namespace

CheckResult FakeImplantChecker::checkPlaceWithOverlay(
    const OverlayCheckRequest& request)
{
  ++request_count_;
  return evaluate(request);
}

std::vector<CheckResult> FakeImplantChecker::checkPlaceWithOverlays(
    const std::vector<OverlayCheckRequest>& requests)
{
  ++batch_count_;
  std::vector<CheckResult> results;
  results.reserve(requests.size());
  for (const OverlayCheckRequest& request : requests) {
    ++request_count_;
    // Each request is evaluated independently: an invalid overlay produces
    // its own InvalidOverlay result and cannot leak into its neighbors.
    results.push_back(evaluate(request));
  }
  return results;
}

MasterId FakeImplantChecker::effectiveMaster(
    const PlacedInstance& inst,
    const OverlayCheckRequest& request,
    const std::map<InstanceId, MasterId>& overlay) const
{
  const auto it = overlay.find(inst.id);
  if (it != overlay.end()) {
    return it->second;
  }
  if (inst.id == request.targetPlace.instanceId) {
    return request.targetPlace.masterId;
  }
  return inst.masterId;
}

std::vector<FakeImplantChecker::Run> FakeImplantChecker::buildRuns(
    RowId rowId,
    const OverlayCheckRequest& request,
    const std::map<InstanceId, MasterId>& overlay) const
{
  std::vector<Run> runs;
  for (const PlacedInstance& placed : design_.instancesInRow(rowId)) {
    const PlacedInstance* inst = design_.instance(placed.id);
    const MasterInfo* master = design_.masterInfo(effectiveMaster(placed, request, overlay));
    const XInterval span = XInterval{placed.x, placed.x + master->width};
    // Extend the previous run only when same VT and x-contiguous; a gap in
    // coverage (illegal design, pre-check territory) breaks the run.
    if (!runs.empty() && runs.back().vt == master->vt
        && runs.back().span.xh == span.xl) {
      runs.back().span.xh = span.xh;
      runs.back().insts.push_back(inst);
    } else {
      runs.push_back(Run{master->vt, span, {inst}});
    }
  }
  return runs;
}

CheckResult FakeImplantChecker::evaluate(const OverlayCheckRequest& request) const
{
  CheckResult result;
  result.requestId = request.requestId;  // echo, always

  // --- Request validation (one atomic overlay). Any defect makes only this
  // request InvalidOverlay, with diagnostics as the protocol demands.
  std::map<InstanceId, MasterId> overlay;
  const auto invalid = [&](std::string why) {
    result.status = CheckStatus::InvalidOverlay;
    result.diagnostics.push_back(
        makeDiag(Severity::Error, "InvalidOverlay", std::move(why)));
    return result;
  };

  if (design_.instance(request.targetPlace.instanceId) == nullptr) {
    return invalid(cat("target instance ", request.targetPlace.instanceId,
                       " not found"));
  }
  for (const FillerChange& change : request.fillerChanges) {
    const PlacedInstance* inst = design_.instance(change.instanceId);
    if (inst == nullptr) {
      return invalid(cat("instance ", change.instanceId, " not found"));
    }
    if (!inst->isFiller) {
      return invalid(cat("instance ", change.instanceId, " is not a filler"));
    }
    const MasterInfo* oldMaster = design_.masterInfo(inst->masterId);
    const MasterInfo* newMaster = design_.masterInfo(change.newMasterId);
    if (newMaster == nullptr || !newMaster->isFiller) {
      return invalid(cat("master ", change.newMasterId, " unknown or not a filler"));
    }
    if (newMaster->width != oldMaster->width
        || newMaster->height != oldMaster->height) {
      return invalid(cat("size mismatch for instance ", change.instanceId,
                         ": new master ", change.newMasterId));
    }
    if (!overlay.emplace(change.instanceId, change.newMasterId).second) {
      return invalid(cat("duplicate instance ", change.instanceId,
                         " in one overlay"));
    }
  }

  // --- Rule evaluation over the overlaid design.
  const std::vector<RowId>& rowIds = design_.rows();
  std::map<RowId, std::vector<Run>> runsByRow;
  for (const RowId rowId : rowIds) {
    runsByRow[rowId] = buildRuns(rowId, request, overlay);
  }

  const auto addViolation = [&](Violation v) {
    if (inGuardRegion(v, request.guardRegion)) {
      result.violations.push_back(std::move(v));
    }
  };
  const auto participants = [&](const Run& run) {
    std::vector<ViolationParticipant> ps;
    for (const PlacedInstance* inst : run.insts) {
      ps.push_back(participantOf(design_, *inst, request.targetPlace));
    }
    return ps;
  };

  for (const RowId rowId : rowIds) {
    const std::vector<Run>& runs = runsByRow[rowId];

    // Intra-row MW (ruleId 1): every run must reach mwIntra.
    if (rules_.mwIntra > 0) {
      for (const Run& run : runs) {
        if (run.span.length() < rules_.mwIntra) {
          Violation v;
          v.ruleId = 1;
          v.kind = ViolationKind::MinWidth;
          v.relation = ViolationRelation::IntraRow;
          v.primaryLayer = run.vt;
          v.rowIds = {rowId};
          v.xWindow = run.span;
          v.measuredValue = run.span.length();
          v.requiredValue = rules_.mwIntra;
          v.participants = participants(run);
          addViolation(std::move(v));
        }
      }
    }

    // Intra-row MS (ruleId 2): distance between consecutive same-VT runs.
    if (rules_.msIntra > 0) {
      for (size_t i = 0; i < runs.size(); ++i) {
        for (size_t j = i + 1; j < runs.size(); ++j) {
          if (runs[j].vt != runs[i].vt) {
            continue;
          }
          const DbCoord gap = runs[j].span.xl - runs[i].span.xh;
          if (gap > 0 && gap < rules_.msIntra) {
            Violation v;
            v.ruleId = 2;
            v.kind = ViolationKind::MinSpacing;
            v.relation = ViolationRelation::IntraRow;
            v.primaryLayer = runs[i].vt;
            v.rowIds = {rowId};
            v.xWindow = XInterval{runs[i].span.xh, runs[j].span.xl};
            v.measuredValue = gap;
            v.requiredValue = rules_.msIntra;
            v.participants = participants(runs[i]);
            const auto more = participants(runs[j]);
            v.participants.insert(v.participants.end(), more.begin(), more.end());
            addViolation(std::move(v));
          }
          break;  // only the nearest same-VT run to the right matters
        }
      }
    }
  }

  // Inter-row rules between vertically adjacent rows.
  for (size_t r = 0; r + 1 < rowIds.size(); ++r) {
    const RowId rowA = rowIds[r];
    const RowId rowB = rowIds[r + 1];
    for (const Run& a : runsByRow[rowA]) {
      for (const Run& b : runsByRow[rowB]) {
        if (a.vt != b.vt) {
          continue;
        }
        const DbCoord overlap = std::min(a.span.xh, b.span.xh)
                                - std::max(a.span.xl, b.span.xl);
        if (overlap > 0 && rules_.mwInter > 0 && overlap < rules_.mwInter) {
          // Inter-row MW (ruleId 3): merged shape too narrow at the row
          // boundary.
          Violation v;
          v.ruleId = 3;
          v.kind = ViolationKind::MinWidth;
          v.relation = ViolationRelation::InterRow;
          v.primaryLayer = a.vt;
          v.rowIds = {rowA, rowB};
          v.xWindow = XInterval{std::max(a.span.xl, b.span.xl),
                                std::min(a.span.xh, b.span.xh)};
          v.measuredValue = overlap;
          v.requiredValue = rules_.mwInter;
          v.participants = participants(a);
          const auto more = participants(b);
          v.participants.insert(v.participants.end(), more.begin(), more.end());
          addViolation(std::move(v));
        } else if (overlap <= 0 && rules_.msInter > 0
                   && -overlap < rules_.msInter) {
          // Inter-row MS (ruleId 4): disjoint same-VT shapes too close
          // (corner touch = distance 0 counts).
          const DbCoord dist = -overlap;
          Violation v;
          v.ruleId = 4;
          v.kind = ViolationKind::MinSpacing;
          v.relation = ViolationRelation::InterRow;
          v.primaryLayer = a.vt;
          v.rowIds = {rowA, rowB};
          const DbCoord lo = std::min(a.span.xh, b.span.xh);
          v.xWindow = XInterval{lo, lo + std::max<DbCoord>(dist, 1)};
          v.measuredValue = dist;
          v.requiredValue = rules_.msInter;
          v.participants = participants(a);
          const auto more = participants(b);
          v.participants.insert(v.participants.end(), more.begin(), more.end());
          addViolation(std::move(v));
        }
      }
    }
  }

  result.status = CheckStatus::Checked;
  result.isLegal = result.violations.empty();
  return result;
}

}  // namespace dpl2::fillerRepair
