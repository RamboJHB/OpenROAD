// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The planner's legality oracle: the second of the two seams the runtime
// engine implements (the other is PlacementView).
//
// This is NOT a second DRC checker. The final ImplantLayerChecker remains the
// sole source of legality; the engine translates its results into these
// records at this boundary while the FillerChanges payload passes through
// unchanged. Runtime callers never see an oracle request id or status.

#pragma once

#include <cstdint>
#include <vector>

#include "RepairTypes.h"

namespace dpl2::fillerRepair {

// Planner-internal oracle protocol. These types live with their sole owner
// instead of the shared model in Types.h. Runtime callers never see an oracle
// request id or status; FillerRepairEngine translates final-checker results at
// this boundary while the FillerChanges payload remains unchanged.
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

inline bool isOracleSnapshotClean(const OracleResult& result)
{
  return result.status == OracleStatus::Checked && result.isLegal
         && result.violations.empty();
}

}  // namespace dpl2::fillerRepair
