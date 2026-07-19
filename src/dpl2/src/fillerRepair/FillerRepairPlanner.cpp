// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "FillerRepairPlanner.h"

#include "OracleGate.h"
#include "Ranker.h"
#include "Signature.h"
#include "SubsetSearch.h"
#include "Swap.h"
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

std::string windowLabel(const RepairWindow& window)
{
  return window.level == 0 ? "L0" : cat("adaptive-L1 step ", window.level);
}

}  // namespace

namespace internal {

FillerRepairPlanner::FillerRepairPlanner(
    const PlannerDataSource& view,
    PlannerOracle& oracle,
    RepairConfig config)
    : view_(view),
      oracle_(oracle),
      config_(config),
      log_(config.verbose)
{
}

FillerRepairResult FillerRepairPlanner::repair(
    const FillerRepairRequest& request)
{
  FillerRepairResult result;

  // Spec 3.3: the overlay API is a pure query and must never call back into
  // repair, and one planner instance never runs two repairs at once
  // (concurrent repairs = one planner per thread over a shared immutable
  // view). Turn a violation into a fatal result instead of corrupted state.
  if (repair_active_.exchange(true, std::memory_order_acq_rel)) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "ReentrantRepair",
        "repair() re-entered on this planner instance (oracle callback or "
        "concurrent use) -> refused"));
    return result;
  }
  struct ActiveGuard
  {
    std::atomic<bool>& flag;
    ~ActiveGuard() { flag.store(false, std::memory_order_release); }
  } activeGuard{repair_active_};

  // The transcript starts with both the immutable request and every search
  // knob. This makes a runtime failure reproducible from one captured log
  // without relying on hidden defaults.
  log_.msg("planner",
           cat("repair start: anchor inst=", request.targetPlace.instanceId,
               " master=", request.targetPlace.masterId,
               " row=", request.targetPlace.rowId,
               " x=", request.targetPlace.x,
               " violations=", request.violations.size()));
  log_.msg("planner",
           cat("config: budget/window=", config_.checkerCallBudgetPerWindow,
               " batch=", config_.batchSize,
               " maxSubset=", config_.maxSubsetSize,
               " memberCaps=[", config_.memberCapSize2, ',',
               config_.memberCapSize3, ',', config_.memberCapSize4,
               "] adaptiveStep=", config_.adaptiveStepFillers,
               " maxAdaptiveLevels=", config_.maxAdaptiveLevels));

  // Placement coverage is intentionally not checked here. Runtime opto
  // calls FillerRepairEngine::precheck() before any mutation; keeping that
  // gate out of repair preserves the explicit orchestration contract.
  // Empty snapshot: nothing to repair is a success with no changes.
  if (request.violations.empty()) {
    result.hasSolution = true;
    result.diagnostics.push_back(makeDiag(
        Severity::Info, "EmptySnapshot", "no violations in initial snapshot"));
    log_.msg("planner",
             "empty violation snapshot -> hasSolution=true, 0 changes");
    return result;
  }

  // Stage 2 (spec 6.2): normalize the snapshot into signatures/footprints.
  const std::vector<NormalizedViolation> violations =
      normalizeViolations(request, view_, log_);

  const DbCoord ruleDistance =
      estimateRuleDistance(request.violations, view_.siteWidth());
  log_.msg("planner",
           cat("normalized=", violations.size(), " siteWidth=",
               view_.siteWidth(), " ruleDistance=", ruleDistance,
               " -> build L0 window"));
  OracleGate gate(view_, oracle_, request.targetPlace, request.violations,
                  view_.siteWidth(), ruleDistance, config_, log_);

  // Stages 3..7 under the adaptive window loop (spec 6.3/6.7/6.8,
  // V2.1 #8): search L0, then grow K fillers toward the best non-clean
  // candidate's blocking side until clean or an expansion cutoff.
  OracleGate::SearchResult best;  // best non-clean across windows (diagnostics)
  // Definitive iff the LAST window we actually searched was fully enumerated
  // (V2.1 #10): an earlier smaller window being complete does not prove the
  // later truncated window has no solution.
  bool lastSearchedDefinitive = false;
  RepairWindow window = buildWindow(0,
                                    request.targetPlace,
                                    violations,
                                    view_,
                                    ruleDistance,
                                    log_);

  for (;;) {
    const std::string label = windowLabel(window);
    std::vector<Violation> blockingForExpansion = request.violations;
    bool currentDefinitive = false;

    // One loop iteration is one independently budgeted search question. The
    // window and guard are logged before generating swaps so a transcript can
    // explain exactly which fillers were editable versus check-only.
    log_.msg("planner",
             cat("search ", label, ": area=", show(window.area()),
                 " guard=", show(window.guardRegion), " editable=",
                 window.editableFillers.size(), " bridge=",
                 window.bridgeFillers.size()));

    if (window.editableFillers.empty()) {
      result.diagnostics.push_back(makeDiag(
          Severity::Warning, "NoEditableFiller",
          cat("window ", label, " ", show(window.area()),
              " contains no editable filler")));
    } else {
      const SwapGenerationResult generated =
          generateSwaps(window, view_, log_);
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
          log_.msg("planner", "baseline gate failed -> abort");
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
          log_.msg("planner", "checker protocol error -> abort");
          return result;
        }

        if (sr.foundClean) {
          result.hasSolution = true;
          result.changes = toFillerChanges(sr.cleanOverlay, view_);
          result.diagnostics.push_back(makeDiag(
              Severity::Info, "Solution",
              cat("window ", label, ": ", result.changes.size(),
                  " change(s); checker requests=", gate.requestsSent(),
                  " batches=", gate.batchesSent(),
                  " cacheHits=", gate.cacheHits())));
          log_.msg("planner",
                   cat("SOLUTION at ", label, ": ", result.changes.size(),
                       " change(s), requests=", gate.requestsSent(),
                       " cacheHits=", gate.cacheHits()));
          return result;
        }

        if (betterBest(sr, best)) {
          best = sr;
          log_.msg("planner",
                   cat("best-so-far updated at ", label, ": swaps=",
                       best.bestOverlay.size(), " residual=",
                       best.bestSummary.residualOriginals, " newInWindow=",
                       best.bestSummary.newInWindow, " relatedInHalo=",
                       best.bestSummary.relatedInHalo));
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
        log_.msg("planner",
                 cat("window ", label, " no clean overlay (",
                     currentDefinitive ? "definitive" : "truncated",
                     ") -> adaptive expansion"));
      }
    }

    // A stable blocking set is not a proof that farther fillers cannot form a
    // clean non-monotone multi-swap. Keep expanding until no adjacent filler
    // can be added, the level cap fires, or the normal per-window search
    // limits stop enumeration.
    if (window.level >= config_.maxAdaptiveLevels) {
      result.diagnostics.push_back(makeDiag(
          Severity::Info, "ExpansionCutoff",
          cat("window ", label, " reached maxAdaptiveLevels=",
              config_.maxAdaptiveLevels, " -> stop escalation (truncated)")));
      log_.msg("planner",
               cat("window ", label, " adaptive level cap ",
                   config_.maxAdaptiveLevels, " -> truncated"));
      // Farther windows were never searched, so no-solution is not definitive.
      lastSearchedDefinitive = false;
      break;
    }
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
      log_.msg("planner",
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
  log_.msg("planner",
           cat("NO SOLUTION (", lastSearchedDefinitive ? "definitive" : "truncated",
               "), requests=", gate.requestsSent(),
               " cacheHits=", gate.cacheHits()));
  return result;
}

}  // namespace internal
}  // namespace dpl2::fillerRepair
