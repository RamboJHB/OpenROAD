// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Oracle gate (spec sections 6.8 / 4.2, TODO 9): the only place that talks
// to the checker during the search.
//
// Responsibilities:
//  - baseline request per guard region (empty fillerChanges);
//  - batch overlay checks with strict protocol validation (requestId echo,
//    no missing/duplicate/unknown ids; order independence);
//  - result cache keyed by (guardRegion, overlay cache key): the same
//    overlay under the same guard hits the checker exactly once, across
//    window escalations too;
//  - baseline-delta classification (the ONLY accept gate):
//      clean <=> status Checked, no fatal diagnostics, not inconsistent
//                (isLegal with violations), all original violations gone,
//                no new violation inside the repair window, no related new
//                violation in the guard halo. Unrelated pre-existing halo
//                violations never fail a candidate (spec 6.8 rule 5).

#pragma once

#include <map>
#include <string>
#include <vector>

#include "CheckerApi.h"
#include "FillerRepairEngine.h"
#include "Log.h"
#include "Signature.h"
#include "Swap.h"
#include "Window.h"

namespace dpl2::fillerRepair {

// Delta classification of one checker result against the baseline.
struct DeltaSummary
{
  bool usable = false;
  bool inconsistent = false;   // Checked && isLegal && violations non-empty
  int residualOriginals = 0;   // originals still matched in the result
  int newInWindow = 0;
  int relatedInHalo = 0;
  int unrelatedInHalo = 0;     // reported, never blocking
  bool clean = false;
};

class OracleGate
{
 public:
  OracleGate(ImplantOverlayChecker& checker,
             const TargetPlace& anchor,
             const std::vector<Violation>& originals,
             DbCoord siteWidth,
             DbCoord ruleDistance,
             const RepairConfig& config,
             const DebugLog& log);

  // Baseline for `guard`; consumes budget only on a cache miss. False when
  // the baseline is unusable (checker error) -- the window cannot be gated.
  bool runBaseline(const Region& guard, int& budget);

  struct SearchResult
  {
    bool foundClean = false;
    Overlay cleanOverlay;
    bool protocolError = false;
    bool budgetExhausted = false;
    // Best non-clean candidate, for diagnostics only.
    bool hasBest = false;
    Overlay bestOverlay;
    DeltaSummary bestSummary;
  };

  // Evaluates candidates in enumeration order, batched; early exit on the
  // first delta-clean overlay. Consumes budget per checker-evaluated request.
  SearchResult search(const std::vector<Overlay>& candidates,
                      const RepairWindow& window,
                      const Region& guard,
                      int& budget);

  // Final full-overlay check (spec 6.4): same overlay + guard; served from
  // the cache when identical to the winning search request.
  bool finalCheck(const Overlay& overlay,
                  const RepairWindow& window,
                  const Region& guard,
                  int& budget);

  int requestsSent() const { return requests_sent_; }
  int batchesSent() const { return batches_sent_; }
  int cacheHits() const { return cache_hits_; }
  const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

 private:
  std::string cacheKey(const Region& guard, const Overlay& overlay) const;
  // nullptr on protocol error / budget exhaustion (flags set accordingly).
  const CheckResult* resolve(const std::vector<Overlay>& chunk,
                             const Region& guard,
                             int& budget,
                             bool& protocolError);
  DeltaSummary classify(const CheckResult& result,
                        const Overlay& overlay,
                        const RepairWindow& window) const;

  ImplantOverlayChecker& checker_;
  const TargetPlace& anchor_;
  const std::vector<Violation>& originals_;
  DbCoord site_width_;
  DbCoord rule_distance_;
  const RepairConfig& config_;
  const DebugLog& log_;

  std::map<std::string, CheckResult> cache_;
  const CheckResult* baseline_ = nullptr;  // points into cache_
  OverlayRequestId next_request_id_ = 0;
  int requests_sent_ = 0;
  int batches_sent_ = 0;
  int cache_hits_ = 0;
  std::vector<Diagnostic> diagnostics_;
};

}  // namespace dpl2::fillerRepair
