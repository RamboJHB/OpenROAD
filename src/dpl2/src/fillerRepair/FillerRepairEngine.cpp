// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "FillerRepairEngine.h"

#include "OracleGate.h"
#include "PreCheck.h"
#include "Ranker.h"
#include "Signature.h"
#include "SubsetSearch.h"
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

  // Stage 2b (spec 6.2): swap-unfixable HINT. V2.1 #6 downgraded this from a
  // fast-fail to a warning: "no filler in the two-instance ring" has no oracle
  // backing (it does not prove a farther filler can't matter through a
  // contiguous implant run), and the payoff was tiny -- with no nearby filler
  // the search itself reports NoEditableFiller with zero checker calls anyway.
  // Keep the hint for upstream triage; let the normal search reach the verdict.
  for (size_t i = 0; i < violations.size(); ++i) {
    if (!hasFillerNearViolation(violations[i], view_)) {
      result.diagnostics.push_back(makeDiag(
          Severity::Warning,
          "UnfixableByTypeSwap",
          cat("violation#", i, " rule=", violations[i].raw.ruleId,
              " footprint=", show(violations[i].xRange),
              " has no filler within its two-instance ring (hint only)")));
      log_.msg("engine",
               cat("violation#", i, " has no nearby filler -> "
                   "UnfixableByTypeSwap hint (search continues)"));
    }
  }

  const DbCoord ruleDistance =
      estimateRuleDistance(request.violations, view_.siteWidth());
  OracleGate gate(checker_, request.targetPlace, request.violations,
                  view_.siteWidth(), ruleDistance, config_, log_);

  // Stages 3..7 under the window escalation loop (spec 6.3/6.7/6.8):
  // L0 -> L1, with the expansion cutoff when a level adds nothing new. V2.1 #7
  // dropped L2 (it was identical to L1 for the single cluster and only ever
  // tripped the cutoff).
  OracleGate::SearchResult best;   // best non-clean across levels (diagnostics)
  // Definitive iff the LAST window we actually searched was fully enumerated
  // (V2.1 #10): an earlier smaller window being complete does not prove the
  // later truncated window has no solution.
  bool lastSearchedDefinitive = false;
  std::vector<InstanceId> previousEditable;

  for (int level = 0; level <= 1; ++level) {
    const RepairWindow window =
        buildWindow(level, request.targetPlace, violations, view_, ruleDistance, log_);

    if (level > 0 && window.editableFillers == previousEditable) {
      // Expansion cutoff (spec 6.3): nothing new to try at this level.
      result.diagnostics.push_back(makeDiag(
          Severity::Info, "ExpansionCutoff",
          cat("window L", level, " adds no new editable filler -> stop escalation")));
      log_.msg("engine",
               cat("window L", level, " identical editable set -> expansion cutoff"));
      break;
    }
    previousEditable = window.editableFillers;

    if (window.editableFillers.empty()) {
      result.diagnostics.push_back(makeDiag(
          Severity::Warning, "NoEditableFiller",
          cat("window L", level, " ", show(window.area()),
              " contains no editable filler")));
      continue;
    }

    const SwapGenerationResult generated =
        generateSwaps(window, view_, candidates_, log_);
    result.diagnostics.insert(result.diagnostics.end(),
                              generated.diagnostics.begin(),
                              generated.diagnostics.end());
    if (generated.swaps.empty()) {
      result.diagnostics.push_back(makeDiag(
          Severity::Warning, "NoSwapGenerated",
          cat("window L", level, ": no usable swap")));
      continue;
    }

    // V2.1 #9: rank fillers, keep each filler's full candidate domain -- the
    // searcher combines fillers and assigns per-domain options, so member caps
    // cannot crowd a key filler (or its third VT) out of the enumeration.
    const std::vector<FillerDomain> ranked = rankFillers(
        generated.swaps, request.targetPlace, violations, window, view_, log_);

    int budget = config_.checkerCallBudgetPerWindow;
    if (!gate.runBaseline(window, budget)) {
      result.hasSolution = false;
      result.diagnostics.insert(result.diagnostics.end(),
                                gate.diagnostics().begin(),
                                gate.diagnostics().end());
      result.diagnostics.push_back(makeDiag(
          Severity::Fatal, "BaselineGateFailed",
          cat("baseline gate failed at window L", level,
              " (see BaselineUnusable/BaselineMismatch above)")));
      log_.msg("engine", "baseline gate failed -> abort");
      return result;
    }

    const EnumerationPlan plan =
        enumerateOverlays(ranked, config_, budget, log_);

    OracleGate::SearchResult sr =
        gate.search(plan.overlays, window, window.guardRegion, budget);
    if (sr.protocolError) {
      result.hasSolution = false;
      result.diagnostics.insert(result.diagnostics.end(),
                                gate.diagnostics().begin(),
                                gate.diagnostics().end());
      result.diagnostics.push_back(makeDiag(
          Severity::Fatal, "CheckerProtocolError",
          "batch protocol violated; rejecting this repair"));
      log_.msg("engine", "checker protocol error -> abort");
      return result;
    }

    if (sr.foundClean) {
      // The winning overlay already passed the baseline-delta gate under this
      // window's guard (spec 6.8). V2.1 #11: no separate final full-overlay
      // check -- it would reuse the identical cacheKey, hit the cache, and add
      // no verification.
      result.hasSolution = true;
      result.changes = toFillerChanges(sr.cleanOverlay);
      result.diagnostics.push_back(makeDiag(
          Severity::Info, "Solution",
          cat("window L", level, ": ", result.changes.size(),
              " change(s); checker requests=", gate.requestsSent(),
              " batches=", gate.batchesSent(), " cacheHits=", gate.cacheHits())));
      log_.msg("engine",
               cat("SOLUTION at L", level, ": ", result.changes.size(),
                   " change(s), requests=", gate.requestsSent(),
                   " cacheHits=", gate.cacheHits()));
      return result;
    }

    if (sr.hasBest
        && (!best.hasBest
            || sr.bestSummary.residualOriginals + sr.bestSummary.newInWindow
                       + sr.bestSummary.relatedInHalo
                   < best.bestSummary.residualOriginals
                         + best.bestSummary.newInWindow
                         + best.bestSummary.relatedInHalo)) {
      best = sr;
    }
    // This window was actually searched; its completeness is what a later
    // "definitive" claim rests on (V2.1 #10).
    lastSearchedDefinitive = plan.complete && !sr.budgetExhausted;
    result.diagnostics.push_back(makeDiag(
        Severity::Info, "WindowExhausted",
        cat("window L", level, ": ", plan.overlays.size(), " candidate(s), ",
            plan.complete && !sr.budgetExhausted
                ? "complete enumeration, definitively no clean overlay"
                : "truncated (size caps or budget), no clean overlay found")));
    log_.msg("engine",
             cat("window L", level, " no clean overlay (",
                 plan.complete && !sr.budgetExhausted ? "definitive" : "truncated",
                 ") -> escalate"));
  }

  // No clean overlay anywhere (spec 6.9): empty changes, explain why.
  result.hasSolution = false;
  result.diagnostics.push_back(makeDiag(
      Severity::Error, "NoCleanOverlay",
      cat("no baseline-delta clean overlay found; ",
          lastSearchedDefinitive ? "window space exhausted definitively" : "budget/caps truncated",
          "; checker requests=", gate.requestsSent(), " batches=",
          gate.batchesSent(), " cacheHits=", gate.cacheHits())));
  if (best.hasBest) {
    result.diagnostics.push_back(makeDiag(
        Severity::Info, "BestOverlay",
        cat("best non-clean candidate: ", best.bestOverlay.size(),
            " swap(s), residualOriginals=", best.bestSummary.residualOriginals,
            " newInWindow=", best.bestSummary.newInWindow,
            " relatedInHalo=", best.bestSummary.relatedInHalo,
            " unrelatedInHalo=", best.bestSummary.unrelatedInHalo)));
  }
  log_.msg("engine",
           cat("NO SOLUTION (", lastSearchedDefinitive ? "definitive" : "truncated",
               "), requests=", gate.requestsSent(),
               " cacheHits=", gate.cacheHits()));
  return result;
}

}  // namespace dpl2::fillerRepair
