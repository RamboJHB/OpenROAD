// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "Preflight.h"

#include <algorithm>

namespace dpl2::fillerRepair {

namespace {

const char* kindName(CoverageIssueKind kind)
{
  switch (kind) {
    case CoverageIssueKind::Gap:
      return "Gap";
    case CoverageIssueKind::Overlap:
      return "Overlap";
    case CoverageIssueKind::OffGrid:
      return "OffGrid";
    case CoverageIssueKind::IllegalOccupant:
      return "IllegalOccupant";
  }
  return "?";
}

}  // namespace

SiteCoverageResult runUtilityPreflight(const PlacementView& view,
                                       const DebugLog& log)
{
  SiteCoverageResult result;
  const DbCoord siteWidth = std::max<DbCoord>(view.siteWidth(), 1);

  const auto addIssue = [&](CoverageIssueKind kind,
                            RowId rowId,
                            DbCoord xLo,
                            DbCoord xHi,
                            std::vector<InstanceId> instances) {
    CoverageIssue issue;
    issue.kind = kind;
    issue.rowId = rowId;
    issue.xLo = xLo;
    issue.xHi = xHi;
    issue.siteCount = static_cast<int>((xHi - xLo) / siteWidth);
    issue.instances = std::move(instances);
    log.msg("preflight",
            cat(kindName(kind), " row=", rowId, " x=", show(XInterval{xLo, xHi}),
                " sites=", issue.siteCount, " -> precondition failure"));
    result.issues.push_back(std::move(issue));
  };

  for (const RowId rowId : view.rows()) {
    const XInterval legal = view.rowLegalSpan(rowId);
    // cursor = end of covered prefix; every instance must continue exactly
    // at cursor, otherwise the difference is a gap or an overlap.
    DbCoord cursor = legal.xl;

    for (const PlacedInstance& inst : view.instancesInRow(rowId)) {
      const XInterval span = instanceSpan(view, inst);

      if ((span.xl - legal.xl) % siteWidth != 0) {
        addIssue(CoverageIssueKind::OffGrid, rowId, span.xl, span.xh, {inst.id});
      }
      if (span.xl < legal.xl || span.xh > legal.xh) {
        addIssue(CoverageIssueKind::IllegalOccupant, rowId, span.xl, span.xh,
                 {inst.id});
      }
      if (span.xl > cursor) {
        addIssue(CoverageIssueKind::Gap, rowId, cursor, span.xl, {});
      } else if (span.xl < cursor) {
        addIssue(CoverageIssueKind::Overlap, rowId, span.xl,
                 std::min(cursor, span.xh), {inst.id});
      }
      cursor = std::max(cursor, span.xh);
    }

    if (cursor < legal.xh) {
      addIssue(CoverageIssueKind::Gap, rowId, cursor, legal.xh, {});
    }
  }

  result.isFullUtility = result.issues.empty();
  if (result.isFullUtility) {
    log.msg("preflight", "all rows fully covered -> 100% utility OK");
  } else {
    for (const CoverageIssue& issue : result.issues) {
      result.diagnostics.push_back(makeDiag(
          Severity::Error,
          "NonFullUtility",
          cat(kindName(issue.kind), " row=", issue.rowId, " x=",
              show(XInterval{issue.xLo, issue.xHi}), " sites=", issue.siteCount)));
    }
  }
  return result;
}

}  // namespace dpl2::fillerRepair
