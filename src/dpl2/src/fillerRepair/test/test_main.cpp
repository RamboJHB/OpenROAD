// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Unit tests for the fillerRepair planner: Swap primitives, fake checker /
// fake candidate provider protocol, and the
// 100% utility pre-check. Plain C++17, no external test framework so the
// suite runs before dpl2 is wired into the CMake build.
//
// Run: test/run_tests.sh   (FR_VERBOSE=1 prints the [fr] debug transcript)

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <map>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

#include "../FillerRepairEngine.h"
#include "../Swap.h"
#include "../PreCheck.h"
#include "../Signature.h"
#include "../OracleGate.h"
#include "../Ranker.h"
#include "../SubsetSearch.h"
#include "../SwapGenerator.h"
#include "../Window.h"
#include "../fake/FakeCandidateProvider.h"
#include "../fake/FakeDesign.h"
#include "../fake/FakeImplantChecker.h"

namespace fr = dpl2::fillerRepair;

// --- Minimal test harness ---------------------------------------------------

namespace {

int g_failures = 0;
const char* g_current = "";

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("FAIL %s: %s (%s:%d)\n", g_current, #cond, __FILE__,     \
                  __LINE__);                                               \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    const auto va = (a);                                                   \
    const auto vb = (b);                                                   \
    if (!(va == vb)) {                                                     \
      std::printf("FAIL %s: %s == %s (%lld vs %lld) (%s:%d)\n", g_current, \
                  #a, #b, (long long) va, (long long) vb, __FILE__,        \
                  __LINE__);                                               \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

struct Test
{
  const char* name;
  std::function<void()> fn;
};

bool verbose()
{
  const char* env = std::getenv("FR_VERBOSE");
  return env != nullptr && std::strcmp(env, "0") != 0;
}

// --- Fixtures ---------------------------------------------------------------

// Master id scheme: filler = width*10 + vt (e.g. 42 = width-4 VT2);
// std cell = 900 + vt, width 4. VTs are {1, 2, 3}. Site width 1.
constexpr fr::VtId kVt1 = 1;
constexpr fr::VtId kVt2 = 2;
constexpr fr::VtId kVt3 = 3;

fr::MasterId fillerMaster(fr::DbCoord width, fr::VtId vt)
{
  return static_cast<fr::MasterId>(width * 10 + vt);
}

fr::MasterId cellMaster(fr::VtId vt)
{
  return static_cast<fr::MasterId>(900 + vt);
}

// Full master library: widths {2,3,4,8} x VTs {1,2,3}, all fillers, plus one
// width-4 std cell master per VT (mirrors appendix A of the spec).
fr::FakeDesign makeLibrary()
{
  fr::FakeDesign design;
  design.setSiteWidth(1);
  for (const fr::DbCoord w : {2, 3, 4, 8}) {
    for (const fr::VtId vt : {kVt1, kVt2, kVt3}) {
      design.addMaster(fillerMaster(w, vt), w, 1, /*isFiller=*/true, vt);
    }
  }
  for (const fr::VtId vt : {kVt1, kVt2, kVt3}) {
    design.addMaster(cellMaster(vt), 4, 1, /*isFiller=*/false, vt);
  }
  return design;
}

// One fully covered row [0,16). Instance ids 100..104.
//
// Vt Type: 1=vt type 1  |  Widths: {2, 4}  |  cell type: 1=std cell, 0=filler
// Format: (vt type, width, cell type)
// Row 0: (1,4,0) (1,2,0) (1,4,0) (1,2,0) (1,4,0)
//   ids:   100     101     102     103     104
// All fillers here; callers often swap inst 103 to a std cell (the anchor).
struct RowFixture
{
  fr::FakeDesign design;
  fr::InstanceId anchor = 103;
};

RowFixture makeCoveredRow()
{
  RowFixture f;
  f.design = makeLibrary();
  f.design.addRow(0, 0, 16)
      .place(100, fillerMaster(4, kVt1), 0, 0)
      .place(101, fillerMaster(2, kVt1), 0, 4)
      .place(102, fillerMaster(4, kVt1), 0, 6)
      .place(103, fillerMaster(2, kVt1), 0, 10)
      .place(104, fillerMaster(4, kVt1), 0, 12);
  return f;
}

fr::Region wholeDesignRegion()
{
  // Generous region: all rows, all x. Windowing (TODO 5) will narrow this.
  return fr::Region{fr::XInterval{-1000, 1000}, 0, 100};
}

fr::TargetPlace anchorPlace(const fr::FakeDesign& design, fr::InstanceId id)
{
  const fr::PlacedInstance* inst = design.instance(id);
  fr::TargetPlace place;
  place.instanceId = id;
  place.masterId = inst->masterId;
  place.rowId = inst->rowId;
  place.x = inst->x;
  return place;
}

// --- TODO 1: Swap primitives -------------------------------------------------

void testSwapConstruction()
{
  RowFixture f = makeCoveredRow();
  std::string error;

  // Valid swap: filler 101 (w2 vt1) -> w2 vt2 master.
  auto move = fr::makeSwap(f.design, 101, fillerMaster(2, kVt2), &error);
  CHECK(move.has_value());
  CHECK_EQ(move->rowId, 0);
  CHECK(move->span == (fr::XInterval{4, 6}));
  CHECK_EQ(move->oldVt, kVt1);
  CHECK_EQ(move->newVt, kVt2);

  // Rejections, each with a reason.
  CHECK(!fr::makeSwap(f.design, 999, fillerMaster(2, kVt2), &error).has_value());
  CHECK(!fr::makeSwap(f.design, 101, fillerMaster(4, kVt2), &error).has_value());
  CHECK(!error.empty());  // size mismatch reason recorded
  CHECK(!fr::makeSwap(f.design, 101, fillerMaster(2, kVt1), &error).has_value());
  CHECK(!fr::makeSwap(f.design, 101, cellMaster(kVt2), &error).has_value());

  // Std cell is never a move target.
  f.design.remove(103).place(103, cellMaster(kVt2), 0, 10);
  CHECK(!fr::makeSwap(f.design, 103, fillerMaster(2, kVt1), &error).has_value());
}

void testCanonicalKeyOrderIndependent()
{
  RowFixture f = makeCoveredRow();
  auto m1 = *fr::makeSwap(f.design, 100, fillerMaster(4, kVt2));
  auto m2 = *fr::makeSwap(f.design, 101, fillerMaster(2, kVt2));

  CHECK(fr::canonicalKey({m1, m2}) == fr::canonicalKey({m2, m1}));
  CHECK(fr::canonicalKey({m1}) != fr::canonicalKey({m1, m2}));
  // Same instance, different target master => different overlay.
  auto m1b = *fr::makeSwap(f.design, 100, fillerMaster(4, kVt3));
  CHECK(fr::canonicalKey({m1}) != fr::canonicalKey({m1b}));
}

void testWireAdapter()
{
  RowFixture f = makeCoveredRow();
  auto m1 = *fr::makeSwap(f.design, 101, fillerMaster(2, kVt2));
  auto m2 = *fr::makeSwap(f.design, 100, fillerMaster(4, kVt3));

  const auto changes = fr::toFillerChanges({m1, m2});
  CHECK_EQ(changes.size(), 2u);
  // Deterministic order: sorted by instanceId.
  CHECK_EQ(changes[0].instanceId, 100);
  CHECK_EQ(changes[0].newMasterId, fillerMaster(4, kVt3));
  CHECK_EQ(changes[1].instanceId, 101);
  CHECK_EQ(changes[1].newMasterId, fillerMaster(2, kVt2));
}

// --- TODO 3: utility pre-check ----------------------------------------------

void testPreCheckFullUtility()
{
  RowFixture f = makeCoveredRow();
  const auto coverage = fr::runUtilityPreCheck(f.design, fr::DebugLog(verbose()));
  CHECK(coverage.isFullUtility);
  CHECK(coverage.issues.empty());
}

void testPreCheckGap()
{
  RowFixture f = makeCoveredRow();
  f.design.remove(101);  // hole [4,6)

  const auto coverage = fr::runUtilityPreCheck(f.design, fr::DebugLog(verbose()));
  CHECK(!coverage.isFullUtility);
  CHECK_EQ(coverage.issues.size(), 1u);
  CHECK(coverage.issues[0].kind == fr::CoverageIssueKind::Gap);
  CHECK_EQ(coverage.issues[0].rowId, 0);
  CHECK_EQ(coverage.issues[0].xLo, 4);
  CHECK_EQ(coverage.issues[0].xHi, 6);
  CHECK_EQ(coverage.issues[0].siteCount, 2);
}

void testPreCheckOverlapOffGridIllegal()
{
  RowFixture f = makeCoveredRow();
  // Overlap: extra filler on top of [4,6).
  f.design.place(300, fillerMaster(2, kVt2), 0, 5);
  auto coverage = fr::runUtilityPreCheck(f.design, fr::DebugLog(verbose()));
  CHECK(!coverage.isFullUtility);
  bool sawOverlap = false;
  for (const auto& issue : coverage.issues) {
    sawOverlap |= issue.kind == fr::CoverageIssueKind::Overlap;
  }
  CHECK(sawOverlap);
  f.design.remove(300);

  // Illegal occupant: instance sticking out of the legal row span.
  f.design.remove(104).place(104, fillerMaster(8, kVt1), 0, 12);  // [12,20) > 16
  coverage = fr::runUtilityPreCheck(f.design, fr::DebugLog(verbose()));
  bool sawIllegal = false;
  for (const auto& issue : coverage.issues) {
    sawIllegal |= issue.kind == fr::CoverageIssueKind::IllegalOccupant;
  }
  CHECK(sawIllegal);

  // Off-grid: site width 2, instance at odd x.
  fr::FakeDesign design = makeLibrary();
  design.setSiteWidth(2).addRow(0, 0, 8).place(400, fillerMaster(4, kVt1), 0, 1);
  coverage = fr::runUtilityPreCheck(design, fr::DebugLog(verbose()));
  bool sawOffGrid = false;
  for (const auto& issue : coverage.issues) {
    sawOffGrid |= issue.kind == fr::CoverageIssueKind::OffGrid;
  }
  CHECK(sawOffGrid);
}

void testEngineFatalOnGapWithoutCheckerCalls()
{
  RowFixture f = makeCoveredRow();
  f.design.remove(101);  // hole [4,6)

  fr::FakeImplantChecker checker(f.design, {});
  fr::FakeCandidateProvider provider(f.design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(f.design, checker, provider, config);

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(f.design, f.anchor);
  const auto result = engine.repair(request);

  CHECK(!result.hasSolution);
  CHECK(result.changes.empty());
  bool sawFatal = false;
  for (const auto& diag : result.diagnostics) {
    sawFatal |= diag.severity == fr::Severity::Fatal && diag.code == "NonFullUtility";
  }
  CHECK(sawFatal);
  // Spec 6.1: pre-check failure must not reach the checker.
  CHECK_EQ(checker.requestCount(), 0);
}

// --- TODO 2: fake candidate provider ----------------------------------------

void testCandidateProvider()
{
  RowFixture f = makeCoveredRow();
  fr::FakeCandidateProvider provider(f.design);

  // Filler 101 is w2 vt1 -> exactly the two other w2 VTs, ascending order.
  const auto result = provider.getUsableMasterCandidates({101});
  CHECK_EQ(result.candidates.size(), 2u);
  CHECK_EQ(result.candidates[0].masterId, fillerMaster(2, kVt2));
  CHECK_EQ(result.candidates[1].masterId, fillerMaster(2, kVt3));

  // Std cell input: empty + diagnostic, not an error.
  f.design.remove(103).place(103, cellMaster(kVt1), 0, 10);
  const auto cellResult = provider.getUsableMasterCandidates({103});
  CHECK(cellResult.candidates.empty());
  CHECK(!cellResult.diagnostics.empty());
}

// --- TODO 2: fake checker protocol -------------------------------------------

// Two rows, anchor std cell at row0 whose VT2 conflicts with a VT2 filler in
// row1 below it (inter-row MS), plus everything else VT1. Recoloring the
// row1 filler to VT1... would merge with neighbors; instead the clean fix is
// recoloring it to VT2? See per-test comments; rules are chosen per case.
fr::OverlayCheckRequest baselineRequest(const fr::FakeDesign& design,
                                        fr::InstanceId anchor,
                                        fr::OverlayRequestId id)
{
  fr::OverlayCheckRequest request;
  request.requestId = id;
  request.targetPlace = anchorPlace(design, anchor);
  request.guardRegion = wholeDesignRegion();
  return request;
}

void testCheckerEchoAndOrder()
{
  RowFixture f = makeCoveredRow();
  fr::FakeImplantChecker checker(f.design, {});

  std::vector<fr::OverlayCheckRequest> batch;
  for (const fr::OverlayRequestId id : {7, 3, 5}) {
    batch.push_back(baselineRequest(f.design, f.anchor, id));
  }
  const auto results = checker.checkPlaceWithOverlays(batch);
  CHECK_EQ(results.size(), 3u);
  CHECK_EQ(results[0].requestId, 7);
  CHECK_EQ(results[1].requestId, 3);
  CHECK_EQ(results[2].requestId, 5);
  CHECK_EQ(checker.batchCount(), 1);
  CHECK_EQ(checker.requestCount(), 3);
}

void testCheckerInvalidIsolated()
{
  RowFixture f = makeCoveredRow();
  fr::FakeImplantChecker checker(f.design, {});

  auto valid = baselineRequest(f.design, f.anchor, 1);
  auto invalid = baselineRequest(f.design, f.anchor, 2);
  // Duplicate instance in one overlay -> InvalidOverlay for this request only.
  invalid.fillerChanges = {{101, fillerMaster(2, kVt2)}, {101, fillerMaster(2, kVt3)}};
  auto valid2 = baselineRequest(f.design, f.anchor, 3);

  const auto results = checker.checkPlaceWithOverlays({valid, invalid, valid2});
  CHECK(results[0].status == fr::CheckStatus::Checked);
  CHECK(results[1].status == fr::CheckStatus::InvalidOverlay);
  CHECK(!results[1].diagnostics.empty());  // status != Checked carries diags
  CHECK(results[2].status == fr::CheckStatus::Checked);
}

void testCheckerIntraMsDetectAndClear()
{
  // Row0: VT1 run [0,10), VT2 filler 103 [10,12), VT1 run [12,16).
  // With msIntra=3 the two VT1 runs are 2 apart -> intra-row MS violation.
  RowFixture f = makeCoveredRow();
  f.design.remove(103).place(103, fillerMaster(2, kVt2), 0, 10);

  fr::FakeImplantRules rules;
  rules.msIntra = 3;
  fr::FakeImplantChecker checker(f.design, rules);

  const auto baseline = checker.checkPlaceWithOverlay(
      baselineRequest(f.design, /*anchor=*/100, 1));
  CHECK(baseline.status == fr::CheckStatus::Checked);
  CHECK(!baseline.isLegal);
  CHECK_EQ(baseline.violations.size(), 1u);
  CHECK(baseline.violations[0].kind == fr::ViolationKind::MinSpacing);
  CHECK(baseline.violations[0].relation == fr::ViolationRelation::IntraRow);
  CHECK(baseline.violations[0].xWindow == (fr::XInterval{10, 12}));

  // Overlay: recolor 103 to VT1 -> single VT1 run [0,16) -> clean.
  auto overlay = baselineRequest(f.design, 100, 2);
  overlay.fillerChanges = {{103, fillerMaster(2, kVt1)}};
  const auto fixed = checker.checkPlaceWithOverlay(overlay);
  CHECK(fixed.status == fr::CheckStatus::Checked);
  CHECK(fr::isRawCheckerSnapshotClean(fixed));
}

void testCheckerInterRowRules()
{
  // Row0: VT2 run [0,4) then VT1 [4,16).
  // Row1: VT1 [0,3), VT2 [3,10), VT1 [10,16).
  // VT2 overlap = [3,4), width 1 < mwInter 2 -> inter-row MW violation.
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 16)
      .place(100, fillerMaster(4, kVt2), 0, 0)
      .place(101, fillerMaster(4, kVt1), 0, 4)
      .place(102, fillerMaster(8, kVt1), 0, 8)
      .addRow(1, 0, 16)
      .place(200, fillerMaster(3, kVt1), 1, 0)
      .place(201, fillerMaster(3, kVt2), 1, 3)
      .place(202, fillerMaster(4, kVt2), 1, 6)
      .place(203, fillerMaster(2, kVt1), 1, 10)
      .place(204, fillerMaster(4, kVt1), 1, 12);

  fr::FakeImplantRules rules;
  rules.mwInter = 2;
  fr::FakeImplantChecker checker(design, rules);

  const auto result = checker.checkPlaceWithOverlay(baselineRequest(design, 100, 1));
  CHECK(result.status == fr::CheckStatus::Checked);
  CHECK_EQ(result.violations.size(), 1u);
  CHECK(result.violations[0].kind == fr::ViolationKind::MinWidth);
  CHECK(result.violations[0].relation == fr::ViolationRelation::InterRow);
  CHECK(result.violations[0].xWindow == (fr::XInterval{3, 4}));
  CHECK_EQ(result.violations[0].rowIds.size(), 2u);

  // Inter-row MS: shrink row1's VT2 to [6,10) so the shapes become disjoint
  // with distance 2 < msInter 3.
  design.remove(201).place(201, fillerMaster(3, kVt1), 1, 3);
  fr::FakeImplantRules msRules;
  msRules.msInter = 3;
  fr::FakeImplantChecker msChecker(design, msRules);
  const auto msResult = msChecker.checkPlaceWithOverlay(baselineRequest(design, 100, 2));
  CHECK_EQ(msResult.violations.size(), 1u);
  CHECK(msResult.violations[0].kind == fr::ViolationKind::MinSpacing);
  CHECK(msResult.violations[0].relation == fr::ViolationRelation::InterRow);
  CHECK_EQ(msResult.violations[0].measuredValue, 2);
}

void testCheckerGuardRegionFilter()
{
  // Same MS layout as testCheckerIntraMsDetectAndClear, but the guard region
  // excludes the violation window -> checker reports clean in-region.
  RowFixture f = makeCoveredRow();
  f.design.remove(103).place(103, fillerMaster(2, kVt2), 0, 10);

  fr::FakeImplantRules rules;
  rules.msIntra = 3;
  fr::FakeImplantChecker checker(f.design, rules);

  auto request = baselineRequest(f.design, 100, 1);
  request.guardRegion = fr::Region{fr::XInterval{0, 8}, 0, 0};
  const auto result = checker.checkPlaceWithOverlay(request);
  CHECK(result.status == fr::CheckStatus::Checked);
  CHECK(result.violations.empty());
}

void testCheckerTargetOverrideSeedsViolation()
{
  // Anchor std cell VT1 at row0 [10,14); row1 has a VT2 filler below at
  // [9,11). Before the opto change everything same-VT overlaps by >= 2, so
  // with mwInter=2 the design is clean. Changing the anchor's master to VT2
  // creates a VT2/VT2 inter-row overlap of exactly 1 < 2 -> the violation
  // appears only AFTER the target override. This is the "checker rebuilds
  // context from targetPlace" contract.
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 16)
      .place(100, fillerMaster(8, kVt1), 0, 0)
      .place(101, fillerMaster(2, kVt1), 0, 8)
      .place(102, cellMaster(kVt1), 0, 10)  // anchor, w4 [10,14)
      .place(103, fillerMaster(2, kVt1), 0, 14)
      .addRow(1, 0, 16)
      .place(200, fillerMaster(3, kVt1), 1, 0)
      .place(201, fillerMaster(3, kVt1), 1, 3)
      .place(202, fillerMaster(3, kVt1), 1, 6)
      .place(203, fillerMaster(2, kVt2), 1, 9)   // bridge filler under anchor
      .place(204, fillerMaster(2, kVt1), 1, 11)
      .place(205, fillerMaster(3, kVt1), 1, 13);

  fr::FakeImplantRules rules;
  rules.mwInter = 2;
  fr::FakeImplantChecker checker(design, rules);

  // Before the change (target master == placed master): VT2 filler 203 has
  // no same-VT neighbor shape -> clean.
  const auto before = checker.checkPlaceWithOverlay(baselineRequest(design, 102, 1));
  CHECK(fr::isRawCheckerSnapshotClean(before));

  // Opto change: anchor becomes VT2 -> its shape [10,14) overlaps filler
  // 203's shape [9,11) by 1 < 2 -> inter-row MW violation with the target.
  auto changed = baselineRequest(design, 102, 2);
  changed.targetPlace.masterId = cellMaster(kVt2);
  const auto after = checker.checkPlaceWithOverlay(changed);
  CHECK(!after.isLegal);
  CHECK_EQ(after.violations.size(), 1u);
  bool targetSeen = false;
  for (const auto& p : after.violations[0].participants) {
    targetSeen |= p.isTarget;
  }
  CHECK(targetSeen);

  // Repair direction (spec anchor-follow): recolor bridge filler 203 to VT1
  // -> row1 becomes one VT1 run, anchor's VT2 shape has no partner -> clean.
  auto repaired = changed;
  repaired.requestId = 3;
  repaired.fillerChanges = {{203, fillerMaster(2, kVt1)}};
  const auto fixed = checker.checkPlaceWithOverlay(repaired);
  CHECK(fr::isRawCheckerSnapshotClean(fixed));
}


// --- TODO 4: normalization + signature ---------------------------------------

// Hand-built violation matching the inter-row MW shape of the fake checker.
fr::Violation makeViolation(int ruleId,
                            fr::ViolationKind kind,
                            fr::ViolationRelation relation,
                            std::vector<fr::RowId> rows,
                            fr::XInterval xWindow)
{
  fr::Violation v;
  v.ruleId = ruleId;
  v.kind = kind;
  v.relation = relation;
  v.rowIds = std::move(rows);
  v.xWindow = xWindow;
  v.requiredValue = 2;
  return v;
}

void testNormalizeViolations()
{
  RowFixture f = makeCoveredRow();
  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(f.design, 103);

  // Violation with participants: footprint must union xWindow with
  // participant ranges; filler/cell participants split into the two lists.
  fr::Violation v = makeViolation(3, fr::ViolationKind::MinWidth,
                                  fr::ViolationRelation::InterRow, {0, 1},
                                  {10, 11});
  fr::ViolationParticipant cell;
  cell.instanceId = 103;
  cell.rowId = 0;
  cell.xRange = {10, 12};
  cell.isFiller = false;
  cell.isTarget = true;
  fr::ViolationParticipant filler;
  filler.instanceId = 104;
  filler.rowId = 0;
  filler.xRange = {12, 16};
  filler.isFiller = true;
  v.participants = {cell, filler};

  // Violation without rows: must fall back to the anchor row and say so.
  fr::Violation noRows = makeViolation(2, fr::ViolationKind::MinSpacing,
                                       fr::ViolationRelation::IntraRow, {},
                                       {4, 6});
  request.violations = {v, noRows};

  const auto normalized =
      fr::normalizeViolations(request, f.design, fr::DebugLog(verbose()));
  CHECK_EQ(normalized.size(), 2u);

  CHECK(normalized[0].xRange == (fr::XInterval{10, 16}));
  CHECK_EQ(normalized[0].fillerParticipants.size(), 1u);
  CHECK_EQ(normalized[0].fillerParticipants[0], 104);
  CHECK_EQ(normalized[0].cellAnchors.size(), 1u);  // target == participant 103
  CHECK_EQ(normalized[0].cellAnchors[0], 103);
  CHECK(!normalized[0].rowIdFallback);

  CHECK(normalized[1].rowIdFallback);
  CHECK_EQ(normalized[1].rowIds.size(), 1u);
  CHECK_EQ(normalized[1].rowIds[0], 0);  // anchor row
}

void testSignatureMatching()
{
  const auto base = makeViolation(3, fr::ViolationKind::MinWidth,
                                  fr::ViolationRelation::InterRow, {0, 1},
                                  {10, 14});

  // Identical -> match; rows in different order -> still match.
  auto same = base;
  same.rowIds = {1, 0};
  CHECK(fr::sameSignature(base, same, 1));

  // Shifted by one site -> match (jitter tolerance).
  auto shifted = base;
  shifted.xWindow = {11, 15};
  CHECK(fr::sameSignature(base, shifted, 1));

  // Far away -> no match even with identical ids.
  auto far = base;
  far.xWindow = {30, 34};
  CHECK(!fr::sameSignature(base, far, 1));

  // Different rule / kind / relation / rows -> no match.
  auto rule = base;
  rule.ruleId = 4;
  CHECK(!fr::sameSignature(base, rule, 1));
  auto kind = base;
  kind.kind = fr::ViolationKind::MinSpacing;
  CHECK(!fr::sameSignature(base, kind, 1));
  auto rel = base;
  rel.relation = fr::ViolationRelation::IntraRow;
  CHECK(!fr::sameSignature(base, rel, 1));
  auto rows = base;
  rows.rowIds = {0};
  CHECK(!fr::sameSignature(base, rows, 1));

  // P/N band: same rule/kind/relation/rows/xWindow but different implant
  // layer -> distinct violations (spec 6.2, no dedup by position).
  fr::Violation pband = base;
  pband.primaryLayer = 10;  // e.g. P-band implant
  fr::Violation nband = base;
  nband.primaryLayer = 11;  // e.g. N-band implant at the same x gap
  CHECK(!fr::sameSignature(pband, nband, 1));
  CHECK(fr::sameSignature(pband, pband, 1));  // same layer still matches
  // secondaryLayer also participates (MS uses primary/secondary).
  fr::Violation sec = pband;
  sec.secondaryLayer = 12;
  CHECK(!fr::sameSignature(pband, sec, 1));
}

void testRelatedness()
{
  RowFixture f = makeCoveredRow();
  const auto move = *fr::makeSwap(f.design, 101, fillerMaster(2, kVt2));
  const fr::Overlay overlay = {move};  // span [4,6) row 0

  // Participant is the changed instance -> related.
  auto direct = makeViolation(2, fr::ViolationKind::MinSpacing,
                              fr::ViolationRelation::IntraRow, {0}, {20, 22});
  fr::ViolationParticipant p;
  p.instanceId = 101;
  direct.participants = {p};
  CHECK(fr::isRelatedToOverlay(direct, overlay, 1));

  // Geometric proximity on the same row -> related.
  auto near = makeViolation(2, fr::ViolationKind::MinSpacing,
                            fr::ViolationRelation::IntraRow, {0}, {6, 7});
  CHECK(fr::isRelatedToOverlay(near, overlay, 1));

  // Same row but far in x -> unrelated.
  auto farX = makeViolation(2, fr::ViolationKind::MinSpacing,
                            fr::ViolationRelation::IntraRow, {0}, {12, 14});
  CHECK(!fr::isRelatedToOverlay(farX, overlay, 1));

  // Near in x but two rows away -> unrelated (rules couple adjacent rows).
  auto farRow = makeViolation(2, fr::ViolationKind::MinSpacing,
                              fr::ViolationRelation::IntraRow, {2}, {5, 6});
  CHECK(!fr::isRelatedToOverlay(farRow, overlay, 1));
}

// --- TODO 5: window builder + guard + unfixable ------------------------------

// Two-row fixture (ScenarioA): anchor std cell 102 [10,14) row0; the VT2
// bridge filler 203 [9,11) row1 sits under it. When opto changes 102 to VT2,
// their VT2 shapes overlap by 1 site -> inter-row MW; the fix swaps 203 back.
//
// Vt Type: 1=vt type 1, 2=vt type 2  |  Widths: {2, 3, 4, 8}
// cell type: 1=std cell, 0=filler    |  Format: (vt type, width, cell type)
// Row 0: (1,8,0) (1,2,0) (1,4,1) (1,2,0)
//   ids:   100     101     102*    103          (* = anchor std cell)
// Row 1: (1,3,0) (1,3,0) (1,3,0) (2,2,0) (1,2,0) (1,3,0)
//   ids:   200     201     202     203     204     205
fr::FakeDesign makeTwoRowDesign()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 16)
      .place(100, fillerMaster(8, kVt1), 0, 0)
      .place(101, fillerMaster(2, kVt1), 0, 8)
      .place(102, cellMaster(kVt1), 0, 10)
      .place(103, fillerMaster(2, kVt1), 0, 14)
      .addRow(1, 0, 16)
      .place(200, fillerMaster(3, kVt1), 1, 0)
      .place(201, fillerMaster(3, kVt1), 1, 3)
      .place(202, fillerMaster(3, kVt1), 1, 6)
      .place(203, fillerMaster(2, kVt2), 1, 9)
      .place(204, fillerMaster(2, kVt1), 1, 11)
      .place(205, fillerMaster(3, kVt1), 1, 13);
  return design;
}

void testWindowL0()
{
  fr::FakeDesign design = makeTwoRowDesign();
  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  request.targetPlace.masterId = cellMaster(kVt2);  // the opto change

  // The seeded inter-row MW between anchor [10,14) and filler 203 [9,11).
  auto v = makeViolation(3, fr::ViolationKind::MinWidth,
                         fr::ViolationRelation::InterRow, {0, 1}, {10, 11});
  fr::ViolationParticipant pf;
  pf.instanceId = 203;
  pf.rowId = 1;
  pf.xRange = {9, 11};
  pf.isFiller = true;
  v.participants = {pf};
  request.violations = {v};

  const auto normalized =
      fr::normalizeViolations(request, design, fr::DebugLog(verbose()));
  const auto window = fr::buildWindow(0, request.targetPlace, normalized,
                                      design, 1, fr::DebugLog(verbose()));

  // Participant 203, anchor-adjacent 101/103, bridge under anchor 204/205
  // ([13,16) overlaps the widened anchor span [9,15)).
  CHECK(window.containsEditable(203));
  CHECK(window.containsEditable(101));
  CHECK(window.containsEditable(103));
  CHECK(window.containsEditable(204));
  CHECK(!window.containsEditable(100));  // [0,8) does not overlap [8,16)
  CHECK(!window.containsEditable(202));  // [6,9) touches 9 only
  // Bridge subset flagged.
  bool bridge203 = false;
  for (const auto id : window.bridgeFillers) {
    bridge203 |= id == 203;
  }
  CHECK(bridge203);
  CHECK_EQ(window.rows.size(), 2u);
}

void testWindowL1ExtendsToFixedBoundary()
{
  fr::FakeDesign design = makeTwoRowDesign();
  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);

  auto v = makeViolation(3, fr::ViolationKind::MinWidth,
                         fr::ViolationRelation::InterRow, {0, 1}, {10, 11});
  request.violations = {v};
  const auto normalized =
      fr::normalizeViolations(request, design, fr::DebugLog(verbose()));

  const auto window = fr::buildWindow(1, request.targetPlace, normalized,
                                      design, 1, fr::DebugLog(verbose()));
  // Row0 left of x=8 is filler 100 -> extension reaches the row edge; row1 is
  // all fillers -> whole row. L1 window covers [0,16) and pulls in 100/200s.
  CHECK(window.x == (fr::XInterval{0, 16}));
  CHECK(window.containsEditable(100));
  CHECK(window.containsEditable(200));
}

void testGuardRegionTwoCellRing()
{
  fr::FakeDesign design = makeTwoRowDesign();
  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);

  auto v = makeViolation(3, fr::ViolationKind::MinWidth,
                         fr::ViolationRelation::InterRow, {0, 1}, {10, 11});
  fr::ViolationParticipant pf;
  pf.instanceId = 203;
  pf.rowId = 1;
  pf.xRange = {9, 11};
  pf.isFiller = true;
  v.participants = {pf};
  request.violations = {v};
  const auto normalized =
      fr::normalizeViolations(request, design, fr::DebugLog(verbose()));

  const auto window = fr::buildWindow(0, request.targetPlace, normalized,
                                      design, 1, fr::DebugLog(verbose()));
  // Guard: rows clamped to the design (0..1); x widened by two instances
  // beyond the window on each side -> reaches the row edges here.
  CHECK_EQ(window.guardRegion.rowLo, 0);
  CHECK_EQ(window.guardRegion.rowHi, 1);
  CHECK(window.guardRegion.x.xl <= 3);   // two instances left of x=8 on row1
  CHECK(window.guardRegion.x.xh >= 16);  // right edge of both rows
  // Guard must always contain the window itself.
  CHECK(window.guardRegion.x.xl <= window.x.xl);
  CHECK(window.guardRegion.x.xh >= window.x.xh);
}

void testEngineUnfixableFastFail()
{
  // A row of std cells only: a violation there has no filler in its ring.
  // V2.1 #6: this is now a warning hint, not a fast-fail. The engine still
  // returns no solution and makes zero checker calls -- but because the search
  // finds no editable filler, not because of an early abort. The
  // UnfixableByTypeSwap diagnostic is still emitted (now as a hint).
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 16)
      .place(100, cellMaster(kVt1), 0, 0)
      .place(101, cellMaster(kVt1), 0, 4)
      .place(102, cellMaster(kVt2), 0, 8)
      .place(103, cellMaster(kVt1), 0, 12);

  fr::FakeImplantChecker checker(design, {});
  fr::FakeCandidateProvider provider(design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(design, checker, provider, config);

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  auto v = makeViolation(2, fr::ViolationKind::MinSpacing,
                         fr::ViolationRelation::IntraRow, {0}, {8, 9});
  request.violations = {v};

  const auto result = engine.repair(request);
  CHECK(!result.hasSolution);
  CHECK(result.changes.empty());
  bool sawUnfixable = false;
  for (const auto& diag : result.diagnostics) {
    sawUnfixable |= diag.code == "UnfixableByTypeSwap";
  }
  CHECK(sawUnfixable);
  CHECK_EQ(checker.requestCount(), 0);  // fail before any checker call
}

void testEngineEmptySnapshotIsSuccess()
{
  RowFixture f = makeCoveredRow();
  fr::FakeImplantChecker checker(f.design, {});
  fr::FakeCandidateProvider provider(f.design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(f.design, checker, provider, config);

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(f.design, f.anchor);
  const auto result = engine.repair(request);
  CHECK(result.hasSolution);
  CHECK(result.changes.empty());
  CHECK_EQ(checker.requestCount(), 0);
}


// --- TODO 6: swap generator ---------------------------------------------------

void testSwapGeneratorBasic()
{
  fr::FakeDesign design = makeTwoRowDesign();
  fr::FakeCandidateProvider provider(design);
  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  request.targetPlace.masterId = cellMaster(kVt2);

  auto v = makeViolation(3, fr::ViolationKind::MinWidth,
                         fr::ViolationRelation::InterRow, {0, 1}, {10, 11});
  fr::ViolationParticipant pf;
  pf.instanceId = 203;
  pf.rowId = 1;
  pf.xRange = {9, 11};
  pf.isFiller = true;
  v.participants = {pf};
  request.violations = {v};

  const auto normalized =
      fr::normalizeViolations(request, design, fr::DebugLog(verbose()));
  const auto window = fr::buildWindow(0, request.targetPlace, normalized,
                                      design, 1, fr::DebugLog(verbose()));
  const auto generated = fr::generateSwaps(window, design, provider,
                                               fr::DebugLog(verbose()));

  // Full library: every editable filler has exactly 2 same-size candidates.
  CHECK_EQ(generated.swaps.size(), window.editableFillers.size() * 2);

  // Deterministic order: window editable order (row, x), then master id.
  // First editable filler is 101 (row0, x=8, w2 vt1) -> masters 22, 23.
  CHECK_EQ(generated.swaps[0].instanceId, 101);
  CHECK_EQ(generated.swaps[0].newMasterId, fillerMaster(2, kVt2));
  CHECK_EQ(generated.swaps[1].instanceId, 101);
  CHECK_EQ(generated.swaps[1].newMasterId, fillerMaster(2, kVt3));

  // Every swap targets an editable filler and never the current master.
  for (const auto& swap : generated.swaps) {
    CHECK(window.containsEditable(swap.instanceId));
    CHECK(swap.newMasterId != swap.oldMasterId);
    CHECK(swap.newVt != swap.oldVt);
  }
}

void testSwapGeneratorNoUsableMaster()
{
  // A width-5 filler exists in exactly one VT: no same-size replacement.
  fr::FakeDesign design = makeLibrary();
  design.addMaster(51, 5, 1, /*isFiller=*/true, kVt1);
  design.addRow(0, 0, 5).place(100, 51, 0, 0);

  fr::FakeCandidateProvider provider(design);
  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 5};
  window.editableFillers = {100};

  const auto generated = fr::generateSwaps(window, design, provider,
                                               fr::DebugLog(verbose()));
  CHECK(generated.swaps.empty());
  bool sawNoUsable = false;
  for (const auto& diag : generated.diagnostics) {
    sawNoUsable |= diag.code == "NoUsableMaster";
  }
  CHECK(sawNoUsable);
}


// --- TODO 7-10: ranker, enumeration, oracle gate, end-to-end -----------------

// Scenario A (inter-row MW): anchor 102 changes VT1->VT2, bridging filler
// 203 (VT2, [9,11) row1) now overlaps the anchor shape by 1 < mwInter=2.
// The checker itself produces the initial snapshot, like the real flow.
struct ScenarioA
{
  fr::FakeDesign design;
  fr::FakeImplantRules rules;
  fr::FillerRepairRequest request;
};

ScenarioA makeScenarioA()
{
  ScenarioA sc;
  sc.design = makeTwoRowDesign();
  sc.rules.mwInter = 2;
  sc.request.targetPlace = anchorPlace(sc.design, 102);
  sc.request.targetPlace.masterId = cellMaster(kVt2);  // the opto change

  fr::FakeImplantChecker snapshotChecker(sc.design, sc.rules);
  auto initial = baselineRequest(sc.design, 102, 0);
  initial.targetPlace.masterId = cellMaster(kVt2);
  sc.request.violations =
      snapshotChecker.checkPlaceWithOverlay(initial).violations;
  return sc;
}

void testRankerOrder()
{
  ScenarioA sc = makeScenarioA();
  const auto normalized =
      fr::normalizeViolations(sc.request, sc.design, fr::DebugLog(verbose()));
  const auto window = fr::buildWindow(0, sc.request.targetPlace, normalized,
                                      sc.design, 2, fr::DebugLog(verbose()));
  fr::FakeCandidateProvider provider(sc.design);
  const auto generated = fr::generateSwaps(window, sc.design, provider,
                                           fr::DebugLog(verbose()));
  const auto ranked =
      fr::rankSwaps(generated.swaps, sc.request.targetPlace, normalized,
                    window, sc.design, fr::DebugLog(verbose()));

  // 203 is the only direct participant -> its swaps rank first; the
  // neighbor-majority (VT1) target beats the demoted third VT (VT3).
  CHECK_EQ(ranked[0].instanceId, 203);
  CHECK_EQ(ranked[0].newVt, kVt1);
  // Third-VT swaps are demoted to the tail but never removed.
  CHECK_EQ(ranked.back().newVt, kVt3);
  CHECK_EQ(ranked.size(), generated.swaps.size());
}

void testEnumerationOrderAndCompleteness()
{
  RowFixture f = makeCoveredRow();
  auto s1 = *fr::makeSwap(f.design, 100, fillerMaster(4, kVt2));
  auto s2 = *fr::makeSwap(f.design, 100, fillerMaster(4, kVt3));
  auto s3 = *fr::makeSwap(f.design, 101, fillerMaster(2, kVt2));
  auto s4 = *fr::makeSwap(f.design, 101, fillerMaster(2, kVt3));
  const std::vector<fr::Swap> ranked = {s1, s2, s3, s4};

  fr::RepairConfig config;
  const auto plan =
      fr::enumerateOverlays(ranked, config, 512, fr::DebugLog(verbose()));
  // Space = (1+2)(1+2)-1 = 8: 4 singles + 4 cross-filler pairs.
  CHECK(plan.complete);
  CHECK_EQ(plan.overlays.size(), 8u);
  CHECK_EQ(plan.overlays[0].size(), 1u);
  CHECK_EQ(plan.overlays[0][0].instanceId, 100);
  // First pair = ranks (0,2): same-filler combos (0,1) are skipped.
  CHECK_EQ(plan.overlays[4].size(), 2u);
  CHECK_EQ(plan.overlays[4][0].instanceId, 100);
  CHECK_EQ(plan.overlays[4][1].instanceId, 101);
  CHECK_EQ(plan.overlays[4][1].newMasterId, fillerMaster(2, kVt2));

  // Tiny budget truncates and clears the completeness claim.
  const auto truncated =
      fr::enumerateOverlays(ranked, config, 3, fr::DebugLog(verbose()));
  CHECK(!truncated.complete);
  CHECK_EQ(truncated.overlays.size(), 3u);
}

void testEngineSolvesSingleSwap()
{
  ScenarioA sc = makeScenarioA();
  fr::FakeImplantChecker checker(sc.design, sc.rules);
  fr::FakeCandidateProvider provider(sc.design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(sc.design, checker, provider, config);

  const auto result = engine.repair(sc.request);
  CHECK(result.hasSolution);
  CHECK_EQ(result.changes.size(), 1u);
  CHECK_EQ(result.changes[0].instanceId, 203);
  CHECK_EQ(result.changes[0].newMasterId, fillerMaster(2, kVt1));
  // One baseline + at most one batch (clean overlay is the top-ranked
  // candidate; the final check is a cache hit, not a new request).
  CHECK(checker.requestCount() <= 1 + config.batchSize);

  // Determinism: same input -> identical outcome and identical call count.
  fr::FakeImplantChecker checker2(sc.design, sc.rules);
  fr::FillerRepairEngine engine2(sc.design, checker2, provider, config);
  const auto result2 = engine2.repair(sc.request);
  CHECK(result2.hasSolution);
  CHECK_EQ(result2.changes.size(), result.changes.size());
  CHECK_EQ(result2.changes[0].instanceId, result.changes[0].instanceId);
  CHECK_EQ(result2.changes[0].newMasterId, result.changes[0].newMasterId);
  CHECK_EQ(checker2.requestCount(), checker.requestCount());
}

// Scenario B (MW-style pair, non-monotone), single row, msIntra=5. Two VT2
// std cells straddle a VT1 gap; the VT2-VT2 spacing and the VT1-VT1 spacing
// both violate. No single swap is clean; recoloring both gap fillers
// 110+111 to VT2 fixes everything.
//
// Vt Type: 1=vt type 1, 2=vt type 2  |  Widths: {2, 4}
// cell type: 1=std cell, 0=filler    |  Format: (vt type, width, cell type)
// Row 0: (2,4,1) (1,2,0) (1,2,0) (2,4,1) (1,4,0)
//   ids:   102*    110     111     112      113     (* = anchor std cell)
void testEngineSolvesPairNonMonotone()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 16)
      .place(102, cellMaster(kVt2), 0, 0)    // anchor (already at new VT)
      .place(110, fillerMaster(2, kVt1), 0, 4)
      .place(111, fillerMaster(2, kVt1), 0, 6)
      .place(112, cellMaster(kVt2), 0, 8)    // fixed std cell, not editable
      .place(113, fillerMaster(4, kVt1), 0, 12);
  fr::FakeImplantRules rules;
  rules.msIntra = 5;

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  fr::FakeImplantChecker snapshotChecker(design, rules);
  request.violations =
      snapshotChecker.checkPlaceWithOverlay(baselineRequest(design, 102, 0))
          .violations;
  CHECK_EQ(request.violations.size(), 2u);  // VT2 MS + VT1 MS

  fr::FakeImplantChecker checker(design, rules);
  fr::FakeCandidateProvider provider(design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(design, checker, provider, config);

  const auto result = engine.repair(request);
  CHECK(result.hasSolution);
  CHECK_EQ(result.changes.size(), 2u);
  CHECK_EQ(result.changes[0].instanceId, 110);
  CHECK_EQ(result.changes[0].newMasterId, fillerMaster(2, kVt2));
  CHECK_EQ(result.changes[1].instanceId, 111);
  CHECK_EQ(result.changes[1].newMasterId, fillerMaster(2, kVt2));
  // All size-1 candidates were evaluated and rejected before the pair won:
  // 6 swaps (3 fillers x 2 usable VTs) + baseline at least.
  CHECK(checker.requestCount() >= 7);
}

// Unrelated pre-existing violation inside the guard halo must not block
// acceptance (spec 6.8 rule 5). Far VT3 inter-row MW at x=[2,3) exists in
// baseline and in every overlay result; the initial snapshot (target-local)
// contains only the anchor-caused violation.
void testEngineIgnoresUnrelatedHaloViolation()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 16)
      .place(130, fillerMaster(3, kVt3), 0, 0)
      .place(131, fillerMaster(2, kVt1), 0, 3)
      .place(132, fillerMaster(3, kVt1), 0, 5)
      .place(101, fillerMaster(2, kVt1), 0, 8)
      .place(102, cellMaster(kVt1), 0, 10)   // anchor, changes to VT2
      .place(103, fillerMaster(2, kVt1), 0, 14)
      .addRow(1, 0, 16)
      .place(200, fillerMaster(2, kVt1), 1, 0)
      .place(206, fillerMaster(2, kVt3), 1, 2)   // overlaps 130 by 1 -> MW
      .place(201, fillerMaster(2, kVt1), 1, 4)
      .place(202, fillerMaster(3, kVt1), 1, 6)
      .place(203, fillerMaster(2, kVt2), 1, 9)
      .place(204, fillerMaster(2, kVt1), 1, 11)
      .place(205, fillerMaster(3, kVt1), 1, 13);
  fr::FakeImplantRules rules;
  rules.mwInter = 2;

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  request.targetPlace.masterId = cellMaster(kVt2);

  // Target-local initial snapshot derived from the checker (as production does,
  // so signatures/layers match the baseline): a narrow guard around the anchor
  // captures only the anchor-caused MW and excludes the far VT3 pre-existing
  // violation at x=[2,3). That pre-existing MW then appears only in the engine's
  // wider baseline -- exactly the unrelated-halo case under test.
  fr::FakeImplantChecker snapshotChecker(design, rules);
  auto snapReq = baselineRequest(design, 102, 0);
  snapReq.targetPlace.masterId = cellMaster(kVt2);  // the opto change
  snapReq.guardRegion = fr::Region{fr::XInterval{8, 16}, 0, 1};
  request.violations =
      snapshotChecker.checkPlaceWithOverlay(snapReq).violations;
  CHECK_EQ(request.violations.size(), 1u);  // only the anchor-caused MW

  fr::FakeImplantChecker checker(design, rules);
  fr::FakeCandidateProvider provider(design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(design, checker, provider, config);

  const auto result = engine.repair(request);
  // The pre-existing VT3 MW sits in the baseline of the same guard region;
  // being unrelated to any changed filler it must not veto the fix.
  CHECK(result.hasSolution);
  CHECK_EQ(result.changes.size(), 1u);
  CHECK_EQ(result.changes[0].instanceId, 203);
}

// mwIntra=100 makes every run violate: no overlay can ever be clean. The
// window space is tiny -> complete enumeration -> definitive no-solution,
// and L1 triggers the expansion cutoff (same editable set).
void testEngineNoSolutionDefinitive()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 8)
      .place(102, cellMaster(kVt2), 0, 0)  // anchor
      .place(120, fillerMaster(2, kVt1), 0, 4)
      .place(121, fillerMaster(2, kVt2), 0, 6);
  fr::FakeImplantRules rules;
  rules.mwIntra = 100;

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  fr::FakeImplantChecker snapshotChecker(design, rules);
  request.violations =
      snapshotChecker.checkPlaceWithOverlay(baselineRequest(design, 102, 0))
          .violations;
  CHECK(!request.violations.empty());

  fr::FakeImplantChecker checker(design, rules);
  fr::FakeCandidateProvider provider(design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(design, checker, provider, config);

  const auto result = engine.repair(request);
  CHECK(!result.hasSolution);
  CHECK(result.changes.empty());
  bool sawNoClean = false;
  bool sawDefinitive = false;
  bool sawCutoff = false;
  bool sawBest = false;
  for (const auto& diag : result.diagnostics) {
    sawNoClean |= diag.code == "NoCleanOverlay";
    sawDefinitive |= diag.code == "NoCleanOverlay"
                     && diag.message.find("definitively") != std::string::npos;
    sawCutoff |= diag.code == "ExpansionCutoff";
    sawBest |= diag.code == "BestOverlay";
  }
  CHECK(sawNoClean);
  CHECK(sawDefinitive);
  CHECK(sawCutoff);
  CHECK(sawBest);
}

// Gate-level cache: the same overlay under the same guard hits the checker
// exactly once, including the baseline.
void testGateCacheSingleEvaluation()
{
  ScenarioA sc = makeScenarioA();
  fr::FakeImplantChecker checker(sc.design, sc.rules);
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::OracleGate gate(checker, sc.request.targetPlace, sc.request.violations,
                      1, 2, config, log);

  const auto normalized =
      fr::normalizeViolations(sc.request, sc.design, log);
  const auto window = fr::buildWindow(0, sc.request.targetPlace, normalized,
                                      sc.design, 2, log);
  auto swap = *fr::makeSwap(sc.design, 204, fillerMaster(2, kVt2));
  const fr::Overlay o1 = {swap};

  int budget = 100;
  CHECK(gate.runBaseline(window, budget));
  CHECK(gate.runBaseline(window, budget));  // cache hit
  (void) gate.search({o1}, window, window.guardRegion, budget);
  (void) gate.search({o1}, window, window.guardRegion, budget);  // cache hit
  CHECK_EQ(checker.requestCount(), 2);  // baseline + o1, each exactly once
  CHECK(gate.cacheHits() >= 2);
}

// A checker that violates the requestId echo protocol must abort the repair
// with CheckerProtocolError instead of producing a result.
class MisbehavingChecker : public fr::ImplantOverlayChecker
{
 public:
  explicit MisbehavingChecker(fr::FakeImplantChecker& inner) : inner_(inner) {}
  fr::CheckResult checkPlaceWithOverlay(const fr::OverlayCheckRequest& request) override
  {
    return inner_.checkPlaceWithOverlay(request);  // baseline stays honest
  }
  std::vector<fr::CheckResult> checkPlaceWithOverlays(
      const std::vector<fr::OverlayCheckRequest>& requests) override
  {
    auto results = inner_.checkPlaceWithOverlays(requests);
    for (auto& result : results) {
      result.requestId = -42;  // corrupt every echo
    }
    return results;
  }
 private:
  fr::FakeImplantChecker& inner_;
};

void testEngineDetectsProtocolError()
{
  ScenarioA sc = makeScenarioA();
  fr::FakeImplantChecker inner(sc.design, sc.rules);
  MisbehavingChecker checker(inner);
  fr::FakeCandidateProvider provider(sc.design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(sc.design, checker, provider, config);

  const auto result = engine.repair(sc.request);
  CHECK(!result.hasSolution);
  CHECK(result.changes.empty());
  bool sawProtocol = false;
  for (const auto& diag : result.diagnostics) {
    sawProtocol |= diag.code == "CheckerProtocolError";
  }
  CHECK(sawProtocol);
}

// Batch result order must not matter: a checker returning results reversed
// (with honest ids) yields the identical solution.
class ReversingChecker : public fr::ImplantOverlayChecker
{
 public:
  explicit ReversingChecker(fr::FakeImplantChecker& inner) : inner_(inner) {}
  fr::CheckResult checkPlaceWithOverlay(const fr::OverlayCheckRequest& request) override
  {
    return inner_.checkPlaceWithOverlay(request);
  }
  std::vector<fr::CheckResult> checkPlaceWithOverlays(
      const std::vector<fr::OverlayCheckRequest>& requests) override
  {
    auto results = inner_.checkPlaceWithOverlays(requests);
    std::reverse(results.begin(), results.end());
    return results;
  }
 private:
  fr::FakeImplantChecker& inner_;
};

void testEngineOrderIndependentBatches()
{
  ScenarioA sc = makeScenarioA();
  fr::FakeImplantChecker inner(sc.design, sc.rules);
  ReversingChecker checker(inner);
  fr::FakeCandidateProvider provider(sc.design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(sc.design, checker, provider, config);

  const auto result = engine.repair(sc.request);
  CHECK(result.hasSolution);
  CHECK_EQ(result.changes.size(), 1u);
  CHECK_EQ(result.changes[0].instanceId, 203);
  CHECK_EQ(result.changes[0].newMasterId, fillerMaster(2, kVt1));
}


// Scripted checker: fixed violation sets per overlay key, honest protocol.
// Lets us hit each delta-classification branch exactly (spec 6.8).
class ScriptedChecker : public fr::ImplantOverlayChecker
{
 public:
  std::map<std::string, std::vector<fr::Violation>> byKey;

  static std::string keyOf(const std::vector<fr::FillerChange>& changes)
  {
    std::vector<fr::FillerChange> sorted = changes;
    std::sort(sorted.begin(), sorted.end(),
              [](const fr::FillerChange& a, const fr::FillerChange& b) {
                return a.instanceId != b.instanceId
                           ? a.instanceId < b.instanceId
                           : a.newMasterId < b.newMasterId;
              });
    std::string key;
    for (const auto& c : sorted) {
      key += std::to_string(c.instanceId) + ">" + std::to_string(c.newMasterId) + "|";
    }
    return key;
  }

  fr::CheckResult checkPlaceWithOverlay(const fr::OverlayCheckRequest& request) override
  {
    fr::CheckResult result;
    result.requestId = request.requestId;
    result.status = fr::CheckStatus::Checked;
    result.violations = byKey[keyOf(request.fillerChanges)];
    result.isLegal = result.violations.empty();
    return result;
  }
  std::vector<fr::CheckResult> checkPlaceWithOverlays(
      const std::vector<fr::OverlayCheckRequest>& requests) override
  {
    std::vector<fr::CheckResult> results;
    for (const auto& request : requests) {
      results.push_back(checkPlaceWithOverlay(request));
    }
    return results;
  }
};

// Three overlays, three classification outcomes: a new violation inside the
// repair window rejects; a related new violation in the guard halo rejects;
// an unrelated pre-existing-style halo violation does not block.
void testGateDeltaClassificationBranches()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20)
      .place(500, fillerMaster(2, kVt1), 0, 4)
      .place(501, fillerMaster(2, kVt1), 0, 14);

  const fr::Violation original =
      makeViolation(1, fr::ViolationKind::MinWidth,
                    fr::ViolationRelation::IntraRow, {0}, {2, 4});
  const auto newInWindow =
      makeViolation(9, fr::ViolationKind::MinSpacing,
                    fr::ViolationRelation::IntraRow, {0}, {5, 6});
  const auto relatedInHalo =  // touches 501's span [14,16)
      makeViolation(9, fr::ViolationKind::MinSpacing,
                    fr::ViolationRelation::IntraRow, {0}, {15, 16});
  const auto unrelatedInHalo =  // 12 sites away from 500's span [4,6)
      makeViolation(9, fr::ViolationKind::MinSpacing,
                    fr::ViolationRelation::IntraRow, {0}, {18, 19});

  const fr::Overlay o1 = {*fr::makeSwap(design, 500, fillerMaster(2, kVt2))};
  const fr::Overlay o2 = {*fr::makeSwap(design, 501, fillerMaster(2, kVt2))};
  const fr::Overlay o3 = {*fr::makeSwap(design, 500, fillerMaster(2, kVt3))};

  ScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] = {original};
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(o1))] = {newInWindow};
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(o2))] = {relatedInHalo};
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(o3))] = {unrelatedInHalo};

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};

  const fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::OracleGate gate(checker, anchor, originals, /*siteWidth=*/1,
                      /*ruleDistance=*/2, config, log);

  int budget = 100;
  CHECK(gate.runBaseline(window, budget));
  const auto sr = gate.search({o1, o2, o3}, window, window.guardRegion, budget);

  // o1/o2 rejected for the pinned reasons; o3 accepted despite the unrelated
  // halo violation (and despite isLegal=false in its raw result).
  CHECK(sr.foundClean);
  CHECK_EQ(sr.cleanOverlay.size(), 1u);
  CHECK_EQ(sr.cleanOverlay[0].instanceId, 500);
  CHECK_EQ(sr.cleanOverlay[0].newMasterId, fillerMaster(2, kVt3));
  CHECK(sr.hasBest);
  CHECK_EQ(sr.bestSummary.newInWindow, 1);  // o1 was the best-tracked reject
}

// --- V2.1 batch-1 correctness regressions ----------------------------------

// Returns an unexplained-illegal result (Checked, isLegal=false, no violations)
// for any candidate overlay; the baseline honestly reproduces the original.
// This is the shape the real checker returns for a blocking overlap / off-grid
// / polarity mismatch, which the fake checker never produces.
class IllegalEmptyChecker : public fr::ImplantOverlayChecker
{
 public:
  fr::Violation original;
  fr::CheckResult checkPlaceWithOverlay(const fr::OverlayCheckRequest& r) override
  {
    fr::CheckResult res;
    res.requestId = r.requestId;
    res.status = fr::CheckStatus::Checked;
    if (r.fillerChanges.empty()) {
      res.violations = {original};  // baseline: original present
      res.isLegal = false;
    } else {
      res.isLegal = false;  // candidate: original cleared but result is illegal
    }
    return res;
  }
  std::vector<fr::CheckResult> checkPlaceWithOverlays(
      const std::vector<fr::OverlayCheckRequest>& rs) override
  {
    std::vector<fr::CheckResult> out;
    for (const auto& r : rs) {
      out.push_back(checkPlaceWithOverlay(r));
    }
    return out;
  }
};

// V2.1 #1: an unexplained illegal result (isLegal=false with no violations)
// must be rejected, not accepted as clean just because no violation is listed.
void testGateRejectsUnexplainedIllegal()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20).place(500, fillerMaster(2, kVt1), 0, 4);
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});

  IllegalEmptyChecker checker;
  checker.original = original;

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  const fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  CHECK(gate.runBaseline(window, budget));
  const fr::Overlay o = {*fr::makeSwap(design, 500, fillerMaster(2, kVt2))};
  const auto sr = gate.search({o}, window, window.guardRegion, budget);
  CHECK(!sr.foundClean);               // NOT accepted
  CHECK(sr.hasBest);
  CHECK(sr.bestSummary.inconsistent);  // rejected for self-inconsistency
}

// V2.1 #2: a baseline that fails to reproduce an in-guard original means the
// input snapshot is stale -- the gate must refuse to search (BaselineMismatch),
// not silently treat "not observed" as "repaired".
void testGateBaselineMismatchAbortsSearch()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20).place(500, fillerMaster(2, kVt1), 0, 4);
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});

  ScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] = {};  // baseline missing the original

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  const fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  CHECK(!gate.runBaseline(window, budget));  // consistency gate fails
  bool sawMismatch = false;
  for (const auto& d : gate.diagnostics()) {
    sawMismatch |= d.code == "BaselineMismatch";
  }
  CHECK(sawMismatch);
}

// V2.1 #3: two candidate violations of the same signature must not both be
// absorbed by a single baseline finding. One baseline H, two candidate H ->
// the second H is genuinely new (here related-in-halo -> reject).
void testGateMultisetNewViolationNotAbsorbed()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20)
      .place(500, fillerMaster(2, kVt1), 0, 4)
      .place(501, fillerMaster(2, kVt1), 0, 14);
  const fr::Violation O = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  const fr::Violation H = makeViolation(  // halo finding, touches 501's span
      9, fr::ViolationKind::MinSpacing, fr::ViolationRelation::IntraRow, {0}, {15, 16});

  const fr::Overlay o = {*fr::makeSwap(design, 501, fillerMaster(2, kVt2))};
  ScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] = {O, H};  // baseline: original + one H
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(o))] = {H, H};  // O gone, two H

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  const fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {O};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  CHECK(gate.runBaseline(window, budget));  // H is out-of-window -> baseline consistent
  const auto sr = gate.search({o}, window, window.guardRegion, budget);
  CHECK(!sr.foundClean);                     // the second H is not absorbed
  CHECK_EQ(sr.bestSummary.relatedInHalo, 1);
}

// V2.1 #5: relatedness of a NEW violation uses max(originalRuleDistance, its
// own requiredValue). A new violation from a larger-distance rule must be seen
// as related (and reject), not mislabeled unrelated and let through.
void testGatePerViolationRuleDistance()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20).place(500, fillerMaster(2, kVt1), 0, 4);
  const fr::Violation O = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  fr::Violation N = makeViolation(  // new, 5 sites from the swap span [4,6)
      9, fr::ViolationKind::MinSpacing, fr::ViolationRelation::IntraRow, {0}, {11, 12});
  N.requiredValue = 6;  // larger-distance rule than the originals

  const fr::Overlay o = {*fr::makeSwap(design, 500, fillerMaster(2, kVt2))};
  ScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] = {O};
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(o))] = {N};  // O gone, N appears

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  const fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {O};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  // Original rule distance is small (2); only max(2, N.requiredValue=6)=6 makes
  // N (5 away) count as related.
  fr::OracleGate gate(checker, anchor, originals, 1, /*ruleDistance=*/2, config, log);

  int budget = 100;
  CHECK(gate.runBaseline(window, budget));
  const auto sr = gate.search({o}, window, window.guardRegion, budget);
  CHECK(!sr.foundClean);
  CHECK_EQ(sr.bestSummary.relatedInHalo, 1);
  CHECK_EQ(sr.bestSummary.unrelatedInHalo, 0);
}

// A checker for which no overlay is ever clean: it returns the original
// violation for the baseline AND for every candidate. Lets an engine test
// drive the window/definitive control flow without the fake rule model.
class AlwaysUnsolvedChecker : public fr::ImplantOverlayChecker
{
 public:
  fr::Violation original;
  fr::CheckResult checkPlaceWithOverlay(const fr::OverlayCheckRequest& r) override
  {
    fr::CheckResult res;
    res.requestId = r.requestId;
    res.status = fr::CheckStatus::Checked;
    res.violations = {original};  // baseline and every candidate stay unsolved
    res.isLegal = false;
    return res;
  }
  std::vector<fr::CheckResult> checkPlaceWithOverlays(
      const std::vector<fr::OverlayCheckRequest>& rs) override
  {
    std::vector<fr::CheckResult> out;
    for (const auto& r : rs) {
      out.push_back(checkPlaceWithOverlay(r));
    }
    return out;
  }
};

// V2.1 #10: "definitive no solution" must reflect the LAST searched window.
// A single contiguous filler run: L0 is just the anchor-touching participant
// filler (space 2, fully enumerated) while L1's sideways sweep pulls in the
// whole run (7 fillers, space 3^7 -> budget-truncated). The failure must read
// "truncated", not "definitively" -- the old OR-accumulator latched L0's
// completeness and would have mislabeled it definitive.
void testEngineDefinitiveReflectsLastWindow()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 18)
      .place(102, cellMaster(kVt2), 0, 0)   // anchor [0,4)
      .place(140, fillerMaster(2, kVt1), 0, 4)   // touches anchor -> in L0
      .place(141, fillerMaster(2, kVt1), 0, 6)
      .place(142, fillerMaster(2, kVt1), 0, 8)
      .place(143, fillerMaster(2, kVt1), 0, 10)
      .place(144, fillerMaster(2, kVt1), 0, 12)
      .place(145, fillerMaster(2, kVt1), 0, 14)
      .place(146, fillerMaster(2, kVt1), 0, 16);

  fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {4, 6});
  fr::ViolationParticipant pf;
  pf.instanceId = 140;
  pf.rowId = 0;
  pf.xRange = {4, 6};
  pf.isFiller = true;
  original.participants = {pf};

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  request.targetPlace.masterId = cellMaster(kVt2);
  request.violations = {original};

  AlwaysUnsolvedChecker checker;
  checker.original = original;
  fr::FakeCandidateProvider provider(design);
  fr::RepairConfig config;
  config.verbose = verbose();
  // L0 (1 filler, space 2) fits and is complete; L1 (7 fillers) far exceeds 50.
  config.checkerCallBudgetPerWindow = 50;
  fr::FillerRepairEngine engine(design, checker, provider, config);

  const auto result = engine.repair(request);
  CHECK(!result.hasSolution);
  bool sawTruncated = false;
  bool sawDefinitive = false;
  for (const auto& d : result.diagnostics) {
    if (d.code == "NoCleanOverlay") {
      sawTruncated |= d.message.find("truncated") != std::string::npos;
      sawDefinitive |= d.message.find("definitively") != std::string::npos;
    }
  }
  CHECK(sawTruncated);
  CHECK(!sawDefinitive);
}

// --- User-provided realistic grid ------------------------------------------
//
// A 5-row multi-width layout with two VT types (0/1) and four std cells,
// supplied to exercise the engine at scale. Its own master scheme (VTs {0,1},
// widths {2,3,4,8}) is kept separate from the {1,2,3}-VT library above.
//
// NOTE ON SEMANTICS: the fake checker is a simplified run-based MW/MS model,
// not the real implant checker. At MW=MS=1 it reports the violations it can
// see on this static layout (a corner-touch inter-row MS near rows 2-3), not
// necessarily the ones the author had in mind. That is exactly the
// checker-as-oracle boundary: the engine repairs whatever the checker
// reports, and the real checker will drive the intended violations unchanged.

namespace grid {

fr::MasterId filler(int w, int vt) { return static_cast<fr::MasterId>(w * 10 + vt); }
fr::MasterId cell(int w, int vt) { return static_cast<fr::MasterId>(900 + w * 10 + vt); }

// Vt Type: 0=vt type 0, 1=vt type 1  |  Widths: {2, 3, 4, 8}
// cell type: 1=std cell, 0=filler    |  Format: (vt type, width, cell type)
// Instance id = row*1000 + column index (Row 0 col 0 -> 0, Row 2 col 4 ->
// 2004, ...). Std cells: 1008=(1,3,1), 1011=(1,2,1), 2004=(1,4,1),
// 2011=(0,8,1). Rows exactly as supplied:
const std::vector<std::vector<std::tuple<int, int, int>>> kRows = {
    {{1,3,0},{1,8,0},{1,4,0},{1,2,0},{1,8,0},{0,2,0},{0,2,0},{0,2,0},{1,4,0},{0,4,0},{1,3,0},{1,3,0},{1,2,0},{0,3,0},{1,4,0},{1,2,0},{0,3,0},{1,3,0},{1,2,0}},
    {{1,2,0},{1,3,0},{1,3,0},{1,4,0},{0,2,0},{0,4,0},{0,2,0},{0,8,0},{1,3,1},{1,3,0},{0,2,0},{1,2,1},{0,4,0},{1,3,0},{0,3,0},{0,2,0},{1,2,0},{0,2,0},{1,4,0},{1,2,0},{1,4,0}},
    {{1,3,0},{1,3,0},{1,4,0},{0,3,0},{1,4,1},{0,3,0},{0,4,0},{1,8,0},{1,3,0},{1,3,0},{0,3,0},{0,8,1},{1,4,0},{1,4,0},{1,3,0},{1,4,0}},
    {{1,3,0},{1,3,0},{1,3,0},{0,8,0},{0,2,0},{0,2,0},{0,2,0},{1,4,0},{1,3,0},{1,3,0},{0,4,0},{0,4,0},{1,8,0},{0,8,0},{0,2,0},{0,2,0},{1,3,0}},
    {{1,3,0},{1,8,0},{1,4,0},{1,2,0},{1,2,0},{1,3,0},{1,8,0},{1,4,0},{1,3,0},{1,4,0},{1,3,0},{1,3,0},{1,2,0},{1,3,0},{1,3,0},{1,2,0},{1,2,0},{1,3,0},{1,2,0}},
};

// Instance id = row*1000 + column index. Std cells: 1008,1011,2004,2011.
fr::FakeDesign build()
{
  fr::FakeDesign d;
  d.setSiteWidth(1);
  for (const int w : {2, 3, 4, 8}) {
    for (const int vt : {0, 1}) {
      d.addMaster(filler(w, vt), w, 1, /*isFiller=*/true, vt);
      d.addMaster(cell(w, vt), w, 1, /*isFiller=*/false, vt);
    }
  }
  for (int r = 0; r < static_cast<int>(kRows.size()); ++r) {
    int x = 0;
    for (int i = 0; i < static_cast<int>(kRows[r].size()); ++i) {
      const auto& c = kRows[r][i];
      const int vt = std::get<0>(c), w = std::get<1>(c), isCell = std::get<2>(c);
      d.place(r * 1000 + i, isCell ? cell(w, vt) : filler(w, vt), r, x);
      x += w;
    }
    d.addRow(r, 0, x);
  }
  return d;
}

}  // namespace grid

// The engine runs end-to-end on the large layout and deterministically
// repairs what the fake checker reports at MW=MS=1: a single inter-row MS
// near rows 2-3, cleared by one filler swap. Anchored at the width-2 std cell
// (1011); the window follows the violation footprint to reach the fix.
void testEngineUserGridMwMs1()
{
  fr::FakeDesign design = grid::build();
  fr::FakeImplantRules rules;  // MW=MS=1 on all four rule classes
  rules.mwIntra = 1;
  rules.msIntra = 1;
  rules.mwInter = 1;
  rules.msInter = 1;

  fr::TargetPlace anchor;  // width-2 std cell 1011 (row1, x=36)
  anchor.instanceId = 1011;
  anchor.masterId = grid::cell(2, 1);
  anchor.rowId = 1;
  anchor.x = 36;

  fr::FakeImplantChecker snapshotChecker(design, rules);
  fr::OverlayCheckRequest snapReq;
  snapReq.requestId = 0;
  snapReq.targetPlace = anchor;
  snapReq.guardRegion = fr::Region{fr::XInterval{-1, 1000}, 0, 4};
  const auto snapshot = snapshotChecker.checkPlaceWithOverlay(snapReq).violations;
  CHECK_EQ(snapshot.size(), 2u);  // two corner-touch inter-row MS at [49,50)

  fr::FakeImplantChecker checker(design, rules);
  fr::FakeCandidateProvider provider(design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(design, checker, provider, config);

  fr::FillerRepairRequest request;
  request.targetPlace = anchor;
  request.violations = snapshot;
  const auto result = engine.repair(request);

  // Deterministic solution: swap the row3 width-8 vt0 filler 3013 to vt1.
  CHECK(result.hasSolution);
  CHECK_EQ(result.changes.size(), 1u);
  CHECK_EQ(result.changes[0].instanceId, 3013);
  CHECK_EQ(result.changes[0].newMasterId, grid::filler(8, 1));

  // Same input -> identical result (planner determinism).
  fr::FakeImplantChecker checker2(design, rules);
  fr::FillerRepairEngine engine2(design, checker2, provider, config);
  const auto result2 = engine2.repair(request);
  CHECK(result2.hasSolution);
  CHECK_EQ(result2.changes.size(), 1u);
  CHECK_EQ(result2.changes[0].instanceId, 3013);
}

}  // namespace

int main(int argc, char** argv)
{
  // Optional name filter: `fillerRepair_tests <substring>` runs only the
  // tests whose name contains <substring>. With FR_VERBOSE=1 this isolates
  // one case's full [fr] transcript.
  const char* filter = argc > 1 ? argv[1] : nullptr;

  const std::vector<Test> tests = {
      {"swap_construction", testSwapConstruction},
      {"canonical_key_order_independent", testCanonicalKeyOrderIndependent},
      {"wire_adapter", testWireAdapter},
      {"precheck_full_utility", testPreCheckFullUtility},
      {"precheck_gap", testPreCheckGap},
      {"precheck_overlap_offgrid_illegal", testPreCheckOverlapOffGridIllegal},
      {"engine_fatal_on_gap_without_checker_calls",
       testEngineFatalOnGapWithoutCheckerCalls},
      {"candidate_provider", testCandidateProvider},
      {"checker_echo_and_order", testCheckerEchoAndOrder},
      {"checker_invalid_isolated", testCheckerInvalidIsolated},
      {"checker_intra_ms_detect_and_clear", testCheckerIntraMsDetectAndClear},
      {"checker_inter_row_rules", testCheckerInterRowRules},
      {"checker_guard_region_filter", testCheckerGuardRegionFilter},
      {"checker_target_override_seeds_violation",
       testCheckerTargetOverrideSeedsViolation},
      {"normalize_violations", testNormalizeViolations},
      {"signature_matching", testSignatureMatching},
      {"relatedness", testRelatedness},
      {"window_L0", testWindowL0},
      {"window_L1_extends_to_fixed_boundary", testWindowL1ExtendsToFixedBoundary},
      {"guard_region_two_cell_ring", testGuardRegionTwoCellRing},
      {"engine_unfixable_fast_fail", testEngineUnfixableFastFail},
      {"engine_empty_snapshot_is_success", testEngineEmptySnapshotIsSuccess},
      {"swap_generator_basic", testSwapGeneratorBasic},
      {"swap_generator_no_usable_master", testSwapGeneratorNoUsableMaster},
      {"ranker_order", testRankerOrder},
      {"enumeration_order_and_completeness", testEnumerationOrderAndCompleteness},
      {"engine_solves_single_swap", testEngineSolvesSingleSwap},
      {"engine_solves_pair_non_monotone", testEngineSolvesPairNonMonotone},
      {"engine_ignores_unrelated_halo_violation",
       testEngineIgnoresUnrelatedHaloViolation},
      {"engine_no_solution_definitive", testEngineNoSolutionDefinitive},
      {"gate_cache_single_evaluation", testGateCacheSingleEvaluation},
      {"engine_detects_protocol_error", testEngineDetectsProtocolError},
      {"engine_order_independent_batches", testEngineOrderIndependentBatches},
      {"gate_delta_classification_branches", testGateDeltaClassificationBranches},
      {"gate_rejects_unexplained_illegal", testGateRejectsUnexplainedIllegal},
      {"gate_baseline_mismatch_aborts_search", testGateBaselineMismatchAbortsSearch},
      {"gate_multiset_new_violation_not_absorbed",
       testGateMultisetNewViolationNotAbsorbed},
      {"gate_per_violation_rule_distance", testGatePerViolationRuleDistance},
      {"engine_definitive_reflects_last_window",
       testEngineDefinitiveReflectsLastWindow},
      {"engine_user_grid_mw_ms_1", testEngineUserGridMwMs1},
  };

  size_t ran = 0;
  for (const Test& test : tests) {
    if (filter != nullptr && std::strstr(test.name, filter) == nullptr) {
      continue;
    }
    g_current = test.name;
    if (verbose()) {
      std::printf("\n===== case: %s =====\n", test.name);
    }
    test.fn();
    ++ran;
  }

  if (ran == 0) {
    std::printf("no test matched filter \"%s\"\n", filter ? filter : "");
    return 1;
  }
  if (g_failures == 0) {
    std::printf("OK: %zu test(s) passed\n", ran);
    return 0;
  }
  std::printf("FAILED: %d check(s) across %zu test(s)\n", g_failures, ran);
  return 1;
}
