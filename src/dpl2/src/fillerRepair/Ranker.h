// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Ranker (spec section 6.6, TODO 7; V2.1 #9). Pure ordering: it decides which
// fillers (and which target VT per filler) the searcher tries first and never
// judges legality (checker-as-oracle).
//
// V2.1 #9 changed the output from a flat ranked swap list to ranked FILLER
// DOMAINS: fillers are ordered by filler-level features, and each filler keeps
// its FULL candidate-master domain. Member caps downstream count fillers, so a
// key filler -- or its third-VT option -- can no longer be crowded out of the
// enumeration by higher-ranked fillers' options filling a flat rank prefix.
//
// Filler order (lexicographic):
//   1. directParticipant  filler appears in a violation's participants
//   2. bridgeScore        filler is in the window's bridge set
//   3. width              narrower first
//   4. position           x, then row (determinism)
//
// Domain order within one filler (the target-VT preference, spec 6.6):
//   anchor's new VT first, then the filler's neighbor majority VT, then
//   stable master id; the "third VT" (matching neither) is demoted LAST
//   within this filler's own domain -- demoted, NOT removed, so completeness
//   is untouched and three-VT corner cases stay reachable.
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

// One editable filler with its full, preference-ordered candidate domain.
// Ranking never truncates a domain (V2.1 #9).
struct FillerDomain
{
  InstanceId instanceId = 0;
  std::vector<Swap> options;
};

std::vector<FillerDomain> rankFillers(
    const std::vector<Swap>& swaps,
    const TargetPlace& anchor,
    const std::vector<NormalizedViolation>& violations,
    const RepairWindow& window,
    const PlacementView& view,
    const DebugLog& log);

}  // namespace dpl2::fillerRepair
