// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Subset searcher enumeration (spec section 6.7, TODO 8).
//
// Produces overlay candidates in the pinned deterministic order:
// (subset size ascending, member ranks lexicographic), grouped per filler --
// at most one swap per filler in a subset, so duplicate-instance conflicts
// are impossible by construction (spec 4.2).
//
// Complete-enumeration rule: when the full subset space (prod of
// (1 + candidates per filler) - 1) fits into the remaining checker budget,
// the whole space is emitted and "no clean overlay in this window" becomes a
// certainty; otherwise size/member caps truncate (RepairConfig).

#pragma once

#include <vector>

#include "FillerRepairEngine.h"
#include "Log.h"
#include "Swap.h"

namespace dpl2::fillerRepair {

struct EnumerationPlan
{
  bool complete = false;          // full space emitted -> definitive result
  std::vector<Overlay> overlays;  // enumeration order, capped at budget
};

EnumerationPlan enumerateOverlays(const std::vector<Swap>& ranked,
                                  const RepairConfig& config,
                                  int budget,
                                  const DebugLog& log);

}  // namespace dpl2::fillerRepair
