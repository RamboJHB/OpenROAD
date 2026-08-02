// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The deterministic search pipeline, in the order a repair flows through it:
// a repair flows through it:
//   swap model -> violation signatures -> repair window -> ranking ->
//   subset enumeration -> oracle gate -> pipeline driver.
//
// These stages only make sense together -- each consumes the previous one's
// output -- so they share a module. The two things that are NOT stages live
// on their own because the runtime engine implements them: PlacementView
// (data in) and RepairOracle (legality out).
//
// The planner owns no state between repair() calls and never mutates the
// design; the final checker stays the only legality oracle.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fillerRepair/Debug.h>
#include <fillerRepair/PlacementView.h>
#include <fillerRepair/RepairOracle.h>
#include <fillerRepair/RepairTypes.h>

namespace dpl2::fillerRepair {

// Search parameters. All knobs live here so tests and diagnostics can
// diagnostics can print the exact configuration used.
struct RepairConfig
{
  int checkerCallBudgetPerWindow = 512;  // includes the baseline request
  // Ceiling on checker calls for ONE repair() across every adaptive level.
  // Without it the worst case is maxAdaptiveLevels windows each spending a
  // full per-window budget (32 x 512 = 16384 calls), which is exactly the
  // path a no-solution case at 100% utilization with sparse fillers takes.
  // Reaching it ends the search with the existing "truncated" semantics --
  // never a wrong answer, only a bounded give-up. <= 0 disables the cap.
  int checkerCallBudgetPerRepair = 2048;
  int batchSize = 32;
  int maxSubsetSize = 4;          // large-window truncation only
  int memberCapSize2 = 24;        // N_2
  int memberCapSize3 = 12;        // N_3
  int memberCapSize4 = 8;         // N_4
  int adaptiveStepFillers = 2;    // K per relevant row/side
  // Safety valve for the NO-SOLUTION path: without it adaptive expansion
  // keeps adding fillers until the window rows are exhausted (levels ~
  // fillers/(2K), each level up to one window budget of checker calls).
  // Reaching the cap ends the search with the existing "truncated"
  // semantics -- never a wrong answer, only a bounded give-up.
  int maxAdaptiveLevels = 32;
  bool verbose = true;            // [fr] transcript; FR_VERBOSE=0 silences
};

// --- Swap: the atomic operation ---------------------------------------------

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

// Builds a validated Swap or explains why it cannot exist: the instance must
// be a placed filler and the new master a same-width/same-height filler
// master different from the current one. On failure *error (if given)
// receives the reason.
std::optional<Swap> makeSwap(const PlacementView& view,
                             InstanceId instanceId,
                             MasterId newMasterId,
                             std::string* error = nullptr);

// --- Overlay identity ------------------------------------------------------
//
// Two candidates are the SAME checker question when they propose the same
// (instance -> new master) set under the same guard: overlay order and
// duplicate swaps must never buy a second checker call. That identity is a
// value, not a serialized string -- the search asks it once per candidate per
// batch, so building it must not allocate a stream or a formatted key.
struct OverlayKey
{
  using Entry = std::pair<InstanceId, MasterId>;
  // One key is built per candidate, and a candidate holds one swap per
  // filler in the subset -- `maxSubsetSize` of them, or the whole (small)
  // window when its space is enumerated completely. Small subsets therefore
  // cover the search, and they must not each cost a heap allocation. Larger
  // ones still work: they spill to `overflow`.
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
  // master id ascending.
  std::vector<Swap> swaps;
  // NoUsableMaster per replacement-less filler, plus any provider
  // diagnostics. A filler without swaps is a normal outcome, not an error.
  std::vector<Diagnostic> diagnostics;
};

SwapGenerationResult generateSwaps(
    const RepairWindow& window,
    const PlacementView& view,
    const DebugLog& log);

// --- violation signatures / relatedness ------------------------------------

// A violation with the derived fields the planner works on. `raw` is kept by
// value: normalization must outlive the request's snapshot vector.
struct NormalizedViolation
{
  Violation raw;

  std::vector<RowId> rowIds;  // sorted unique, never empty
  bool rowIdFallback = false;  // rowIds were missing; anchor row substituted

  // xWindow united with all participant x ranges: the geometric footprint
  // used for windowing and the unfixable fast check.
  XInterval xRange;

  std::vector<InstanceId> cellAnchors;        // target + non-filler participants
  std::vector<InstanceId> fillerParticipants;  // filler participants
};

// Normalizes the initial snapshot. Deterministic; logs one line per
// violation (signature summary -> derived footprint).
std::vector<NormalizedViolation> normalizeViolations(
    const FillerRepairRequest& request,
    const PlacementView& view,
    const DebugLog& log);

// Pinned signature match across two checker snapshots (see file header).
bool sameSignature(const Violation& a, const Violation& b, DbCoord siteWidth);

// The exact-equality prefix of `sameSignature`: two violations whose classes
// differ can never match. Comparing the packed class first turns the delta
// classifier's O(originals x findings) scan into integer compares for every
// pair that cannot match, without touching which pair is chosen -- the scan
// order and the one-to-one consumption rule are unchanged.
struct SignatureClass
{
  int ruleId = 0;
  ViolationKind kind = ViolationKind::MinWidth;
  ViolationRelation relation = ViolationRelation::IntraRow;
  LayerId primaryLayer = 0;
  bool hasSecondaryLayer = false;
  LayerId secondaryLayer = 0;

  bool operator==(const SignatureClass& other) const
  {
    return ruleId == other.ruleId && kind == other.kind
           && relation == other.relation
           && primaryLayer == other.primaryLayer
           && hasSecondaryLayer == other.hasSecondaryLayer
           && secondaryLayer == other.secondaryLayer;
  }
  bool operator!=(const SignatureClass& other) const
  {
    return !(*this == other);
  }
};

inline SignatureClass signatureClass(const Violation& v)
{
  SignatureClass c;
  c.ruleId = v.ruleId;
  c.kind = v.kind;
  c.relation = v.relation;
  c.primaryLayer = v.primaryLayer;
  c.hasSecondaryLayer = v.secondaryLayer.has_value();
  c.secondaryLayer = v.secondaryLayer.value_or(0);
  return c;
}

// Pinned relatedness: participants touch a changed instance, or xWindow is
// within `ruleDistance` of a changed span on the same/adjacent row.
bool isRelatedToOverlay(const Violation& violation,
                        const Overlay& overlay,
                        DbCoord ruleDistance);

// Rule-distance estimate for geometry heuristics: the largest requiredValue
// in the snapshot, falling back to one site. Only used for windows and
// relatedness margins -- never for legality decisions (checker-as-oracle).
DbCoord estimateRuleDistance(const std::vector<Violation>& violations,
                             DbCoord siteWidth);

// --- repair window: L0 + adaptive-L1 ----------------------------------------

struct RepairWindow
{
  int level = 0;
  std::vector<RowId> rows;  // sorted; rows the planner may edit fillers in
  XInterval x;              // editable x range (snapped to whole instances)

  // Fillers inside rows/x, sorted by (row, x): the move-generation universe.
  std::vector<InstanceId> editableFillers;
  // Subset of editableFillers flagged as bridge fillers (default-mandatory
  // candidates).
  std::vector<InstanceId> bridgeFillers;

  Region area() const
  {
    if (rows.empty()) {
      return Region{};
    }
    return Region{x, rows.front(), rows.back()};
  }

  Region guardRegion;  // two-cell ring around area()

  bool containsEditable(InstanceId id) const;
};

// Builds L0 for the (single) violation cluster around the anchor.
// `ruleDistance` widens bridge detection margins only. `level` is retained for
// source compatibility and must be 0; adaptive growth uses the API below.
RepairWindow buildWindow(int level,
                         const TargetPlace& anchor,
                         const std::vector<NormalizedViolation>& violations,
                         const PlacementView& view,
                         DbCoord ruleDistance,
                         const DebugLog& log);

// One adaptive-L1 step. `blocking` is the best non-clean
// candidate's residual/new-related violation set. Direction is derived from
// those x windows relative to `current`; when no directional finding exists,
// both sides are tried. The step is deterministic and never sweeps an entire
// filler run: each selected side adds at most `fillersPerRow` adjacent fillers
// per relevant row, stopping at a non-filler boundary.
RepairWindow expandWindowAdaptive(const RepairWindow& current,
                                  const TargetPlace& anchor,
                                  const std::vector<Violation>& blocking,
                                  const PlacementView& view,
                                  int fillersPerRow,
                                  const DebugLog& log);

// --- ranking into filler domains --------------------------------------------

// One editable filler with its full, preference-ordered candidate domain.
// Ranking never truncates a domain.
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

// --- subset enumeration -----------------------------------------------------

struct EnumerationPlan
{
  bool complete = false;          // full space emitted -> definitive result
  std::vector<Overlay> overlays;  // enumeration order, capped at budget
};

// `freshFillers` (sorted instance ids) makes the enumeration INCREMENTAL
// across adaptive levels: a combination that contains none of them was already
// emitted -- and answered -- at the previous level. Re-emitting it re-asks a
// question whose answer cannot have changed, because under an unchanged guard
// a candidate that was not clean at level L cannot become clean at L+1: the
// window only grows, so a halo finding can migrate into the window (still
// blocking) but no blocker can disappear, and a clean candidate would have
// ended the search at L. Skipping them is therefore not an approximation --
// it spends the window budget on questions the search has not asked yet.
//
// Empty = no filtering: L0, and any level whose quantized guard moved (a new
// guard is a different checker question, so every candidate is new again).
EnumerationPlan enumerateOverlays(const std::vector<FillerDomain>& ranked,
                                  const RepairConfig& config,
                                  int budget,
                                  const DebugLog& log,
                                  const std::vector<InstanceId>& freshFillers
                                  = {});

// --- oracle gate: baseline-delta accept -------------------------------------

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
  // L1 uses their rows/x windows to choose the next growth side.
  std::vector<Violation> blockingViolations;
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

  // Baseline for `window.guardRegion`; consumes budget only on a cache miss.
  // False when the baseline is unusable (checker error) OR fails the baseline
  // consistency gate: the baseline must reproduce every
  // original that lies inside the guard, and must not carry an unexpected
  // in-window violation that was not in the input snapshot. A false return is
  // fatal for the window -- either a checker error or a stale/inconsistent
  // snapshot, both of which the planner must not silently treat as "repaired".
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
  // Answers one batch of candidates. `keys` are their identities, built once
  // by the caller. `out` receives one entry per candidate: the cached or
  // freshly received result, or nullptr when budget stopped it from being
  // evaluated -- so the caller never repeats the cache lookup this already
  // did. Returns false on a batch protocol error (diagnostics pushed).
  bool resolve(const Overlay* chunk,
               const OverlayKey* keys,
               std::size_t count,
               const Region& guard,
               int& budget,
               std::vector<const OracleResult*>& out);
  DeltaSummary classify(const OracleResult& result,
                        const Overlay& overlay,
                        const RepairWindow& window) const;
  // Baseline consistency gate. Uses the already-fetched
  // baseline_, spends no budget. Pushes a fatal BaselineMismatch diagnostic and
  // returns false when the snapshot is stale/inconsistent.
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
  // only find/emplace/size, so bucket order cannot affect search order.
  std::unordered_map<OverlayKey, OracleResult, OverlayKeyHash> cache_;
  const OracleResult* baseline_ = nullptr;  // points into cache_

  // Signature classes of the fixed original snapshot and of the current
  // baseline, so classify() compares packed ints instead of re-deriving them
  // for every candidate. Rebuilt with the baseline, which changes only when
  // the guard does.
  std::vector<SignatureClass> original_classes_;
  std::vector<SignatureClass> baseline_classes_;
  // classify() scratch, reused across candidates: one repair runs one search
  // at a time, so these never overlap. Keeps the delta classifier allocation
  // free on a path that runs once per candidate.
  mutable std::vector<char> consumed_scratch_;
  mutable std::vector<char> baseline_consumed_scratch_;
  mutable std::vector<SignatureClass> result_classes_scratch_;
  OracleRequestId next_request_id_ = 0;
  int requests_sent_ = 0;
  int batches_sent_ = 0;
  int cache_hits_ = 0;
  std::vector<Diagnostic> diagnostics_;
};

// --- pipeline driver -------------------------------------------------------

namespace internal {

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
  // planner instance; concurrent repairs use one planner per thread.
  std::atomic<bool> repair_active_{false};
};

}  // namespace internal

}  // namespace dpl2::fillerRepair
