// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Swap generator (spec section 6.5, TODO 6): the only move producer of this
// stage. For every editable filler in the repair window it asks the
// infrastructure candidate provider for same-size replacement masters and
// emits one validated SwapMove per candidate.
//
// Deliberately atomic-swaps-only: no group hints, no seed overlays.
// Combinations arise as size-2/3 subsets in the subset searcher (spec 6.7),
// and the anchor-follow preference is realized by the ranker's move order
// (spec 6.6) -- the generator stays a plain enumeration.
//
// Guard-only fillers never reach this function: the window's editable set is
// the generation universe (spec 6.3).

#pragma once

#include <vector>

#include "CandidateApi.h"
#include "Log.h"
#include "Move.h"
#include "PlacementView.h"
#include "Window.h"

namespace dpl2::fillerRepair {

struct SwapGenerationResult
{
  // Deterministic order: window editable order (row, x), then candidate
  // master id ascending.
  std::vector<SwapMove> moves;
  // NoUsableMaster per replacement-less filler, plus any provider
  // diagnostics. A filler without moves is a normal outcome, not an error.
  std::vector<Diagnostic> diagnostics;
};

SwapGenerationResult generateSwapMoves(
    const RepairWindow& window,
    const PlacementView& view,
    const FillerMasterCandidateProvider& provider,
    const DebugLog& log);

}  // namespace dpl2::fillerRepair
