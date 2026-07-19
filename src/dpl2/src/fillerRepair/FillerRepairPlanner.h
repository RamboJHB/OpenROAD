// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Internal pure filler-repair planner (spec sections 3.2 / 5.4).
//
// Deterministic pipeline: normalize -> window -> swap gen ->
// rank -> subset search -> oracle gate -> result. The planner owns no state
// between repair() calls and never mutates the design; all effects are the
// returned FillerRepairResult. With RepairConfig::verbose enabled, each stage
// prints a deterministic [fr][stage] decision transcript.

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
  // Safety valve for the NO-SOLUTION path: without it adaptive expansion
  // keeps adding fillers until the window rows are exhausted (levels ~
  // fillers/(2K), each level up to one window budget of checker calls).
  // Reaching the cap ends the search with the existing "truncated"
  // semantics -- never a wrong answer, only a bounded give-up.
  int maxAdaptiveLevels = 32;
  bool verbose = false;           // enables the [fr] debug transcript
};

namespace internal {

class FillerRepairPlanner
{
 public:
  FillerRepairPlanner(const PlacementView& view,
                      ImplantOverlayChecker& checker,
                      RepairConfig config = {});

  FillerRepairResult repair(const FillerRepairRequest& request);

 private:
  const PlacementView& view_;
  ImplantOverlayChecker& checker_;
  RepairConfig config_;
  DebugLog log_;
  // Guards spec 3.3's no-reentrancy contract AND flags concurrent use of one
  // planner instance; concurrent repairs use one planner per thread.
  std::atomic<bool> repair_active_{false};
};

}  // namespace internal

}  // namespace dpl2::fillerRepair
