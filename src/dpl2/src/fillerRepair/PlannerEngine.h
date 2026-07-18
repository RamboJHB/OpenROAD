// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Internal pure repair planner (spec sections 3.2 / 5.4).
//
// Deterministic pipeline: normalize -> window -> swap gen ->
// rank -> subset search -> oracle gate -> result. The engine owns no state
// between repair() calls and never mutates the design; all effects are the
// returned FillerRepairResult.
//
// Implementation status (spec section 11 TODO order):
//   [x] 1  planner API, Swap struct, overlay cache key
//   [x] 2  fake checker / fake candidate provider (see fake/)
//   [x] 3  placement coverage scanner (public gate now owned by facade)
//   [x] 4  violation normalization + signature matching (Signature.h)
//   [x] 5  L0 + adaptive-L1, guardRegion, unfixable warning (#6/#7/#8)
//   [x] 6  swap generator: atomic swap moves only (Swap.h)
//   [x] 7  ranker (Ranker.h)
//   [x] 8  subset searcher (SubsetSearch.h)
//   [x] 9  oracle gate: batch, cache, baseline-delta (OracleGate.h)
//   [x] 10 adaptive window escalation + diagnostics (final check dropped #11)

#pragma once

#include <atomic>

#include "Log.h"
#include "OracleGate.h"
#include "PlacementView.h"
#include "Types.h"

namespace dpl2::fillerRepair {

// Search parameters (spec section 7). All knobs live here so tests and
// diagnostics can print the exact configuration used.
struct RepairConfig
{
  int checkerCallBudgetPerWindow = 512;  // includes the baseline request
  int batchSize = 32;
  int maxSubsetSize = 4;          // large-window truncation only (spec 6.7)
  int memberCapSize2 = 24;        // N_2
  int memberCapSize3 = 12;        // N_3
  int memberCapSize4 = 8;         // N_4
  int adaptiveStepFillers = 2;    // K per relevant row/side (spec 6.3 #8)
  bool verbose = false;           // enables the [fr] debug transcript
};

namespace internal {

class PlannerEngine
{
 public:
  PlannerEngine(const PlacementView& view,
                ImplantOverlayChecker& checker,
                RepairConfig config = {});

  FillerRepairResult repair(const FillerRepairRequest& request);

 private:
  const PlacementView& view_;
  ImplantOverlayChecker& checker_;
  RepairConfig config_;
  DebugLog log_;
  // Guards spec 3.3's no-reentrancy contract AND flags concurrent use of one
  // engine instance; concurrent repairs use one engine per thread.
  std::atomic<bool> repair_active_{false};
};

}  // namespace internal

}  // namespace dpl2::fillerRepair
