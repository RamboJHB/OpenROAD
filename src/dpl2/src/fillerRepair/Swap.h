// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Swap: the planner's atomic operation of this stage (spec section 4).
//
// A Swap is exactly the FillerChange wire semantics (instanceId +
// newMasterId) plus the geometry/rank metadata the planner needs (row, span,
// VTs). There is deliberately NO generic "Move" abstraction layer: this
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

#include "PlacementView.h"
#include "Types.h"

namespace dpl2::fillerRepair {

struct Swap
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

using Overlay = std::vector<Swap>;

// Builds a validated Swap or explains why it cannot exist: the instance must
// be a placed filler and the new master a same-width/same-height filler
// master different from the current one. On failure *error (if given)
// receives the reason.
std::optional<Swap> makeSwap(const PlacementView& view,
                             InstanceId instanceId,
                             MasterId newMasterId,
                             std::string* error = nullptr);

// Checker-call cache key: sorted_unique((instanceId, newMasterId)) serialized
// to a string. Order-independent.
std::string canonicalKey(const Overlay& overlay);

// Wire conversion, deterministic order (sorted by instanceId).
std::vector<FillerChange> toFillerChanges(const Overlay& overlay);

}  // namespace dpl2::fillerRepair
