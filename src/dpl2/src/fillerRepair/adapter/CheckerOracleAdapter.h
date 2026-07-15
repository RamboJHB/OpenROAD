// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// fillerRepair::ImplantOverlayChecker over the real
// ipl::ImplantLayerChecker::checkPlaceWithOverlays (final list-only contract,
// spec 5.2.1 / AGENTS D21): each candidate returns EVERY guard-clipped
// violation, no filtering, no dedup; correlation is by order.
//
// Responsibilities of this adapter:
//  - type conversion: TargetPlace (x DBU) -> CheckRequest (colId site units);
//    Region (row-based) -> eUTL::Rect guard (y = row*rowHeight, yh uses
//    (rowHi+1)*rowHeight - 1 so a touching adjacent row is NOT pulled in --
//    mirrors the checker test harness);
//  - protocol synthesis: the planner's requestId is echoed back by INDEX
//    (result i belongs to request i); a batch is grouped by identical
//    (targetPlace, guardRegion) -- the engine always sends one group per
//    batch, but mixed groups are handled correctly by sub-batching;
//  - violation enrichment: kind from ruleSource, relation from relationship,
//    participants synthesized from violation.instances via the view
//    (masterId/rowId/xRange/isFiller/isTarget);
//  - result status: violations empty + !isLegal + diagnostics -> the
//    candidate itself was invalid (InvalidOverlay); otherwise Checked.
//
// Duplicate contract: the checker may report one physical violation once per
// band/direction, deterministically. The engine's one-to-one multiset delta
// tolerates that as long as the ORIGINAL SNAPSHOT comes from this same
// adapter (see FillerVtRepair) -- never mix snapshot sources.

#pragma once

#include <vector>

#include "../CheckerApi.h"
#include "../Log.h"
#include "InfrastructurePlacementView.h"
#include "drc/ImplantLayerChecker.h"

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

class CheckerOracleAdapter : public ImplantOverlayChecker
{
 public:
  CheckerOracleAdapter(const ipl::ImplantLayerChecker& checker,
                       const PlacementView& view,
                       DbCoord rowHeight,
                       const DebugLog& log);

  CheckResult checkPlaceWithOverlay(
      const OverlayCheckRequest& request) override;
  std::vector<CheckResult> checkPlaceWithOverlays(
      const std::vector<OverlayCheckRequest>& requests) override;

  // Conversion helpers (exposed for the entry point / tests).
  ipl::CheckRequest toCheckRequest(const TargetPlace& place) const;
  ::Rect toGuardRect(const Region& region) const;
  Violation toPlannerViolation(const ipl::Violation& v,
                               InstanceId targetInstance) const;

 private:
  const ipl::ImplantLayerChecker& checker_;
  const PlacementView& view_;
  DbCoord row_height_ = 1;
  const DebugLog& log_;
};

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
