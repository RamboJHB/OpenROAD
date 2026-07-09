// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Subset searcher enumeration (spec section 6.7, TODO 8; V2.1 #9).
//
// Input is the ranker's FILLER DOMAINS (V2.1 #9): enumeration walks
// (subset size ascending, filler-combination lexicographic by filler rank,
// then the Cartesian product of the chosen fillers' domains in domain order,
// last filler's option varying fastest). One option per filler by
// construction, so duplicate-instance conflicts cannot occur (spec 4.2).
// Member caps count FILLERS, not options -- a filler that makes the cut
// always brings its whole domain, so its third-VT option can never be
// crowded out by other fillers' options.
//
// Complete-enumeration rule: when the full subset space (prod of
// (1 + |domain_i|) - 1) fits into the remaining checker budget, the whole
// space is emitted and "no clean overlay in this window" becomes a certainty;
// otherwise size/member caps truncate (RepairConfig).

#pragma once

#include <vector>

#include "FillerRepairEngine.h"
#include "Log.h"
#include "Ranker.h"
#include "Swap.h"

namespace dpl2::fillerRepair {

struct EnumerationPlan
{
  bool complete = false;          // full space emitted -> definitive result
  std::vector<Overlay> overlays;  // enumeration order, capped at budget
};

EnumerationPlan enumerateOverlays(const std::vector<FillerDomain>& ranked,
                                  const RepairConfig& config,
                                  int budget,
                                  const DebugLog& log);

}  // namespace dpl2::fillerRepair
