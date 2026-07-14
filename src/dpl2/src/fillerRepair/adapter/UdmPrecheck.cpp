// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "UdmPrecheck.h"

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

const SiteCoverageResult& UdmPrecheck::check(const PlacementView& view,
                                             std::uint64_t coverageRevision)
{
  if (revision_.has_value() && *revision_ == coverageRevision) {
    log_.msg("precheck",
             cat("coverage revision ", coverageRevision,
                 " unchanged -> cached result (", cached_.issues.size(),
                 " issue(s))"));
    return cached_;
  }
  cached_ = runUtilityPreCheck(view, log_);
  revision_ = coverageRevision;
  return cached_;
}

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
