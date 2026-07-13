// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "PreCheck.h"

#include <algorithm>
#include <tuple>

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

SiteCoverageResult runUtilityPreCheck(const PlacementView& view,
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
    result.issues.push_back(std::move(issue));
  };

  for (const RowId rowId : view.rows()) {
    const XInterval legal = view.rowLegalSpan(rowId);
    const std::vector<PlacedInstance> instances = view.instancesInRow(rowId);
    std::vector<XInterval> clippedSpans;
    clippedSpans.reserve(instances.size());
    std::vector<DbCoord> cuts{legal.xl, legal.xh};

    for (const PlacedInstance& inst : instances) {
      const XInterval span = instanceSpan(view, inst);

      if ((span.xl - legal.xl) % siteWidth != 0) {
        addIssue(CoverageIssueKind::OffGrid, rowId, span.xl, span.xh, {inst.id});
      }
      if (span.xl < legal.xl || span.xh > legal.xh) {
        addIssue(CoverageIssueKind::IllegalOccupant, rowId, span.xl, span.xh,
                 {inst.id});
      }

      const XInterval clipped{std::max(span.xl, legal.xl),
                              std::min(span.xh, legal.xh)};
      clippedSpans.push_back(clipped);
      if (!clipped.empty()) {
        cuts.push_back(clipped.xl);
        cuts.push_back(clipped.xh);
      }
    }

    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
      const XInterval segment{cuts[i], cuts[i + 1]};
      if (segment.empty()) {
        continue;
      }
      std::vector<InstanceId> active;
      for (size_t j = 0; j < instances.size(); ++j) {
        if (clippedSpans[j].overlaps(segment)) {
          active.push_back(instances[j].id);
        }
      }
      std::sort(active.begin(), active.end());
      if (active.empty()) {
        addIssue(CoverageIssueKind::Gap, rowId, segment.xl, segment.xh, {});
      } else if (active.size() > 1) {
        addIssue(CoverageIssueKind::Overlap,
                 rowId,
                 segment.xl,
                 segment.xh,
                 std::move(active));
      }
    }
  }

  std::sort(result.issues.begin(),
            result.issues.end(),
            [](const CoverageIssue& a, const CoverageIssue& b) {
              return std::tie(a.rowId, a.xLo, a.kind, a.xHi, a.instances)
                     < std::tie(b.rowId, b.xLo, b.kind, b.xHi, b.instances);
            });

  std::vector<CoverageIssue> merged;
  for (CoverageIssue& issue : result.issues) {
    if (!merged.empty() && merged.back().kind == issue.kind
        && merged.back().rowId == issue.rowId
        && merged.back().xHi == issue.xLo
        && merged.back().instances == issue.instances) {
      merged.back().xHi = issue.xHi;
      merged.back().siteCount
          = static_cast<int>((merged.back().xHi - merged.back().xLo) / siteWidth);
    } else {
      merged.push_back(std::move(issue));
    }
  }
  result.issues = std::move(merged);

  result.isFullUtility = result.issues.empty();
  if (result.isFullUtility) {
    log.msg("precheck", "all rows fully covered -> 100% utility OK");
  } else {
    for (const CoverageIssue& issue : result.issues) {
      log.msg("precheck",
              cat(kindName(issue.kind), " row=", issue.rowId, " x=",
                  show(XInterval{issue.xLo, issue.xHi}), " sites=",
                  issue.siteCount, " -> precondition failure"));
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
