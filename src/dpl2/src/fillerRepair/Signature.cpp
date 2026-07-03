// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "Signature.h"

#include <algorithm>

namespace dpl2::fillerRepair {

namespace {

std::vector<RowId> sortedUniqueRows(const std::vector<RowId>& rows)
{
  std::vector<RowId> result = rows;
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

const char* kindName(ViolationKind kind)
{
  return kind == ViolationKind::MinWidth ? "MW" : "MS";
}

const char* relationName(ViolationRelation relation)
{
  switch (relation) {
    case ViolationRelation::IntraInstance:
      return "intraInst";
    case ViolationRelation::IntraRow:
      return "intraRow";
    case ViolationRelation::InterRow:
      return "interRow";
  }
  return "?";
}

// Distance between two disjoint intervals; <= 0 when they overlap/touch.
DbCoord intervalDistance(const XInterval& a, const XInterval& b)
{
  return std::max(a.xl, b.xl) - std::min(a.xh, b.xh);
}

}  // namespace

std::vector<NormalizedViolation> normalizeViolations(
    const FillerRepairRequest& request,
    const PlacementView& view,
    const DebugLog& log)
{
  std::vector<NormalizedViolation> result;
  result.reserve(request.violations.size());

  for (size_t i = 0; i < request.violations.size(); ++i) {
    const Violation& raw = request.violations[i];
    NormalizedViolation nv;
    nv.raw = raw;

    // Rows: checker-provided, sorted unique; when missing, fall back to the
    // anchor row and flag it (spec 6.2).
    nv.rowIds = sortedUniqueRows(raw.rowIds);
    if (nv.rowIds.empty()) {
      nv.rowIds = {request.targetPlace.rowId};
      nv.rowIdFallback = true;
    }

    // Footprint: xWindow united with every participant's x range.
    nv.xRange = raw.xWindow;
    for (const ViolationParticipant& p : raw.participants) {
      if (!p.xRange.empty()) {
        nv.xRange.xl = std::min(nv.xRange.xl, p.xRange.xl);
        nv.xRange.xh = std::max(nv.xRange.xh, p.xRange.xh);
      }
      if (p.isFiller) {
        nv.fillerParticipants.push_back(p.instanceId);
      } else {
        nv.cellAnchors.push_back(p.instanceId);
      }
    }

    // The changed std cell is always an anchor, participant or not.
    const InstanceId target = request.targetPlace.instanceId;
    if (std::find(nv.cellAnchors.begin(), nv.cellAnchors.end(), target)
        == nv.cellAnchors.end()) {
      nv.cellAnchors.push_back(target);
    }
    std::sort(nv.cellAnchors.begin(), nv.cellAnchors.end());
    std::sort(nv.fillerParticipants.begin(), nv.fillerParticipants.end());

    log.msg("normalize",
            cat("violation#", i, " rule=", raw.ruleId, ' ', kindName(raw.kind),
                '/', relationName(raw.relation), " rows=", nv.rowIds.size(),
                (nv.rowIdFallback ? " (fallback to anchor row)" : ""),
                " xWindow=", show(raw.xWindow), " -> footprint=",
                show(nv.xRange), " anchors=", nv.cellAnchors.size(),
                " fillers=", nv.fillerParticipants.size()));

    result.push_back(std::move(nv));
  }

  (void) view;  // reserved for participant lookups when checker data is thin
  return result;
}

bool sameSignature(const Violation& a, const Violation& b, DbCoord siteWidth)
{
  if (a.ruleId != b.ruleId || a.kind != b.kind || a.relation != b.relation) {
    return false;
  }
  if (sortedUniqueRows(a.rowIds) != sortedUniqueRows(b.rowIds)) {
    return false;
  }

  // xWindow tolerance: overlap of at least half the shorter window, or the
  // windows lie within one site of each other (covers zero-length windows
  // and one-site jitter across snapshots).
  const DbCoord overlap = std::min(a.xWindow.xh, b.xWindow.xh)
                          - std::max(a.xWindow.xl, b.xWindow.xl);
  const DbCoord shorter = std::min(a.xWindow.length(), b.xWindow.length());
  if (overlap * 2 >= shorter && overlap > 0) {
    return true;
  }
  return intervalDistance(a.xWindow, b.xWindow) <= siteWidth;
}

bool isRelatedToOverlay(const Violation& violation,
                        const Overlay& overlay,
                        DbCoord ruleDistance)
{
  for (const Swap& swap : overlay) {
    // Direct participation of a changed instance.
    for (const ViolationParticipant& p : violation.participants) {
      if (p.instanceId == swap.instanceId) {
        return true;
      }
    }
    // Geometric proximity: within one rule distance of the changed span, on
    // the same or a vertically adjacent row (implant rules couple at most
    // adjacent rows).
    bool rowNear = violation.rowIds.empty();  // no row info -> conservative
    for (const RowId row : violation.rowIds) {
      if (row >= swap.rowId - 1 && row <= swap.rowId + 1) {
        rowNear = true;
        break;
      }
    }
    if (rowNear && intervalDistance(violation.xWindow, swap.span) <= ruleDistance) {
      return true;
    }
  }
  return false;
}

DbCoord estimateRuleDistance(const std::vector<Violation>& violations,
                             DbCoord siteWidth)
{
  DbCoord distance = siteWidth;
  for (const Violation& v : violations) {
    distance = std::max(distance, v.requiredValue);
  }
  return distance;
}

}  // namespace dpl2::fillerRepair
