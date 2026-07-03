// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Pure repair planner entry point (spec sections 3.2 / 5.4).
//
// Deterministic pipeline: precheck -> normalize -> window -> move gen ->
// rank -> subset search -> oracle gate -> result. The engine owns no state
// between repair() calls and never mutates the design; all effects are the
// returned FillerRepairResult.
//
// Implementation status (spec section 11 TODO order):
//   [x] 1  planner API, internal Move, canonical key/conflict, adapter
//   [x] 2  fake checker / fake candidate provider (see fake/)
//   [x] 3  100% utility pre-check with fatal short-circuit
//   [x] 4  violation normalization + signature matching (Signature.h)
//   [x] 5  L0/L1/L2 window builder + guardRegion + unfixable fast check
//   [ ] 6  swap move generator + bridge fillers + group hints
//   [ ] 7  ranker
//   [ ] 8  subset searcher
//   [ ] 9  oracle gate (batch, cache, baseline-delta)
//   [ ] 10 final full-overlay check + diagnostics

#pragma once

#include "CandidateApi.h"
#include "CheckerApi.h"
#include "Log.h"
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
  bool verbose = false;           // enables the [fr] debug transcript
};

class FillerRepairEngine
{
 public:
  FillerRepairEngine(const PlacementView& view,
                     ImplantOverlayChecker& checker,
                     const FillerMasterCandidateProvider& candidates,
                     RepairConfig config = {});

  FillerRepairResult repair(const FillerRepairRequest& request);

 private:
  const PlacementView& view_;
  ImplantOverlayChecker& checker_;
  const FillerMasterCandidateProvider& candidates_;
  RepairConfig config_;
  DebugLog log_;
};

}  // namespace dpl2::fillerRepair
