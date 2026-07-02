// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "FillerRepairEngine.h"

#include "Preflight.h"

namespace dpl2::fillerRepair {

FillerRepairEngine::FillerRepairEngine(
    const PlacementView& view,
    ImplantOverlayChecker& checker,
    const FillerMasterCandidateProvider& candidates,
    RepairConfig config)
    : view_(view),
      checker_(checker),
      candidates_(candidates),
      config_(config),
      log_(config.verbose)
{
}

FillerRepairResult FillerRepairEngine::repair(const FillerRepairRequest& request)
{
  FillerRepairResult result;

  log_.msg("engine",
           cat("repair start: anchor inst=", request.targetPlace.instanceId,
               " master=", request.targetPlace.masterId,
               " row=", request.targetPlace.rowId,
               " x=", request.targetPlace.x,
               " violations=", request.violations.size()));

  // Stage 1: hard precondition (spec 6.1). On failure: fatal, no candidate
  // generation, zero checker calls, empty changes.
  const SiteCoverageResult coverage = runUtilityPreflight(view_, log_);
  if (!coverage.isFullUtility) {
    const CoverageIssue& first = coverage.issues.front();
    result.hasSolution = false;
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal,
        "NonFullUtility",
        cat("placement precondition failed (", coverage.issues.size(),
            " issue(s)); first: row=", first.rowId, " x=",
            show(XInterval{first.xLo, first.xHi}), " sites=", first.siteCount)));
    result.diagnostics.insert(result.diagnostics.end(),
                              coverage.diagnostics.begin(),
                              coverage.diagnostics.end());
    log_.msg("engine",
             cat("preflight FAIL (", coverage.issues.size(),
                 " issue(s)) -> NonFullUtility fatal; skip candidates, ",
                 "0 checker calls"));
    return result;
  }

  // Stages 2..7 (normalization, windowing, move generation, ranking, subset
  // search, oracle gate) land with spec section 11 TODO items 4-10. Until
  // then the engine reports an explicit NotImplemented instead of a silent
  // "no solution" so callers cannot mistake the skeleton for a real search.
  result.hasSolution = false;
  result.diagnostics.push_back(
      makeDiag(Severity::Error,
               "NotImplemented",
               "search pipeline stages (spec TODO 4-10) not implemented yet"));
  log_.msg("engine",
           "preflight OK -> search pipeline pending (TODO 4-10), "
           "returning NotImplemented");
  return result;
}

}  // namespace dpl2::fillerRepair
