// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Repair window builder: L0/L1/L2 ladder, guardRegion, bridge fillers and
// the swap-unfixable fast check (spec sections 6.2/6.3, TODO 5).
//
// Levels (V1 runs a single cluster, spec 6.4, so L2 == the merged L1):
//   L0  violation participants ∪ anchor-adjacent fillers ∪ bridge fillers
//   L1  L0 snapped to whole instances, extended sideways to the nearest
//       non-filler boundary (fixed cell / row edge), rows ±1
//   L2  merge of overlapping L1 windows -- identical to L1 for one cluster
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

// Swap-unfixable fast check (spec 6.2, adopted from DAC'23): true when at
// least one filler lies within a two-instance ring of the violation's
// footprint on its rows ±2. When false for any original violation, no swap
// overlay can possibly affect it -- the engine fails fast with
// UnfixableByTypeSwap and zero checker calls.
bool hasFillerNearViolation(const NormalizedViolation& violation,
                            const PlacementView& view);

}  // namespace dpl2::fillerRepair
