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

#include "Log.h"
#include "Signature.h"
#include "Swap.h"
#include "Types.h"
#include "Window.h"

namespace dpl2::fillerRepair {

struct RepairConfig;

// Planner-internal checker protocol. Production callers never see these
// requestId/status types; FillerRepairEngine translates ordered final-checker
// CheckResult/FillerChanges at its private boundary. Test fakes implement this
// interface to inject protocol failures and exact search states.
class ImplantOverlayChecker
{
 public:
  virtual ~ImplantOverlayChecker() = default;

  virtual CheckResult checkPlaceWithOverlay(
      const OverlayCheckRequest& request) = 0;
  virtual std::vector<CheckResult> checkPlaceWithOverlays(
      const std::vector<OverlayCheckRequest>& requests) = 0;
};

inline bool isRawCheckerSnapshotClean(const CheckResult& result)
{
  return result.status == CheckStatus::Checked && result.isLegal
         && result.violations.empty();
}

inline bool isCheckedResultUsable(const CheckResult& result)
{
  return result.status == CheckStatus::Checked
         && (result.isLegal || !result.violations.empty()
             || !result.diagnostics.empty());
}

// Delta classification of one checker result against the baseline.
struct DeltaSummary
{
  bool usable = false;
  bool inconsistent = false;   // isLegal disagrees with violations-empty (#1)
  int residualOriginals = 0;   // originals still matched in the result
  int newInWindow = 0;
  int relatedInHalo = 0;
  int unrelatedInHalo = 0;     // reported, never blocking
  // Actual residual/new-related findings that prevented acceptance. Adaptive
  // L1 uses their rows/x windows to choose the next growth side (V2.1 #8).
  std::vector<Violation> blockingViolations;
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

  // Baseline for `window.guardRegion`; consumes budget only on a cache miss.
  // False when the baseline is unusable (checker error) OR fails the baseline
  // consistency gate (spec 6.8, V2.1 #2+#4): the baseline must reproduce every
  // original that lies inside the guard, and must not carry an unexpected
  // in-window violation that was not in the input snapshot. A false return is
  // fatal for the window -- either a checker error or a stale/inconsistent
  // snapshot, both of which the engine must not silently treat as "repaired".
  bool runBaseline(const RepairWindow& window, int& budget);

  struct SearchResult
  {
    bool foundClean = false;
    Overlay cleanOverlay;
    bool protocolError = false;
    bool budgetExhausted = false;
    // Best non-clean candidate, also used to steer adaptive-L1 growth.
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
  // Baseline consistency gate (spec 6.8, V2.1 #2+#4). Uses the already-fetched
  // baseline_, spends no budget. Pushes a fatal BaselineMismatch diagnostic and
  // returns false when the snapshot is stale/inconsistent.
  bool checkBaselineConsistency(const RepairWindow& window);

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
