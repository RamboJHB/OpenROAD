// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Violation normalization, signature matching and change-relatedness
// (spec section 6.2).
//
// The signature key is pinned by the spec -- the baseline-delta gate depends
// on it, so no implementation freedom here:
//   signature = (ruleId, kind, relation, primaryLayer, secondaryLayer,
//                sorted(rowIds), xWindow overlap)
// Two violations from different checker snapshots match iff all id/enum/layer
// fields are equal and their xWindows overlap by at least half of the shorter
// window (or lie within one site of each other). The implant layers matter
// because P-band and N-band MS can occur at the same x gap with the same
// rule/kind/relation/rows -- they are distinct violations (spec 6.2: no dedup
// by position), and only the layer tells them apart when the checker shares a
// ruleId across bands.
//
// "Related to this overlay" is likewise pinned: a violation is related iff a
// participant is a changed instance, or its xWindow is within one rule
// distance of a changed filler's span on the same or an adjacent row.

#pragma once

#include <vector>

#include "Log.h"
#include "Swap.h"
#include "PlannerDataSource.h"
#include "Types.h"

namespace dpl2::fillerRepair {

// A violation with the derived fields the planner works on. `raw` is kept by
// value: normalization must outlive the request's snapshot vector.
struct NormalizedViolation
{
  Violation raw;

  std::vector<RowId> rowIds;  // sorted unique, never empty
  bool rowIdFallback = false;  // rowIds were missing; anchor row substituted

  // xWindow united with all participant x ranges: the geometric footprint
  // used for windowing and the unfixable fast check.
  XInterval xRange;

  std::vector<InstanceId> cellAnchors;        // target + non-filler participants
  std::vector<InstanceId> fillerParticipants;  // filler participants
};

// Normalizes the initial snapshot. Deterministic; logs one line per
// violation (signature summary -> derived footprint).
std::vector<NormalizedViolation> normalizeViolations(
    const FillerRepairRequest& request,
    const PlannerDataSource& view,
    const DebugLog& log);

// Pinned signature match across two checker snapshots (see file header).
bool sameSignature(const Violation& a, const Violation& b, DbCoord siteWidth);

// Pinned relatedness: participants touch a changed instance, or xWindow is
// within `ruleDistance` of a changed span on the same/adjacent row.
bool isRelatedToOverlay(const Violation& violation,
                        const Overlay& overlay,
                        DbCoord ruleDistance);

// Rule-distance estimate for geometry heuristics: the largest requiredValue
// in the snapshot, falling back to one site. Only used for windows and
// relatedness margins -- never for legality decisions (checker-as-oracle).
DbCoord estimateRuleDistance(const std::vector<Violation>& violations,
                             DbCoord siteWidth);

}  // namespace dpl2::fillerRepair
