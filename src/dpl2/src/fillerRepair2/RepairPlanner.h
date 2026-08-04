// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The search. Opto retargeted one standard cell to a different VT; the

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fillerRepair/Debug.h>
#include <fillerRepair/PlacementView.h>
#include <fillerRepair/RepairOracle.h>
#include <fillerRepair/RepairTypes.h>

namespace dpl2::fillerRepair {

// Every search knob, in one place, so the transcript can print the exact
struct RepairConfig
{
// Checker calls one window may spend, the baseline request included.
  int checkerCallBudgetPerWindow = 512;
// Checker calls ONE repair() may spend in total. Without it the worst case
  int checkerCallBudgetPerRepair = 2048;
  int batchSize = 32;
// How many fillers one candidate may change at once. Only bites on windows
  int maxSubsetSize = 4;
// ... and, for each of those sizes, how many of the best-ranked fillers may
  int memberCapSize2 = 24;
  int memberCapSize3 = 12;
  int memberCapSize4 = 8;
// Fillers one growth step adds, per row, per side.
  int adaptiveStepFillers = 2;
// How many times the window may grow before giving up. Without it a
  int maxAdaptiveLevels = 32;
  bool verbose = true;            // [fr] transcript; FR_VERBOSE=0 silences
};

// --- Swap: one filler changes master ---------------------------------------

struct RepairWindow;

struct Swap
{
  InstanceId instanceId = 0;
  MasterId oldMasterId = 0;
  MasterId newMasterId = 0;
  RowId rowId = 0;
  XInterval span;
  VtId oldVt = kUnknownVt;
  VtId newVt = kUnknownVt;
};

using Overlay = std::vector<Swap>;

// The one move the planner can make. Refuses anything else: the instance must
std::optional<Swap> makeSwap(const PlacementView& view,
                             InstanceId instanceId,
                             MasterId newMasterId,
                             std::string* error = nullptr);

// --- Overlay identity ------------------------------------------------------
struct OverlayKey
{
  using Entry = std::pair<InstanceId, MasterId>;
// One key per candidate, holding one entry per filler that candidate
  static constexpr std::size_t kInlineSwaps = 8;

  DbCoord guardXl = 0;
  DbCoord guardXh = 0;
  RowId guardRowLo = 0;
  RowId guardRowHi = 0;

  std::size_t count = 0;
  std::array<Entry, kInlineSwaps> inlineSwaps{};
  std::vector<Entry> overflow;  // used iff count > kInlineSwaps

  const Entry* data() const
  {
    return count > kInlineSwaps ? overflow.data() : inlineSwaps.data();
  }
  Entry* data()
  {
    return count > kInlineSwaps ? overflow.data() : inlineSwaps.data();
  }
  std::size_t size() const { return count; }
  const Entry* begin() const { return data(); }
  const Entry* end() const { return data() + count; }

// Grows to `n` entries; the caller then writes them through data().
  void resize(std::size_t n)
  {
    if (n > kInlineSwaps) {
      overflow.resize(n);
      if (count <= kInlineSwaps) {
        std::copy(inlineSwaps.begin(), inlineSwaps.begin() + count,
                  overflow.begin());
      }
    }
    count = n;
  }

  bool operator==(const OverlayKey& other) const
  {
    return guardXl == other.guardXl && guardXh == other.guardXh
           && guardRowLo == other.guardRowLo && guardRowHi == other.guardRowHi
           && count == other.count
           && std::equal(begin(), end(), other.begin());
  }
  bool operator!=(const OverlayKey& other) const { return !(*this == other); }
};

struct OverlayKeyHash
{
  std::size_t operator()(const OverlayKey& key) const;
};

OverlayKey overlayKey(const Region& guard, const Overlay& overlay);

// Wire conversion, deterministic order (sorted by instanceId).
ipl::FillerChanges toFillerChanges(const Overlay& overlay,
                                   const PlacementView& dataSource);

struct SwapGenerationResult
{
// Deterministic order: window editable order (row, x), then candidate
  std::vector<Swap> swaps;
// NoUsableMaster per replacement-less filler, plus any provider
  std::vector<Diagnostic> diagnostics;
};

SwapGenerationResult generateSwaps(
    const RepairWindow& window,
    const PlacementView& view,
    const DebugLog& log);

// --- what the checker reported, in the planner's terms ----------------------

// One checker violation with the few things the planner keeps asking about
struct NormalizedViolation
{
  Violation raw;

  std::vector<RowId> rowIds;  // sorted unique, never empty
  bool rowIdFallback = false;  // rowIds were missing; anchor row substituted

// How much x this violation actually covers: its own xWindow plus the span
  XInterval xRange;

// Who is involved, split by what we can do about them: cells we cannot
  std::vector<InstanceId> cellAnchors;
  std::vector<InstanceId> fillerParticipants;
};

// Deterministic; one transcript line per violation, showing what it was and
std::vector<NormalizedViolation> normalizeViolations(
    const FillerRepairRequest& request,
    const DebugLog& log);

// "Are these two the same violation?", across two separate checker runs. Not
bool sameSignature(const Violation& a, const Violation& b, DbCoord siteWidth);

// The part of sameSignature that is a plain equality test. Two violations
using SignatureClass =
    std::tuple<int,
               ViolationKind,
               ViolationRelation,
               LayerId,
               std::optional<LayerId>>;

inline SignatureClass signatureClass(const Violation& v)
{
  return {v.ruleId, v.kind, v.relation, v.primaryLayer, v.secondaryLayer};
}

// "Did WE cause this?" -- true when a violation involves one of the fillers
bool isRelatedToOverlay(const Violation& violation,
                        const Overlay& overlay,
                        DbCoord ruleDistance);

// Roughly how far a rule can reach: the largest requiredValue the checker
DbCoord estimateRuleDistance(const std::vector<Violation>& violations,
                             DbCoord siteWidth);

// --- repair window: where the search is allowed to edit ---------------------

struct RepairWindow
{
// 0 = the starting window; N = grown N times.
  int level = 0;
  std::vector<RowId> rows;  // rows we may edit fillers in, sorted
  XInterval x;              // x range we may edit, snapped to whole instances

// Every filler inside rows/x, sorted by (row, x). This is the complete set
  std::vector<InstanceId> editableFillers;
// The subset that sits BETWEEN the runs a violation is about: recolouring
  std::vector<InstanceId> bridgeFillers;

  Region area() const
  {
    if (rows.empty()) {
      return Region{};
    }
    return Region{x, rows.front(), rows.back()};
  }

// What the checker is asked to look at: area() plus a ring wide enough that
  Region guardRegion;

  bool containsEditable(InstanceId id) const;
};

// The starting window: the fillers around the violation the checker just
RepairWindow buildWindow(const TargetPlace& anchor,
                         const std::vector<NormalizedViolation>& violations,
                         const PlacementView& view,
                         DbCoord ruleDistance,
                         const DebugLog& log);

// Nothing in this window worked, so reach a little further. `blocking` is
RepairWindow expandWindowAdaptive(const RepairWindow& current,
                                  const TargetPlace& anchor,
                                  const std::vector<Violation>& blocking,
                                  const PlacementView& view,
                                  int fillersPerRow,
                                  const DebugLog& log);

// --- which moves to try first -----------------------------------------------

// One editable filler and every master it could take, best guess first.
struct FillerDomain
{
  InstanceId instanceId = 0;
  std::vector<Swap> options;
};

std::vector<FillerDomain> rankFillers(
    const std::vector<Swap>& swaps,
    const TargetPlace& anchor,
    const std::vector<NormalizedViolation>& violations,
    const RepairWindow& window,
    const PlacementView& view,
    const DebugLog& log);

// --- turning moves into candidates ------------------------------------------

struct EnumerationPlan
{
// True when everything the window could offer was emitted, so a
  bool complete = false;
  std::vector<Overlay> overlays;  // try in this order, capped by the budget
};

// Combinations of moves: first every single swap, then every pair, and so on
EnumerationPlan enumerateOverlays(const std::vector<FillerDomain>& ranked,
                                  const RepairConfig& config,
                                  int budget,
                                  const DebugLog& log,
                                  const std::vector<InstanceId>& freshFillers
                                  = {});

// --- asking the checker, and reading its answer -----------------------------

// What one checker answer means for one candidate. Acceptance is NOT "the
struct DeltaSummary
{
  bool usable = false;         // the checker actually answered
  bool inconsistent = false;   // it said legal but listed violations, or v.v.
  int residualOriginals = 0;   // violations we were asked to fix, still there
  int newInWindow = 0;         // we broke something where we were editing
  int relatedInHalo = 0;       // we broke something just outside it
  int unrelatedInHalo = 0;     // was already broken out there; not our problem
// The findings that actually blocked acceptance. Where they sit is what
  std::vector<Violation> blockingViolations;
// Accept: nothing left of the originals, and nothing new that we caused.
  bool clean = false;
};

class OracleGate
{
 public:
  OracleGate(const PlacementView& dataSource,
             RepairOracle& oracle,
             const TargetPlace& anchor,
             const std::vector<Violation>& originals,
             DbCoord siteWidth,
             DbCoord ruleDistance,
             const RepairConfig& config,
             const DebugLog& log);

// Asks the checker what this guard looks like with NOTHING changed. That
  bool runBaseline(const RepairWindow& window, int& budget);

  struct SearchResult
  {
    bool foundClean = false;
    Overlay cleanOverlay;
    bool protocolError = false;
    bool budgetExhausted = false;
// Best non-clean candidate, also used to steer which side to grow.
    bool hasBest = false;
    Overlay bestOverlay;
    DeltaSummary bestSummary;
  };

// Walks the candidates in order, in batches, and stops at the first one the
  SearchResult search(const std::vector<Overlay>& candidates,
                      const RepairWindow& window,
                      const Region& guard,
                      int& budget);

  int requestsSent() const { return requests_sent_; }
  int batchesSent() const { return batches_sent_; }
  int cacheHits() const { return cache_hits_; }
  const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

 private:
// Answers one batch: cached candidates come straight back, the rest go to
  bool resolve(const Overlay* chunk,
               const OverlayKey* keys,
               std::size_t count,
               const Region& guard,
               int& budget,
               std::vector<const OracleResult*>& out);
  DeltaSummary classify(const OracleResult& result,
                        const Overlay& overlay,
                        const RepairWindow& window) const;
// The stale-snapshot check described on runBaseline(). Reads the baseline
  bool checkBaselineConsistency(const RepairWindow& window);

  const PlacementView& data_source_;
  RepairOracle& oracle_;
  const TargetPlace& anchor_;
  const std::vector<Violation>& originals_;
  DbCoord site_width_;
  DbCoord rule_distance_;
  const RepairConfig& config_;
  const DebugLog& log_;

// Node-based, so `baseline_` stays valid across rehashes. Never iterated:
  std::unordered_map<OverlayKey, OracleResult, OverlayKeyHash> cache_;
  const OracleResult* baseline_ = nullptr;  // points into cache_

// Signature classes of the fixed original snapshot and of the current
  std::vector<SignatureClass> original_classes_;
  std::vector<SignatureClass> baseline_classes_;
  mutable std::vector<char> consumed_scratch_;
  mutable std::vector<char> baseline_consumed_scratch_;
  mutable std::vector<SignatureClass> result_classes_scratch_;
  OracleRequestId next_request_id_ = 0;
  int requests_sent_ = 0;
  int batches_sent_ = 0;
  int cache_hits_ = 0;
  std::vector<Diagnostic> diagnostics_;
};

// --- the driver -------------------------------------------------------------

namespace internal {

// Runs the whole thing: build the starting window, search it, and when
class RepairPlanner
{
 public:
  RepairPlanner(const PlacementView& view,
                      RepairOracle& oracle,
                      RepairConfig config = {});

  FillerRepairResult repair(const FillerRepairRequest& request);

 private:
  const PlacementView& view_;
  RepairOracle& oracle_;
  RepairConfig config_;
  DebugLog log_;
// Guards the no-reentrancy contract AND flags concurrent use of one
  std::atomic<bool> repair_active_{false};
};

}  // namespace internal

}  // namespace dpl2::fillerRepair
