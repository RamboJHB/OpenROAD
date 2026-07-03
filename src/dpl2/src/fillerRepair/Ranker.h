// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Ranker (spec section 6.6, TODO 7). Pure ordering: it decides which swaps
// the searcher tries first and never judges legality (checker-as-oracle).
//
// V1 features, lexicographic:
//   0. third-VT demotion  target VT that matches neither the anchor's new VT
//                         nor the filler's neighbor majority goes last
//                         (demoted, NOT removed -- completeness untouched)
//   1. directParticipant  filler appears in a violation's participants
//   2. bridgeScore        filler is in the window's bridge set
//   3. cellAnchorVote     newVt == the anchor's new VT (the most common fix)
//   4. width              narrower first
//   5. position           x, then row, then master id (determinism)
//
// The anchor-follow preference lives HERE: with this order the searcher's
// first size-1/2 subsets are exactly the anchor-follow candidates, so no
// seed/hint machinery exists anywhere else (spec 6.5).

#pragma once

#include <vector>

#include "Log.h"
#include "PlacementView.h"
#include "Signature.h"
#include "Swap.h"
#include "Window.h"

namespace dpl2::fillerRepair {

std::vector<Swap> rankSwaps(std::vector<Swap> swaps,
                            const TargetPlace& anchor,
                            const std::vector<NormalizedViolation>& violations,
                            const RepairWindow& window,
                            const PlacementView& view,
                            const DebugLog& log);

}  // namespace dpl2::fillerRepair
