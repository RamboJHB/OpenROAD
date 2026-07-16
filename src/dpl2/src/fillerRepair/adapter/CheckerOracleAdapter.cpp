// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "CheckerOracleAdapter.h"

#include <algorithm>

#include "InfrastructurePlacementView.h"  // toUdmOrient

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

namespace {

ViolationKind toKind(ipl::RuleSource source)
{
  return (source == ipl::RuleSource::Width
          || source == ipl::RuleSource::Lef58Width)
             ? ViolationKind::MinWidth
             : ViolationKind::MinSpacing;
}

ViolationRelation toRelation(ipl::Relationship relationship)
{
  switch (relationship) {
    case ipl::Relationship::IntraInstance:
      return ViolationRelation::IntraInstance;
    case ipl::Relationship::InterRow:
      return ViolationRelation::InterRow;
    default:
      return ViolationRelation::IntraRow;
  }
}

// The checker validates candidates individually; an invalid candidate comes
// back as isLegal=false with ONLY diagnostics. Map that shape to
// InvalidOverlay so the engine skips the candidate instead of treating it as
// an inconsistent oracle answer.
CheckStatus statusOf(const ipl::CheckResult& result)
{
  if (!result.isLegal && result.violations.empty()
      && !result.diagnostics.empty()) {
    return CheckStatus::InvalidOverlay;
  }
  return CheckStatus::Checked;
}

}  // namespace

CheckerOracleAdapter::CheckerOracleAdapter(
    const ipl::ImplantLayerChecker& checker,
    const PlacementView& view,
    DbCoord rowHeight,
    const DebugLog& log)
    : checker_(checker), view_(view), row_height_(rowHeight), log_(log)
{
}

ipl::CheckRequest CheckerOracleAdapter::toCheckRequest(
    const TargetPlace& place) const
{
  ipl::CheckRequest request;
  request.instanceId = static_cast<ipl::InstanceId>(place.instanceId);
  request.masterId = static_cast<ipl::MasterId>(place.masterId);
  request.rowId = static_cast<ipl::RowId>(place.rowId);
  // Planner x is DBU in the checker frame; the checker wants site units.
  request.colId = static_cast<ipl::ColId>(place.x / view_.siteWidth());
  request.orientation = toUdmOrient(place.orientation);
  return request;
}

::Rect CheckerOracleAdapter::toGuardRect(const Region& region) const
{
  // y covers rows [rowLo, rowHi]; the -1 keeps a touching adjacent row out
  // (the checker's isInGuard treats touch as inside). [VERIFY-UDM] UvDist
  // construction: the checker cpp itself uses UvDist(int64_t).
  const ipl::Dbu yl = static_cast<ipl::Dbu>(region.rowLo) * row_height_;
  const ipl::Dbu yh =
      static_cast<ipl::Dbu>(region.rowHi + 1) * row_height_ - 1;
  return ::Rect(eUTL::UvDist(static_cast<int64_t>(region.x.xl)),
                eUTL::UvDist(static_cast<int64_t>(yl)),
                eUTL::UvDist(static_cast<int64_t>(region.x.xh)),
                eUTL::UvDist(static_cast<int64_t>(yh)));
}

Violation CheckerOracleAdapter::toPlannerViolation(
    const ipl::Violation& v, InstanceId targetInstance) const
{
  Violation out;
  out.ruleId = v.ruleId;
  out.kind = toKind(v.ruleSource);
  out.relation = toRelation(v.relationship);
  out.primaryLayer = static_cast<LayerId>(v.primaryLayer);
  if (v.secondaryLayer) {
    out.secondaryLayer = static_cast<LayerId>(*v.secondaryLayer);
  }
  out.xWindow = XInterval{static_cast<DbCoord>(v.xWindow.xl),
                          static_cast<DbCoord>(v.xWindow.xh)};
  out.measuredValue = static_cast<DbCoord>(v.measuredValue);
  out.requiredValue = static_cast<DbCoord>(v.requiredValue);
  out.rowIds.reserve(v.rowIds.size());
  for (const ipl::RowId rowId : v.rowIds) {
    out.rowIds.push_back(static_cast<RowId>(rowId));
  }
  // Participants: the checker reports instance ids only; synthesize the rest
  // from the view snapshot. Swapped fillers keep their committed masterId in
  // the view -- the engine matches participants by instanceId, masterId is
  // informational.
  for (const ipl::InstanceId id : v.instances) {
    ViolationParticipant p;
    p.instanceId = static_cast<InstanceId>(id);
    p.isTarget = p.instanceId == targetInstance;
    if (const PlacedInstance* inst = view_.instance(p.instanceId)) {
      p.masterId = inst->masterId;
      p.rowId = inst->rowId;
      const MasterInfo* master = view_.masterInfo(inst->masterId);
      const DbCoord width = master != nullptr ? master->width : 0;
      p.xRange = XInterval{inst->x, inst->x + width};
      p.isFiller = inst->isFiller;
    }
    out.participants.push_back(p);
  }
  return out;
}

CheckResult CheckerOracleAdapter::checkPlaceWithOverlay(
    const OverlayCheckRequest& request)
{
  std::vector<CheckResult> results = checkPlaceWithOverlays({request});
  return results.empty() ? CheckResult{} : std::move(results.front());
}

std::vector<CheckResult> CheckerOracleAdapter::checkPlaceWithOverlays(
    const std::vector<OverlayCheckRequest>& requests)
{
  std::vector<CheckResult> results(requests.size());
  if (requests.empty()) {
    return results;
  }

  // The wire API is one (target, guard) + N candidates, and the engine sends
  // exactly that shape by construction. A mixed batch is a protocol error --
  // reject it loudly instead of quietly splitting it.
  const OverlayCheckRequest& first = requests.front();
  for (const OverlayCheckRequest& request : requests) {
    const bool same =
        request.targetPlace.instanceId == first.targetPlace.instanceId
        && request.targetPlace.masterId == first.targetPlace.masterId
        && request.targetPlace.rowId == first.targetPlace.rowId
        && request.targetPlace.x == first.targetPlace.x
        && request.targetPlace.orientation == first.targetPlace.orientation
        && request.guardRegion.x.xl == first.guardRegion.x.xl
        && request.guardRegion.x.xh == first.guardRegion.x.xh
        && request.guardRegion.rowLo == first.guardRegion.rowLo
        && request.guardRegion.rowHi == first.guardRegion.rowHi;
    if (!same) {
      for (size_t i = 0; i < requests.size(); ++i) {
        results[i].requestId = requests[i].requestId;
        results[i].status = CheckStatus::CheckerError;
        results[i].diagnostics.push_back(makeDiag(
            Severity::Fatal, "CheckerProtocolError",
            "mixed (targetPlace, guardRegion) in one overlay batch"));
      }
      return results;
    }
  }

  const ipl::CheckRequest target = toCheckRequest(first.targetPlace);
  const ::Rect guard = toGuardRect(first.guardRegion);
  std::vector<std::vector<ipl::FillerChange>> changes;
  changes.reserve(requests.size());
  for (const OverlayCheckRequest& request : requests) {
    std::vector<ipl::FillerChange> list;
    list.reserve(request.fillerChanges.size());
    for (const FillerChange& change : request.fillerChanges) {
      list.push_back(ipl::FillerChange{
          static_cast<ipl::InstanceId>(change.instanceId),
          static_cast<ipl::MasterId>(change.newMasterId)});
    }
    changes.push_back(std::move(list));
  }

  std::vector<ipl::CheckResult> raw;
  {
    // Serialize the checker (its const overlay path mutates internal ids).
    std::lock_guard<std::mutex> lock(checker_mutex_);
    raw = checker_.checkPlaceWithOverlays(target, guard, changes);
  }
  log_.msg("adapter",
           cat("overlay batch: ", changes.size(), " candidate(s) -> ",
               raw.size(), " result(s)"));

  for (size_t i = 0; i < requests.size(); ++i) {
    CheckResult& out = results[i];
    // Order IS the correlation (list-only contract); echo the planner id.
    out.requestId = requests[i].requestId;
    if (i >= raw.size()) {
      out.status = CheckStatus::CheckerError;
      out.diagnostics.push_back(makeDiag(
          Severity::Fatal, "CheckerProtocolError",
          cat("checker returned ", raw.size(), " result(s) for ",
              changes.size(), " candidate(s)")));
      continue;
    }
    const ipl::CheckResult& r = raw[i];
    out.status = statusOf(r);
    out.isLegal = r.isLegal;
    for (const ipl::Diagnostic& diag : r.diagnostics) {
      // The checker has no severity; validation failures already flip the
      // status to InvalidOverlay, so Warning keeps classify() usable.
      out.diagnostics.push_back(
          makeDiag(Severity::Warning, diag.status, diag.message));
    }
    out.violations.reserve(r.violations.size());
    for (const ipl::Violation& v : r.violations) {
      out.violations.push_back(
          toPlannerViolation(v, first.targetPlace.instanceId));
    }
  }
  return results;
}

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
