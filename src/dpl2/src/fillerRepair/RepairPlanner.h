// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The search. Opto retargeted one standard cell to a different VT; the
// checker says that is now a DRC violation; this finds which surrounding
// FILLERS to recolour so it stops being one.
//
// A worked example -- one row, `[ 2F2 ]` is a cell on VT family F2:
//
//     built (legal):   ... [ 3F3 ][ cF3 ][ 2F2 ][ bF2 ][ 3F3 ][ cF3 ] ...
//     opto retargets           the 2F2 cell to F3      ^^^^^^
//     now:             ... [ 3F3 ][ cF3 ][ 3F3 ][ bF2 ][ 3F3 ][ cF3 ] ...
//                                          ^^^^^^ one site wide  -> min WIDTH
//                                          and one site away from the F3 run
//                                          on its right          -> min SPACING
//     we answer:                   recolour [ bF2 ] to F3, and the two runs
//                                  merge into one four-site run.
//
// That filler in the middle is the "bridge", and finding it is the easy case.
// The hard cases need several fillers at once, or fillers further out, which
// is what the stages below are for. They run in this order, each consuming the
// previous one's output:
//
//   makeSwap        one legal (filler -> new master) move
//   normalize       what the checker reported, in the planner's terms
//   buildWindow     which fillers we may edit, and how far the checker must look
//   generateSwaps   every legal move inside that window
//   rankFillers     which moves to try first
//   enumerate       combinations of moves = candidates
//   OracleGate      ask the checker, accept only a candidate that is clean
//   RepairPlanner   drive all of the above, growing the window when stuck
//
// Two things here are NOT stages, because the runtime engine supplies them:
// PlacementView (what is placed where) and RepairOracle (is this legal).
//
// We never decide legality ourselves -- the real checker does, every time.
// The planner keeps no state between repair() calls and never touches the
// design; committing the answer is the caller's job.

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

// Every search knob, in one place, so the transcript can print the exact
// configuration a run used.
//
// [PORT-TUNE] Every default below was chosen against a SYNTHETIC oracle that
// answers instantly. In production each checker call is real DRC work, so the
// budgets are really "how much DRC time may one repair cost", and only your
// hardware can answer that. Before touching any of them, get the two numbers
// the transcript already prints on a real design -- `checker requests=` and
// the wall time of one repair() -- and change one knob at a time. They are
// safe as shipped: reaching a budget only ever ends the search early, it
// never produces a wrong answer.
struct RepairConfig
{
  // Checker calls one window may spend, the baseline request included.
  int checkerCallBudgetPerWindow = 512;
  // Checker calls ONE repair() may spend in total. Without it the worst case
  // is every growth step spending a full window budget -- 32 x 512 = 16384
  // real DRC calls -- which is exactly what a no-solution case on a fully
  // filled design does. Hitting either budget ends the search as *truncated*:
  // never a wrong answer, only a bounded give-up. <= 0 disables this one.
  int checkerCallBudgetPerRepair = 2048;
  // Candidates per checker batch. [PORT-TUNE] The best value is roughly your
  // checker's parallelFor width: a batch is one call whose candidates run in
  // parallel, over a fixed per-batch cost (one region scan). Too small wastes
  // that scan; too large speculatively checks candidates an earlier one in
  // the same batch already made unnecessary.
  int batchSize = 32;
  // How many fillers one candidate may change at once. Only bites on windows
  // too large to enumerate exhaustively.
  int maxSubsetSize = 4;
  // ... and, for each of those sizes, how many of the best-ranked fillers may
  // take part. Combinations grow as C(members, size), so these are what keep
  // a wide window from exploding.
  int memberCapSize2 = 24;
  int memberCapSize3 = 12;
  int memberCapSize4 = 8;
  // Fillers one growth step adds, per row, per side.
  int adaptiveStepFillers = 2;
  // How many times the window may grow before giving up. Without it a
  // no-solution case keeps growing until the rows run out, paying a window
  // budget each time. Also truncation, never a wrong answer.
  //
  // [PORT-TUNE] 32 is a safety valve, not a tuned value. What it should be is
  // "how far from the target could a usable filler plausibly be" on your
  // designs. The transcript names the level each answer came from
  // ("grown xN"), so a histogram of that over a real run tells you directly.
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
// be a placed filler, and the new master a DIFFERENT filler master of exactly
// the same width and height -- so the cell keeps its site, and nothing has to
// be re-placed. `*error` (if given) says which of those failed.
std::optional<Swap> makeSwap(const PlacementView& view,
                             InstanceId instanceId,
                             MasterId newMasterId,
                             std::string* error = nullptr);

// --- Overlay identity ------------------------------------------------------
//
// Two candidates are the SAME question to the checker when they change the
// same fillers to the same masters and ask about the same guard. Listing
// those swaps in a different order, or twice, must not buy a second real DRC
// call. This is that identity -- as a value, not a formatted string: the
// search builds one per candidate, so it has to be cheap.
struct OverlayKey
{
  using Entry = std::pair<InstanceId, MasterId>;
  // One key per candidate, holding one entry per filler that candidate
  // changes -- `maxSubsetSize` of them (4), or a whole small window when its
  // space is enumerated exhaustively. 8 covers both without ever touching the
  // heap; anything longer still works, it just spills to `overflow`.
  //
  // [PORT-TUNE] Only worth revisiting if you raise `maxSubsetSize`. Bigger
  // costs memory on every cache entry for nothing; smaller silently puts an
  // allocation back on the hottest path in the search.
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

// --- what the checker reported, in the planner's terms ----------------------

// One checker violation with the few things the planner keeps asking about
// pre-computed. `raw` is a copy on purpose: the caller's snapshot vector does
// not have to outlive the search.
struct NormalizedViolation
{
  Violation raw;

  std::vector<RowId> rowIds;  // sorted unique, never empty
  bool rowIdFallback = false;  // rowIds were missing; anchor row substituted

  // How much x this violation actually covers: its own xWindow plus the span
  // of everyone involved. For a SPACING violation xWindow is only the gap, so
  // without this the window would miss the runs on either side of it.
  XInterval xRange;

  // Who is involved, split by what we can do about them: cells we cannot
  // touch (the retargeted target and its neighbours) and fillers we can.
  std::vector<InstanceId> cellAnchors;
  std::vector<InstanceId> fillerParticipants;
};

// Deterministic; one transcript line per violation, showing what it was and
// what footprint it turned into.
std::vector<NormalizedViolation> normalizeViolations(
    const FillerRepairRequest& request,
    const PlacementView& view,
    const DebugLog& log);

// "Are these two the same violation?", across two separate checker runs. Not
// pointer or index identity -- the checker rebuilds its findings every call --
// but same rule, same kind, same layers, same rows, and x windows that overlap
// or sit within a site of each other. That last tolerance is what lets us tell
// "the original violation is still there" from "a new one appeared nearby".
bool sameSignature(const Violation& a, const Violation& b, DbCoord siteWidth);

// The part of sameSignature that is a plain equality test. Two violations
// whose classes differ can never match, so comparing this first skips the
// real call for every pair that was never going to match. Pure speed: it
// changes no answer, no scan order, and no matching rule.
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

// "Did WE cause this?" -- true when a violation involves one of the fillers
// this candidate changed, or sits within `ruleDistance` of one on the same or
// an adjacent row. A new violation we caused blocks the candidate; one that
// was already there, or is too far away to be our doing, does not.
bool isRelatedToOverlay(const Violation& violation,
                        const Overlay& overlay,
                        DbCoord ruleDistance);

// Roughly how far a rule can reach: the largest requiredValue the checker
// reported, or one site if it reported none. Used only to size windows and
// relatedness margins. It never decides legality -- the checker does.
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
  // of moves available at this level -- nothing outside it can be changed.
  std::vector<InstanceId> editableFillers;
  // The subset that sits BETWEEN the runs a violation is about: recolouring
  // one of these is what merges them, so they are tried first.
  std::vector<InstanceId> bridgeFillers;

  Region area() const
  {
    if (rows.empty()) {
      return Region{};
    }
    return Region{x, rows.front(), rows.back()};
  }

  // What the checker is asked to look at: area() plus a ring wide enough that
  // its own rule reach never runs off the edge of the snapshot. A guard
  // narrower than that reach truncates the run at the boundary and the
  // checker reports a min-width violation that does not exist.
  Region guardRegion;

  bool containsEditable(InstanceId id) const;
};

// The starting window: the fillers around the violation the checker just
// reported. `ruleDistance` only widens how far a bridge filler is looked for.
// Growing the window is a separate, stateful step -- see below.
RepairWindow buildWindow(const TargetPlace& anchor,
                         const std::vector<NormalizedViolation>& violations,
                         const PlacementView& view,
                         DbCoord ruleDistance,
                         const DebugLog& log);

// Nothing in this window worked, so reach a little further. `blocking` is
// what stopped the best candidate we found; where those violations sit tells
// us which side to grow -- and if they say nothing, both sides grow.
//
// Deliberately small steps: each side gains at most `fillersPerRow` fillers
// per row and stops at the first non-filler, so a long filler run is walked
// a few sites at a time instead of being swallowed whole.
RepairWindow expandWindowAdaptive(const RepairWindow& current,
                                  const TargetPlace& anchor,
                                  const std::vector<Violation>& blocking,
                                  const PlacementView& view,
                                  int fillersPerRow,
                                  const DebugLog& log);

// --- which moves to try first -----------------------------------------------

// One editable filler and every master it could take, best guess first.
// Ranking only ORDERS -- it never drops an option, so a repair that needs an
// unlikely-looking swap is still reachable.
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
  // no-solution answer from this level is definitive rather than a give-up.
  bool complete = false;
  std::vector<Overlay> overlays;  // try in this order, capped by the budget
};

// Combinations of moves: first every single swap, then every pair, and so on
// up to `maxSubsetSize`, with the member caps keeping a wide window from
// exploding.
//
// `freshFillers` is what makes growth cheap. When the window grows but the
// guard does not move, most combinations are ones the previous level already
// put to the checker -- and their answer cannot have changed:
//
//   the window only ever GROWS, so a violation in the halo can move inside it
//   (still blocking) but nothing that blocked can stop blocking; and a
//   candidate that had been clean would have ended the search back then.
//
// So a level only needs the combinations touching a filler it just gained.
// This is not a heuristic prune -- the skipped ones still count as covered,
// so a level that skips them is still `complete`. Pass it empty for the
// starting window, and for any level where the guard moved: a new guard is a
// different question, so every candidate is new again.
EnumerationPlan enumerateOverlays(const std::vector<FillerDomain>& ranked,
                                  const RepairConfig& config,
                                  int budget,
                                  const DebugLog& log,
                                  const std::vector<InstanceId>& freshFillers
                                  = {});

// --- asking the checker, and reading its answer -----------------------------

// What one checker answer means for one candidate. Acceptance is NOT "the
// design is now violation-free" -- a design can have violations elsewhere
// that are none of our business. It is a delta against the baseline: did we
// fix what we were asked to fix, without breaking anything?
struct DeltaSummary
{
  bool usable = false;         // the checker actually answered
  bool inconsistent = false;   // it said legal but listed violations, or v.v.
  int residualOriginals = 0;   // violations we were asked to fix, still there
  int newInWindow = 0;         // we broke something where we were editing
  int relatedInHalo = 0;       // we broke something just outside it
  int unrelatedInHalo = 0;     // was already broken out there; not our problem
  // The findings that actually blocked acceptance. Where they sit is what
  // tells the next growth step which way to reach.
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
  // answer is what every candidate is compared against, so it is also a
  // sanity check on the input: the baseline must still show every violation
  // we were asked to fix, and must not show one inside the window that the
  // caller never told us about. Either would mean the snapshot we were handed
  // no longer describes the design -- and a search on a stale snapshot could
  // report "repaired" for something it never looked at. So a false return
  // kills the window rather than degrading quietly. Costs budget only when
  // the answer is not already cached.
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
  // checker calls clean. Budget is spent per candidate actually sent.
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
  // the checker in a single call. `out` gets one entry per candidate -- the
  // answer, or nullptr if the budget ran out before it was asked. Handing the
  // answers back means the caller never repeats the lookup done here.
  // False = the checker broke the batch protocol.
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
  // already in hand, so it costs nothing.
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

// --- the driver -------------------------------------------------------------

namespace internal {

// Runs the whole thing: build the starting window, search it, and when
// nothing there works, grow it and search again -- until something is clean,
// a budget runs out, or the window cannot grow any further.
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
