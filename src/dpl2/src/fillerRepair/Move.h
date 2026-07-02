// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Planner-internal move. V1 is swap-only (spec section 2.1): one filler
// instance replaced by a same-width/same-height master. rowId/span are kept
// on the move because windowing, bridge-filler detection and delta
// relatedness (spec sections 6.2/6.3) are geometry-based -- they do not
// imply any merge/split support.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "PlacementView.h"
#include "Types.h"

namespace dpl2::fillerRepair {

struct SwapMove
{
  InstanceId instanceId = 0;
  MasterId oldMasterId = 0;
  MasterId newMasterId = 0;
  RowId rowId = 0;
  XInterval span;
  VtId oldVt = kUnknownVt;
  VtId newVt = kUnknownVt;

  FillerChange change() const { return FillerChange{instanceId, newMasterId}; }
};

// One overlay candidate = a set of non-conflicting moves applied atomically.
using Overlay = std::vector<SwapMove>;

// Builds a validated SwapMove or explains why it cannot exist: the instance
// must be a placed filler and the new master a same-width/same-height filler
// master different from the current one. On failure *error (if given)
// receives the reason.
std::optional<SwapMove> makeSwapMove(const PlacementView& view,
                                     InstanceId instanceId,
                                     MasterId newMasterId,
                                     std::string* error = nullptr);

// Swap-only conflict rule: the same instance must not appear twice in one
// overlay (two swaps of one filler cannot be atomic).
bool overlayHasConflict(const Overlay& overlay);

// Canonical key: sorted_unique((instanceId, newMasterId)) serialized to a
// string. Order-independent -- checker-call dedupe and the cache of
// enumerated subsets both key on this (spec 4.2).
std::string canonicalKey(const Overlay& overlay);

// V1 wire conversion, deterministic order (sorted by instanceId).
std::vector<FillerChange> toFillerChanges(const Overlay& overlay);

}  // namespace dpl2::fillerRepair
