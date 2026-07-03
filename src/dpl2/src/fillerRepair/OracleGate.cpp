// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "OracleGate.h"

#include <algorithm>
#include <set>

namespace dpl2::fillerRepair {

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

bool OracleGate::runBaseline(const Region& guard, int& budget)
{
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
  summary.inconsistent = result.isLegal && !result.violations.empty();

  // Rule: every original must be gone (signature match, spec 6.2).
  for (const Violation& original : originals_) {
    for (const Violation& v : result.violations) {
      if (sameSignature(original, v, site_width_)) {
        ++summary.residualOriginals;
        break;
      }
    }
  }

  // New violations = not matched in the baseline. Inside the repair window
  // they always reject; in the guard halo only when related to this overlay.
  for (const Violation& v : result.violations) {
    bool preExisting = false;
    for (const Violation& b : baseline_->violations) {
      if (sameSignature(v, b, site_width_)) {
        preExisting = true;
        break;
      }
    }
    if (preExisting) {
      continue;
    }
    bool inWindow = v.xWindow.overlaps(window.x);
    if (inWindow) {
      bool rowInWindow = false;
      for (const RowId row : v.rowIds) {
        rowInWindow |= std::find(window.rows.begin(), window.rows.end(), row)
                       != window.rows.end();
      }
      inWindow = rowInWindow;
    }
    if (inWindow) {
      ++summary.newInWindow;
    } else if (isRelatedToOverlay(v, overlay, rule_distance_)) {
      ++summary.relatedInHalo;
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

bool OracleGate::finalCheck(const Overlay& overlay,
                            const RepairWindow& window,
                            const Region& guard,
                            int& budget)
{
  bool protocolError = false;
  if (resolve({overlay}, guard, budget, protocolError) == nullptr && protocolError) {
    return false;
  }
  const auto it = cache_.find(cacheKey(guard, overlay));
  if (it == cache_.end()) {
    return false;  // out of budget -- cannot certify
  }
  const DeltaSummary summary = classify(it->second, overlay, window);
  log_.msg("gate",
           cat("final full-overlay check: ", overlay.size(), " swap(s) -> ",
               summary.clean ? "clean" : "NOT clean"));
  return summary.clean;
}

}  // namespace dpl2::fillerRepair
