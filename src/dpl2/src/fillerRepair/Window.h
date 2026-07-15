// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Repair window builder: L0 + adaptive-L1, guardRegion, bridge fillers and
// the swap-unfixable hint (spec sections 6.2/6.3, TODO 5, V2.1 #8).
//
// Levels (V2.1 #7 dropped L2):
//   L0  violation participants ∪ anchor-adjacent fillers ∪ bridge fillers
//   adaptive-L1  each step adds at most K contiguous fillers per relevant row
//       toward the best non-clean candidate's blocking side; coupled rows ±1
//       are included, and a non-filler boundary stops growth on that row.
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

// Builds L0 for the (single) violation cluster around the anchor.
// `ruleDistance` widens bridge detection margins only. `level` is retained for
// source compatibility and must be 0; adaptive growth uses the API below.
RepairWindow buildWindow(int level,
                         const TargetPlace& anchor,
                         const std::vector<NormalizedViolation>& violations,
                         const PlacementView& view,
                         DbCoord ruleDistance,
                         const DebugLog& log);

// One adaptive-L1 step (spec 6.3, V2.1 #8). `blocking` is the best non-clean
// candidate's residual/new-related violation set. Direction is derived from
// those x windows relative to `current`; when no directional finding exists,
// both sides are tried. The step is deterministic and never sweeps an entire
// filler run: each selected side adds at most `fillersPerRow` adjacent fillers
// per relevant row, stopping at a non-filler boundary.
RepairWindow expandWindowAdaptive(const RepairWindow& current,
                                  const TargetPlace& anchor,
                                  const std::vector<Violation>& blocking,
                                  const PlacementView& view,
                                  int fillersPerRow,
                                  const DebugLog& log);

}  // namespace dpl2::fillerRepair
