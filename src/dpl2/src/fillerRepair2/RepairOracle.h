// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Seam 2 of 2: "if I changed these fillers, would it be legal?" (Seam 1 is

#pragma once

#include <cstdint>
#include <vector>

#include <fillerRepair/RepairTypes.h>

namespace dpl2::fillerRepair {

// Planner-internal oracle protocol. These types live with their sole owner
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

// The engine provides the oracle; ImplantLayerChecker remains authoritative.
class RepairOracle
{
 public:
  virtual ~RepairOracle() = default;

  virtual OracleResult checkPlaceWithOverlay(const OracleRequest& request) = 0;
  virtual std::vector<OracleResult> checkPlaceWithOverlays(
      const std::vector<OracleRequest>& requests) = 0;
};

}  // namespace dpl2::fillerRepair
