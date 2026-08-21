// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Seam 2 of 2: "if I changed these fillers, would it be legal?" (Seam 1 is
// PlacementView -- what is placed where.)
//
// This is NOT a second DRC checker, and it must never grow into one. The real
// ImplantLayerChecker decides; the engine only translates its answers into
// the records below, and the list of changes passes through untouched. Every
// acceptance in this module traces back to a real checker call.
//
// The request id and status are bookkeeping between the search and the
// engine -- callers of the feature never see them.

#pragma once

#include <cstdint>
#include <vector>

#include <fillerRepair/RepairTypes.h>

namespace dpl2::fillerRepair {

// Planner-internal oracle protocol. These types live with their sole owner
// instead of the shared model in RepairTypes.h.
// Runtime callers never see an oracle request id or status;
// FillerRepairEngine translates final-checker results at this boundary while
// the FillerChanges payload remains unchanged.
using OracleRequestId = int32_t;

struct OracleRequest
{
  OracleRequestId requestId = -1;  // planner-generated, unique per batch
  TargetPlace targetPlace;
  Region guardRegion;  // repair window expanded by a two-cell guard halo
  ipl::FillerChanges fillerChanges;  // one atomic overlay candidate
};

enum class OracleStatus
{
  Checked,
  InvalidOverlay,
  CheckerError
};

struct OracleResult
{
  OracleRequestId requestId = -1;  // must echo OracleRequest.requestId
  OracleStatus status = OracleStatus::CheckerError;
  bool isLegal = false;  // meaningful only when status == Checked
  std::vector<Violation> violations;
  std::vector<Diagnostic> diagnostics;
};

// Runtime and test implementations provide the oracle. The interface is not
// a second DRC checker: the final ImplantLayerChecker remains the sole source
// of legality.
class RepairOracle
{
 public:
  virtual ~RepairOracle() = default;

  virtual OracleResult checkPlaceWithOverlay(const OracleRequest& request) = 0;
  virtual std::vector<OracleResult> checkPlaceWithOverlays(
      const std::vector<OracleRequest>& requests) = 0;
};

}  // namespace dpl2::fillerRepair
