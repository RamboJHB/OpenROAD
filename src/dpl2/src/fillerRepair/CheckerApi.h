// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Abstract checker-facing API (spec section 5.2).
//
// The checker is the only DRC oracle: it applies one atomic overlay per
// request (all fillerChanges together), re-checks in target-local scope, and
// collects violations at least inside guardRegion. It never mutates the DB.
//
// Protocol the planner relies on (enforced by tests against the fake, and
// required from the real checker):
//  - every CheckResult echoes its request's requestId;
//  - correctness must not depend on batch result order;
//  - one invalid request only affects its own CheckResult;
//  - status != Checked always carries diagnostics.

#pragma once

#include <vector>

#include "Types.h"

namespace dpl2::fillerRepair {

class ImplantOverlayChecker
{
 public:
  virtual ~ImplantOverlayChecker() = default;

  virtual CheckResult checkPlaceWithOverlay(
      const OverlayCheckRequest& request) = 0;

  virtual std::vector<CheckResult> checkPlaceWithOverlays(
      const std::vector<OverlayCheckRequest>& requests) = 0;
};

// A snapshot with zero findings. This alone is NOT the repair accept gate;
// accept is baseline-delta clean under the same guardRegion (spec 6.8).
inline bool isRawCheckerSnapshotClean(const CheckResult& result)
{
  return result.status == CheckStatus::Checked && result.isLegal
         && result.violations.empty();
}

// Result carries enough information to be scored at all (spec 7.1 of V1,
// kept in V2 6.8): checked, and either legal or explains why not.
inline bool isCheckedResultUsable(const CheckResult& result)
{
  return result.status == CheckStatus::Checked
         && (result.isLegal || !result.violations.empty()
             || !result.diagnostics.empty());
}

}  // namespace dpl2::fillerRepair
