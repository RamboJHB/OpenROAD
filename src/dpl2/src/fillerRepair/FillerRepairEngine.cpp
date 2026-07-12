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

#include <algorithm>

namespace dpl2::fillerRepair {

namespace {

int blockerCount(const OracleGate::SearchResult& result)
{
  return result.bestSummary.residualOriginals
         + result.bestSummary.newInWindow
         + result.bestSummary.relatedInHalo
         + (result.bestSummary.usable ? 0 : 1000);
}

bool betterBest(const OracleGate::SearchResult& candidate,
                const OracleGate::SearchResult& current)
{
  return candidate.hasBest
         && (!current.hasBest || blockerCount(candidate) < blockerCount(current)
             || (blockerCount(candidate) == blockerCount(current)
                 && candidate.bestOverlay.size() < current.bestOverlay.size()));
}

bool sameBlockingMultiset(const std::vector<Violation>& left,
                          const std::vector<Violation>& right,
                          DbCoord siteWidth)
{
  if (left.size() != right.size()) {
    return false;
  }
  std::vector<char> consumed(right.size(), 0);
  for (const Violation& violation : left) {
    bool matched = false;
    for (size_t i = 0; i < right.size(); ++i) {
      if (!consumed[i] && sameSignature(violation, right[i], siteWidth)) {
        consumed[i] = 1;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }
  return true;
}

std::string windowLabel(const RepairWindow& window)
{
  return window.level == 0 ? "L0" : cat("adaptive-L1 step ", window.level);
}

}  // namespace

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

  // Stages 3..7 under the adaptive window loop (spec 6.3/6.7/6.8,
  // V2.1 #8): search L0, then grow K fillers toward the best non-clean
  // candidate's blocking side until clean or an expansion cutoff.
  OracleGate::SearchResult best;  // best non-clean across windows (diagnostics)
  // Definitive iff the LAST window we actually searched was fully enumerated
  // (V2.1 #10): an earlier smaller window being complete does not prove the
  // later truncated window has no solution.
  bool lastSearchedDefinitive = false;
  std::vector<Violation> previousBlocking;
  bool havePreviousBlocking = false;
  RepairWindow window = buildWindow(0,
                                    request.targetPlace,
                                    violations,
                                    view_,
                                    ruleDistance,
                                    log_);

  for (;;) {
    const std::string label = windowLabel(window);
    std::vector<Violation> blockingForExpansion = request.violations;
    bool searched = false;
    bool currentDefinitive = false;

    if (window.editableFillers.empty()) {
      result.diagnostics.push_back(makeDiag(
          Severity::Warning, "NoEditableFiller",
          cat("window ", label, " ", show(window.area()),
              " contains no editable filler")));
    } else {
      const SwapGenerationResult generated =
          generateSwaps(window, view_, candidates_, log_);
      result.diagnostics.insert(result.diagnostics.end(),
                                generated.diagnostics.begin(),
                                generated.diagnostics.end());
      if (generated.swaps.empty()) {
        result.diagnostics.push_back(makeDiag(
            Severity::Warning, "NoSwapGenerated",
            cat("window ", label, ": no usable swap")));
      } else {
        const std::vector<FillerDomain> ranked =
            rankFillers(generated.swaps,
                        request.targetPlace,
                        violations,
                        window,
                        view_,
                        log_);

        int budget = config_.checkerCallBudgetPerWindow;
        if (!gate.runBaseline(window, budget)) {
          result.hasSolution = false;
          result.diagnostics.insert(result.diagnostics.end(),
                                    gate.diagnostics().begin(),
                                    gate.diagnostics().end());
          result.diagnostics.push_back(makeDiag(
              Severity::Fatal, "BaselineGateFailed",
              cat("baseline gate failed at window ", label,
                  " (see BaselineUnusable/BaselineMismatch above)")));
          log_.msg("engine", "baseline gate failed -> abort");
          return result;
        }

        const EnumerationPlan plan =
            enumerateOverlays(ranked, config_, budget, log_);
        OracleGate::SearchResult sr =
            gate.search(plan.overlays, window, window.guardRegion, budget);
        searched = true;
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
          result.hasSolution = true;
          result.changes = toFillerChanges(sr.cleanOverlay);
          result.diagnostics.push_back(makeDiag(
              Severity::Info, "Solution",
              cat("window ", label, ": ", result.changes.size(),
                  " change(s); checker requests=", gate.requestsSent(),
                  " batches=", gate.batchesSent(),
                  " cacheHits=", gate.cacheHits())));
          log_.msg("engine",
                   cat("SOLUTION at ", label, ": ", result.changes.size(),
                       " change(s), requests=", gate.requestsSent(),
                       " cacheHits=", gate.cacheHits()));
          return result;
        }

        if (betterBest(sr, best)) {
          best = sr;
        }
        currentDefinitive = plan.complete && !sr.budgetExhausted;
        lastSearchedDefinitive = currentDefinitive;
        if (sr.hasBest && !sr.bestSummary.blockingViolations.empty()) {
          blockingForExpansion = sr.bestSummary.blockingViolations;
        }
        result.diagnostics.push_back(makeDiag(
            Severity::Info, "WindowExhausted",
            cat("window ", label, ": ", plan.overlays.size(),
                " candidate(s), ",
                currentDefinitive
                    ? "complete enumeration, definitively no clean overlay"
                    : "truncated (size caps or budget), no clean overlay found")));
        log_.msg("engine",
                 cat("window ", label, " no clean overlay (",
                     currentDefinitive ? "definitive" : "truncated",
                     ") -> adaptive expansion"));
      }
    }

    if (searched && window.level > 0 && currentDefinitive
        && havePreviousBlocking
        && sameBlockingMultiset(blockingForExpansion,
                                previousBlocking,
                                view_.siteWidth())) {
      result.diagnostics.push_back(makeDiag(
          Severity::Info, "ExpansionCutoff",
          cat("window ", label,
              " completed enumeration with unchanged blocking violations -> "
              "stop adaptive expansion")));
      log_.msg("engine",
               cat("window ", label,
                   " blocking multiset unchanged after complete search -> cutoff"));
      break;
    }

    previousBlocking = blockingForExpansion;
    havePreviousBlocking = true;
    const RepairWindow expanded = expandWindowAdaptive(
        window,
        request.targetPlace,
        blockingForExpansion,
        view_,
        config_.adaptiveStepFillers,
        log_);
    if (expanded.editableFillers == window.editableFillers) {
      result.diagnostics.push_back(makeDiag(
          Severity::Info, "ExpansionCutoff",
          cat("window ", label,
              " adaptive step adds no new editable filler -> stop escalation")));
      log_.msg("engine",
               cat("window ", label,
                   " adaptive step adds no filler -> expansion cutoff"));
      break;
    }
    window = expanded;
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
