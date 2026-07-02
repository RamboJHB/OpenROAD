// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// 100% utility preflight (spec section 6.1).
//
// Hard precondition of the repair: every legal std-cell site is covered
// exactly once by a std cell or a filler. Gap / overlap / off-grid / illegal
// occupant are all precondition failures; on any of them the engine must
// return a fatal NonFullUtility diagnostic without generating candidates or
// calling the checker.

#pragma once

#include "Log.h"
#include "PlacementView.h"
#include "Types.h"

namespace dpl2::fillerRepair {

// Scans every row of the view. Collects all issues (deterministic order:
// row ascending, then x ascending) so diagnostics can report more than the
// first defect. isFullUtility == issues.empty().
SiteCoverageResult runUtilityPreflight(const PlacementView& view,
                                       const DebugLog& log);

}  // namespace dpl2::fillerRepair
