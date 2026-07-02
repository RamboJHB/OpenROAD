// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Planner-internal Move abstraction (spec section 4).
//
// The atomic operation is a span rewrite: replace everything on [span) of
// the listed rows with a new left-to-right master sequence. V1 only builds
// the degenerate SwapMove (one filler, one same-size master), but conflict
// detection, canonical keys and the overlay cache are all span-anchored so
// merge/split/multi-height later extend the move *generator* only.
//
// Wire format note (spec 4.1/5.2): FillerRewrite never crosses the checker
// API in V1; SwapMove converts losslessly to FillerChange.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "PlacementView.h"
#include "Types.h"

namespace dpl2::fillerRepair {

struct FillerRewrite
{
  std::vector<RowId> rowIds;           // V1: exactly one row
  XInterval span;                      // rewritten x range
  std::vector<MasterId> newMasterIds;  // instantiated left to right
};

// Degenerate rewrite: one filler instance swapped to a same-size master.
// Keeps the instance id so the V1 wire conversion needs no span lookup.
struct SwapMove
{
  InstanceId instanceId = 0;
  MasterId oldMasterId = 0;
  MasterId newMasterId = 0;
  RowId rowId = 0;
  XInterval span;
  VtId oldVt = kUnknownVt;
  VtId newVt = kUnknownVt;

  FillerRewrite rewrite() const
  {
    return FillerRewrite{{rowId}, span, {newMasterId}};
  }

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

// Two moves conflict when they share a row and their spans intersect
// ("same instance twice" in V1 is a special case of this rule).
bool movesConflict(const FillerRewrite& a, const FillerRewrite& b);
bool overlayHasConflict(const Overlay& overlay);

// Canonical key: sorted_unique((rowIds, span, newMasterIds)) serialized to a
// string. Order-independent -- the checker cache and the dedupe of enumerated
// subsets both key on this (spec 4.2).
std::string canonicalKey(const Overlay& overlay);

// V1 wire conversion, deterministic order (sorted by instanceId).
std::vector<FillerChange> toFillerChanges(const Overlay& overlay);

// Structural invariants of a rewrite against the current placement
// (spec 4.1): the span covers whole filler instances exactly (no partial
// instance, no std cell/blockage inside), and the new masters are fillers
// whose widths sum to the span length. Returns an error text, or nullopt
// when the rewrite is well-formed. Planner constructs moves that hold this
// by construction; the check is used in tests and debug builds.
std::optional<std::string> validateRewriteCoverage(const PlacementView& view,
                                                   const FillerRewrite& rewrite);

}  // namespace dpl2::fillerRepair
