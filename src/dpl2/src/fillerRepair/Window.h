// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Repair window builder: L0/L1 ladder, guardRegion, bridge fillers and the
// swap-unfixable hint (spec sections 6.2/6.3, TODO 5).
//
// Levels (V2.1 #7 dropped L2 -- it was the merged form of L1 and identical for
// the single cluster the engine solves):
//   L0  violation participants ∪ anchor-adjacent fillers ∪ bridge fillers
//   L1  L0 snapped to whole instances, extended sideways to the nearest
//       non-filler boundary (fixed cell / row edge), rows ±1
//
// NOTE: spec V2.1 #8 further refines L1 into an "adaptive" step that grows K
// fillers toward the blocking side instead of sweeping a whole run; that
// refinement is not yet implemented here (L1 still does the boundary sweep).
//
// guardRegion = expandByCellRing(window, 2): two placed instances beyond the
// window on each side, rows ±2. Guard-only fillers may never be edited; the
// region only widens checker collection (spec 6.3).

#pragma once

#include <vector>

#include "Log.h"
#include "PlacementView.h"
#include "Signature.h"
#include "Types.h"

namespace dpl2::fillerRepair {

struct RepairWindow
{
  int level = 0;
  std::vector<RowId> rows;  // sorted; rows the planner may edit fillers in
  XInterval x;              // editable x range (snapped to whole instances)

  // Fillers inside rows/x, sorted by (row, x): the move-generation universe.
  std::vector<InstanceId> editableFillers;
  // Subset of editableFillers flagged as bridge fillers (default-mandatory
  // candidates, spec 6.3/6.5).
  std::vector<InstanceId> bridgeFillers;

  Region area() const
  {
    if (rows.empty()) {
      return Region{};
    }
    return Region{x, rows.front(), rows.back()};
  }

  Region guardRegion;  // two-cell ring around area()

  bool containsEditable(InstanceId id) const;
};

// Builds the window at `level` for the (single) violation cluster around the
// anchor. `ruleDistance` widens bridge detection margins only. Deterministic;
// logs the resulting rows/x/filler counts and the guard region.
RepairWindow buildWindow(int level,
                         const TargetPlace& anchor,
                         const std::vector<NormalizedViolation>& violations,
                         const PlacementView& view,
                         DbCoord ruleDistance,
                         const DebugLog& log);

// Swap-unfixable hint (spec 6.2, adopted from DAC'23): true when at least one
// filler lies within a two-instance ring of the violation's footprint on its
// rows ±2. When false, a swap is unlikely to help -- but V2.1 #6 uses this only
// as a warning hint, not a fast-fail (the ring argument has no oracle backing).
bool hasFillerNearViolation(const NormalizedViolation& violation,
                            const PlacementView& view);

}  // namespace dpl2::fillerRepair
