// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "PlacementPrecheck.h"

#include <algorithm>
#include <utility>

namespace dpl2::fillerRepair::internal {

const char* coverageFindingStatus(CoverageFindingKind kind)
{
  return kind == CoverageFindingKind::Gap ? "Gap" : "Overlap";
}

std::vector<CoverageFinding> findCoverageFindings(
    std::vector<PlacementCoverageRow> rows)
{
  std::sort(
      rows.begin(),
      rows.end(),
      [](const PlacementCoverageRow& left, const PlacementCoverageRow& right) {
        return left.rowId < right.rowId;
      });

  std::vector<CoverageFinding> findings;
  for (PlacementCoverageRow& row : rows) {
    std::sort(row.legalSpans.begin(),
              row.legalSpans.end(),
              [](const XInterval& left, const XInterval& right) {
                return left.xl != right.xl ? left.xl < right.xl
                                           : left.xh < right.xh;
              });
    for (const XInterval& legal : row.legalSpans) {
      if (legal.empty()) {
        continue;
      }
      std::vector<DbCoord> cuts{legal.xl, legal.xh};
      std::vector<DbCoord> starts;
      std::vector<DbCoord> ends;
      starts.reserve(row.placedSpans.size());
      ends.reserve(row.placedSpans.size());
      for (const XInterval& span : row.placedSpans) {
        const DbCoord clippedXl = std::max(span.xl, legal.xl);
        const DbCoord clippedXh = std::min(span.xh, legal.xh);
        if (clippedXh <= clippedXl) {
          continue;
        }
        cuts.push_back(clippedXl);
        cuts.push_back(clippedXh);
        starts.push_back(clippedXl);
        ends.push_back(clippedXh);
      }
      std::sort(cuts.begin(), cuts.end());
      cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
      std::sort(starts.begin(), starts.end());
      std::sort(ends.begin(), ends.end());

      size_t nextStart = 0;
      size_t nextEnd = 0;
      int active = 0;
      for (size_t index = 0; index + 1 < cuts.size(); ++index) {
        const DbCoord segmentXl = cuts[index];
        const DbCoord segmentXh = cuts[index + 1];
        while (nextEnd < ends.size() && ends[nextEnd] <= segmentXl) {
          --active;
          ++nextEnd;
        }
        while (nextStart < starts.size() && starts[nextStart] <= segmentXl) {
          ++active;
          ++nextStart;
        }
        if (segmentXh <= segmentXl || active == 1) {
          continue;
        }
        const CoverageFindingKind kind = active == 0
                                             ? CoverageFindingKind::Gap
                                             : CoverageFindingKind::Overlap;
        if (!findings.empty() && findings.back().kind == kind
            && findings.back().rowId == row.rowId
            && findings.back().span.xh == segmentXl) {
          findings.back().span.xh = segmentXh;
        } else {
          findings.push_back(
              CoverageFinding{kind, row.rowId, {segmentXl, segmentXh}});
        }
      }
    }
  }
  return findings;
}

}  // namespace dpl2::fillerRepair::internal
