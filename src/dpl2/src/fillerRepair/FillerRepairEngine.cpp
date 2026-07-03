// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "FillerRepairEngine.h"

#include "PreCheck.h"
#include "Signature.h"
#include "SwapGenerator.h"
#include "Window.h"

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
  const SiteCoverageResult coverage = runUtilityPreCheck(view_, log_);
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
             cat("precheck FAIL (", coverage.issues.size(),
                 " issue(s)) -> NonFullUtility fatal; skip candidates, ",
                 "0 checker calls"));
    return result;
  }

  // Empty snapshot: nothing to repair is a success with no changes.
  if (request.violations.empty()) {
    result.hasSolution = true;
    result.diagnostics.push_back(makeDiag(
        Severity::Info, "EmptySnapshot", "no violations in initial snapshot"));
    log_.msg("engine", "empty violation snapshot -> hasSolution=true, 0 changes");
    return result;
  }

  // Stage 2 (spec 6.2): normalize the snapshot into signatures/footprints.
  const std::vector<NormalizedViolation> violations =
      normalizeViolations(request, view_, log_);

  // Stage 2b (spec 6.2): swap-unfixable fast check. A violation with no
  // filler inside its two-instance ring cannot be affected by any swap ->
  // fail fast, zero checker calls, so upstream can tell "structurally
  // unrepairable" from "search exhausted".
  for (size_t i = 0; i < violations.size(); ++i) {
    if (!hasFillerNearViolation(violations[i], view_)) {
      result.hasSolution = false;
      result.diagnostics.push_back(makeDiag(
          Severity::Error,
          "UnfixableByTypeSwap",
          cat("violation#", i, " rule=", violations[i].raw.ruleId,
              " footprint=", show(violations[i].xRange),
              " has no filler within its two-instance ring")));
      log_.msg("engine",
               cat("violation#", i, " has no nearby filler -> "
                   "UnfixableByTypeSwap, abort before search, 0 checker calls"));
      return result;
    }
  }

  // Stage 3 (spec 6.3): open the repair window at L0. Escalation to L1/L2
  // is driven by the subset search (TODO 8) once it lands.
  const DbCoord ruleDistance =
      estimateRuleDistance(request.violations, view_.siteWidth());
  const RepairWindow window = buildWindow(
      /*level=*/0, request.targetPlace, violations, view_, ruleDistance, log_);
  if (window.editableFillers.empty()) {
    result.hasSolution = false;
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "NoEditableFiller",
        cat("window L0 ", show(window.area()), " contains no editable filler")));
    log_.msg("engine", "window L0 has no editable filler -> no solution");
    return result;
  }

  // Stage 4 (spec 6.5): generate atomic swaps for the window's editable
  // fillers. No swaps at all means the search cannot start.
  const SwapGenerationResult generated =
      generateSwaps(window, view_, candidates_, log_);
  result.diagnostics.insert(result.diagnostics.end(),
                            generated.diagnostics.begin(),
                            generated.diagnostics.end());
  if (generated.swaps.empty()) {
    result.hasSolution = false;
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "NoSwapGenerated",
        cat("window L0 has ", window.editableFillers.size(),
            " editable filler(s) but no usable swap")));
    log_.msg("engine", "no swap generated -> no solution");
    return result;
  }

  // Stages 5..7 (ranking, subset search, oracle gate) land with spec section
  // 11 TODO items 7-10. Until then the engine reports an explicit
  // NotImplemented instead of a silent "no solution" so callers cannot
  // mistake the skeleton for a real search.
  result.hasSolution = false;
  result.diagnostics.push_back(
      makeDiag(Severity::Error,
               "NotImplemented",
               "search pipeline stages (spec TODO 7-10) not implemented yet"));
  log_.msg("engine",
           cat("window L0 ready (editable=", window.editableFillers.size(),
               ", swaps=", generated.swaps.size(),
               ") -> search pipeline pending (TODO 7-10), returning "
               "NotImplemented"));
  return result;
}

}  // namespace dpl2::fillerRepair
