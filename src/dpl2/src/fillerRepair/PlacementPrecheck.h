// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Pure placement-coverage sweep used by FillerRepairEngine::precheck().
// Infrastructure converts Grid legal pixels and placed UDM cells into these
// row spans; this layer only answers whether each legal x interval is covered
// exactly once. Keeping the sweep UDM-free makes its boundary cases portable.

#pragma once

#include <vector>

#include "Types.h"

namespace dpl2::fillerRepair::internal {

enum class CoverageFindingKind
{
  Gap,
  Overlap
};

struct PlacementCoverageRow
{
  RowId rowId = 0;
  std::vector<XInterval> legalSpans;
  std::vector<XInterval> placedSpans;
};

struct CoverageFinding
{
  CoverageFindingKind kind = CoverageFindingKind::Gap;
  RowId rowId = 0;
  XInterval span;
};

// Returns findings in row/legal-span/x order. Adjacent findings of the same
// kind in one row are coalesced, including across touching legal spans.
std::vector<CoverageFinding> findCoverageFindings(
    std::vector<PlacementCoverageRow> rows);

const char* coverageFindingStatus(CoverageFindingKind kind);

}  // namespace dpl2::fillerRepair::internal
