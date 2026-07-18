// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "OracleGate.h"

#include "PlannerEngine.h"

#include <algorithm>
#include <set>

namespace dpl2::fillerRepair {

namespace {

// A violation falls inside the repair window when its x overlaps the editable
// span and at least one of its rows is editable (spec 6.8 rule 3). Empty rows
// -> not in-window, matching the checker-provided-rows fallback.
bool inRepairWindow(const Violation& v, const RepairWindow& window)
{
  if (!v.xWindow.overlaps(window.x)) {
    return false;
  }
  for (const RowId row : v.rowIds) {
    if (std::find(window.rows.begin(), window.rows.end(), row)
        != window.rows.end()) {
      return true;
    }
  }
  return false;
}

// A violation is observable in a baseline collected within `guard` when it
// overlaps the guard geometrically. Used to scope the baseline-reproduces-
// originals check: an original outside the current guard is simply not this
// window's responsibility (it is covered once the window grows).
bool inGuardRegion(const Violation& v, const Region& guard)
{
  if (!v.xWindow.overlaps(guard.x)) {
    return false;
  }
  if (v.rowIds.empty()) {
    return true;  // no row info -> conservative: treat as in-guard by x
  }
  for (const RowId row : v.rowIds) {
    if (row >= guard.rowLo && row <= guard.rowHi) {
      return true;
    }
  }
  return false;
}

}  // namespace

OracleGate::OracleGate(ImplantOverlayChecker& checker,
                       const TargetPlace& anchor,
                       const std::vector<Violation>& originals,
                       DbCoord siteWidth,
                       DbCoord ruleDistance,
                       const RepairConfig& config,
                       const DebugLog& log)
    : checker_(checker),
      anchor_(anchor),
      originals_(originals),
      site_width_(siteWidth),
      rule_distance_(ruleDistance),
      config_(config),
      log_(log)
{
}

std::string OracleGate::cacheKey(const Region& guard, const Overlay& overlay) const
{
  // Guard region is part of the key: the same overlay under a different
  // guard is a different checker question.
  return cat('g', guard.x.xl, ':', guard.x.xh, ':', guard.rowLo, ':',
             guard.rowHi, '|', canonicalKey(overlay));
}

bool OracleGate::runBaseline(const RepairWindow& window, int& budget)
{
  const Region& guard = window.guardRegion;
  const std::string key = cacheKey(guard, {});
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    ++cache_hits_;
    log_.msg("gate", cat("baseline cache hit for guard ", show(guard)));
  } else {
    if (budget <= 0) {
      return false;
    }
    OverlayCheckRequest request;
    request.requestId = next_request_id_++;
    request.targetPlace = anchor_;
    request.guardRegion = guard;
    const CheckResult result = checker_.checkPlaceWithOverlay(request);
    ++requests_sent_;
    --budget;
    if (result.requestId != request.requestId) {
      diagnostics_.push_back(makeDiag(
          Severity::Fatal, "CheckerProtocolError",
          cat("baseline echoed id ", result.requestId, " != ", request.requestId)));
      return false;
    }
    it = cache_.emplace(key, result).first;
  }

  baseline_ = &it->second;
  if (baseline_->status != CheckStatus::Checked) {
    diagnostics_.push_back(makeDiag(Severity::Error, "BaselineUnusable",
                                    "baseline check did not complete"));
    baseline_ = nullptr;
    return false;
  }
  log_.msg("gate",
           cat("baseline for guard ", show(guard), ": ",
               baseline_->violations.size(), " violation(s)"));

  // Baseline consistency gate: refuse to search on a stale/inconsistent
  // snapshot (spec 6.8, V2.1 #2+#4).
  if (!checkBaselineConsistency(window)) {
    baseline_ = nullptr;
    return false;
  }
  return true;
}

bool OracleGate::checkBaselineConsistency(const RepairWindow& window)
{
  // One-to-one bookkeeping so a single baseline finding cannot satisfy two
  // originals (and, below, cannot be double-counted as unexpected).
  std::vector<char> consumed(baseline_->violations.size(), 0);

  // (a) Every original inside the guard must reproduce in the baseline. If it
  //     does not, the input snapshot is stale/inconsistent and a candidate
  //     that merely "does not observe" it would be mistaken for a repair.
  int inGuardOriginals = 0;
  for (const Violation& original : originals_) {
    if (!inGuardRegion(original, window.guardRegion)) {
      continue;  // outside this window's scope; a larger window will cover it
    }
    ++inGuardOriginals;
    bool matched = false;
    for (size_t i = 0; i < baseline_->violations.size(); ++i) {
      if (!consumed[i]
          && sameSignature(original, baseline_->violations[i], site_width_)) {
        consumed[i] = 1;
        matched = true;
        break;
      }
    }
    if (!matched) {
      diagnostics_.push_back(makeDiag(
          Severity::Fatal, "BaselineMismatch",
          cat("original rule=", original.ruleId, ' ', show(original.xWindow),
              " lies in the guard but is absent from the baseline; input "
              "snapshot is stale/inconsistent -- refusing to search")));
      log_.msg("gate",
               cat("baseline MISMATCH: original ", show(original.xWindow),
                   " not reproduced -> abort window"));
      return false;
    }
  }

  // (b) No unexpected in-window violation may pre-exist in the baseline: by the
  //     §2.3 assumption the input snapshot is clean apart from the originals,
  //     so an unmatched baseline finding inside the repair window signals an
  //     inconsistent snapshot. Unmatched findings OUTSIDE the window are the
  //     allowed unrelated pre-existing halo (spec 6.8 rule 5).
  for (size_t i = 0; i < baseline_->violations.size(); ++i) {
    if (consumed[i]) {
      continue;
    }
    const Violation& b = baseline_->violations[i];
    if (inRepairWindow(b, window)) {
      diagnostics_.push_back(makeDiag(
          Severity::Fatal, "BaselineMismatch",
          cat("baseline carries an in-window violation rule=", b.ruleId, ' ',
              show(b.xWindow), " that is not among the original snapshot; "
              "input is inconsistent -- refusing to search")));
      log_.msg("gate",
               cat("baseline MISMATCH: unexpected in-window ", show(b.xWindow),
                   " -> abort window"));
      return false;
    }
  }

  log_.msg("gate",
           cat("baseline consistent: ", inGuardOriginals,
               " in-guard original(s) all reproduced, no unexpected in-window "
               "violation"));
  return true;
}

DeltaSummary OracleGate::classify(const CheckResult& result,
                                  const Overlay& overlay,
                                  const RepairWindow& window) const
{
  DeltaSummary summary;
  summary.usable = result.status == CheckStatus::Checked;
  for (const Diagnostic& diag : result.diagnostics) {
    summary.usable &= diag.severity != Severity::Fatal;
  }
  if (!summary.usable) {
    return summary;
  }
  // Self-consistency both ways (V2.1 #1): isLegal must agree with whether the
  // result reports violations. `isLegal && violations non-empty` AND
  // `!isLegal && violations empty` (an unexplained illegal result, which the
  // real checker returns for blocking overlaps / off-grid / polarity) are both
  // rejected as not clean.
  summary.inconsistent = (result.isLegal != result.violations.empty());

  // Residual originals: match each original to a DISTINCT result finding, so
  // two originals cannot both claim the same one (V2.1 #3, one-to-one).
  {
    std::vector<char> consumed(result.violations.size(), 0);
    for (const Violation& original : originals_) {
      for (size_t i = 0; i < result.violations.size(); ++i) {
        if (!consumed[i]
            && sameSignature(original, result.violations[i], site_width_)) {
          consumed[i] = 1;
          ++summary.residualOriginals;
          summary.blockingViolations.push_back(result.violations[i]);
          break;
        }
      }
    }
  }

  // New violations = result findings not matched one-to-one against the
  // baseline (V2.1 #3): one baseline finding absorbs at most one candidate
  // finding, so a second same-signature finding is correctly counted as new
  // (P/N bands + the one-site signature tolerance make duplicates real). Inside
  // the repair window a new violation always rejects; in the guard halo only
  // when related to this overlay.
  std::vector<char> baselineConsumed(baseline_->violations.size(), 0);
  for (const Violation& v : result.violations) {
    bool preExisting = false;
    for (size_t i = 0; i < baseline_->violations.size(); ++i) {
      if (!baselineConsumed[i]
          && sameSignature(v, baseline_->violations[i], site_width_)) {
        baselineConsumed[i] = 1;
        preExisting = true;
        break;
      }
    }
    if (preExisting) {
      continue;
    }
    if (inRepairWindow(v, window)) {
      ++summary.newInWindow;
      summary.blockingViolations.push_back(v);
    } else if (isRelatedToOverlay(v, overlay,
                                  std::max(rule_distance_, v.requiredValue))) {
      // Per-violation rule distance (V2.1 #5): a new violation from a
      // larger-distance rule must not be mislabeled unrelated and let through.
      ++summary.relatedInHalo;
      summary.blockingViolations.push_back(v);
    } else {
      ++summary.unrelatedInHalo;
    }
  }

  summary.clean = !summary.inconsistent && summary.residualOriginals == 0
                  && summary.newInWindow == 0 && summary.relatedInHalo == 0;
  return summary;
}

const CheckResult* OracleGate::resolve(const std::vector<Overlay>& chunk,
                                       const Region& guard,
                                       int& budget,
                                       bool& protocolError)
{
  // Send everything in the chunk that is not cached yet as one batch.
  std::vector<OverlayCheckRequest> requests;
  std::vector<std::string> keys;
  for (const Overlay& overlay : chunk) {
    const std::string key = cacheKey(guard, overlay);
    if (cache_.count(key) > 0 || budget <= 0) {
      if (cache_.count(key) > 0) {
        ++cache_hits_;
      }
      continue;
    }
    OverlayCheckRequest request;
    request.requestId = next_request_id_++;
    request.targetPlace = anchor_;
    request.guardRegion = guard;
    request.fillerChanges = toFillerChanges(overlay);
    requests.push_back(std::move(request));
    keys.push_back(key);
    --budget;
  }
  if (!requests.empty()) {
    const std::vector<CheckResult> results =
        checker_.checkPlaceWithOverlays(requests);
    ++batches_sent_;
    requests_sent_ += static_cast<int>(requests.size());

    // Protocol validation: one result per request, ids echo exactly once,
    // no unknown ids. Order must NOT matter -- map back by id.
    if (results.size() != requests.size()) {
      diagnostics_.push_back(makeDiag(
          Severity::Fatal, "CheckerProtocolError",
          cat("batch returned ", results.size(), " result(s) for ",
              requests.size(), " request(s)")));
      protocolError = true;
      return nullptr;
    }
    std::map<OverlayRequestId, const CheckResult*> byId;
    for (const CheckResult& result : results) {
      if (!byId.emplace(result.requestId, &result).second) {
        diagnostics_.push_back(makeDiag(Severity::Fatal, "CheckerProtocolError",
                                        cat("duplicate requestId ", result.requestId)));
        protocolError = true;
        return nullptr;
      }
    }
    for (size_t i = 0; i < requests.size(); ++i) {
      const auto it = byId.find(requests[i].requestId);
      if (it == byId.end()) {
        diagnostics_.push_back(makeDiag(Severity::Fatal, "CheckerProtocolError",
                                        cat("missing result for requestId ",
                                            requests[i].requestId)));
        protocolError = true;
        return nullptr;
      }
      cache_.emplace(keys[i], *it->second);
    }
  }
  return baseline_;  // non-null marker; per-overlay lookup goes via cache_
}

OracleGate::SearchResult OracleGate::search(const std::vector<Overlay>& candidates,
                                            const RepairWindow& window,
                                            const Region& guard,
                                            int& budget)
{
  SearchResult sr;

  size_t next = 0;
  while (next < candidates.size()) {
    const size_t chunkEnd =
        std::min(candidates.size(),
                 next + static_cast<size_t>(config_.batchSize));
    const std::vector<Overlay> chunk(candidates.begin() + next,
                                     candidates.begin() + chunkEnd);
    bool protocolError = false;
    if (resolve(chunk, guard, budget, protocolError) == nullptr && protocolError) {
      sr.protocolError = true;
      return sr;
    }

    // Evaluate the chunk in enumeration order; first delta-clean wins.
    for (size_t i = next; i < chunkEnd; ++i) {
      const auto it = cache_.find(cacheKey(guard, candidates[i]));
      if (it == cache_.end()) {
        sr.budgetExhausted = true;  // was not evaluated: out of budget
        continue;
      }
      const DeltaSummary summary = classify(it->second, candidates[i], window);
      if (summary.clean) {
        sr.foundClean = true;
        sr.cleanOverlay = candidates[i];
        log_.msg("gate",
                 cat("clean overlay #", i, " (", candidates[i].size(),
                     " swap(s)) residual=0 newInWindow=0 relatedInHalo=0 ",
                     "unrelatedInHalo=", summary.unrelatedInHalo,
                     " -> accept"));
        return sr;
      }
      // Track best non-clean for diagnostics (fewer blocking findings, then
      // fewer changes, then earlier enumeration index).
      const int blockers = summary.residualOriginals + summary.newInWindow
                           + summary.relatedInHalo + (summary.usable ? 0 : 1000);
      const int bestBlockers = sr.bestSummary.residualOriginals
                               + sr.bestSummary.newInWindow
                               + sr.bestSummary.relatedInHalo
                               + (sr.bestSummary.usable ? 0 : 1000);
      if (!sr.hasBest || blockers < bestBlockers
          || (blockers == bestBlockers
              && candidates[i].size() < sr.bestOverlay.size())) {
        sr.hasBest = true;
        sr.bestOverlay = candidates[i];
        sr.bestSummary = summary;
      }
    }
    next = chunkEnd;
    if (budget <= 0) {
      sr.budgetExhausted = next < candidates.size();
      break;
    }
  }
  return sr;
}

}  // namespace dpl2::fillerRepair
