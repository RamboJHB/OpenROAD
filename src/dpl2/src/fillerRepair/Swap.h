// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Swap: the planner's atomic operation of this stage (spec section 4).
//
// A Swap is one FillerCellRecord replacement operation plus the geometry/rank
// metadata the planner needs (row, span, VTs). There is deliberately NO
// generic "Move" abstraction layer: this
// stage's operation is swap; the next stage's is rewrite (spec 4.3 / 8.1).
//
// One overlay candidate = a set of Swaps applied atomically. The subset
// searcher enumerates per filler (at most one target VT per filler), so
// "same instance twice" cannot occur by construction -- no conflict
// machinery. canonicalKey() exists only as the checker-call cache key
// (spec 4.2: identical overlays hit the checker once).

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "PlannerDataSource.h"
#include "Types.h"

namespace dpl2::fillerRepair {

class DebugLog;
struct RepairWindow;

struct Swap
{
  InstanceId instanceId = 0;
  MasterId oldMasterId = 0;
  MasterId newMasterId = 0;
  RowId rowId = 0;
  XInterval span;
  VtId oldVt = kUnknownVt;
  VtId newVt = kUnknownVt;
};

using Overlay = std::vector<Swap>;

// Builds a validated Swap or explains why it cannot exist: the instance must
// be a placed filler and the new master a same-width/same-height filler
// master different from the current one. On failure *error (if given)
// receives the reason.
std::optional<Swap> makeSwap(const PlannerDataSource& view,
                             InstanceId instanceId,
                             MasterId newMasterId,
                             std::string* error = nullptr);

// Checker-call cache key: sorted_unique((instanceId, newMasterId)) serialized
// to a string. Order-independent.
std::string canonicalKey(const Overlay& overlay);

// Wire conversion, deterministic order (sorted by instanceId).
ipl::FillerChanges toFillerChanges(const Overlay& overlay,
                                   const PlannerDataSource& dataSource);

struct SwapGenerationResult
{
  // Deterministic order: window editable order (row, x), then candidate
  // master id ascending.
  std::vector<Swap> swaps;
  // NoUsableMaster per replacement-less filler, plus any provider
  // diagnostics. A filler without swaps is a normal outcome, not an error.
  std::vector<Diagnostic> diagnostics;
};

SwapGenerationResult generateSwaps(
    const RepairWindow& window,
    const PlannerDataSource& view,
    const DebugLog& log);


}  // namespace dpl2::fillerRepair
