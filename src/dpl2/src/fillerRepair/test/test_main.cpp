// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Unit tests for spec section 11 TODO items 1-3: planner API + Move
// abstraction, fake checker / fake candidate provider protocol, and the
// 100% utility pre-check. Plain C++17, no external test framework so the
// suite runs before dpl2 is wired into the CMake build.
//
// Run: test/run_tests.sh   (FR_VERBOSE=1 prints the [fr] debug transcript)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../FillerRepairEngine.h"
#include "../Move.h"
#include "../PreCheck.h"
#include "../Signature.h"
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

// One fully covered row [0,16):
//   x:      0        4      6        10       12         16
//   inst: 100(F w4) 101(F w2) 102(F w4) 103(C w2*)  104(F w4)
// Layout below uses fillers except inst 103; callers adjust as needed.
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

// --- TODO 1: Move abstraction ----------------------------------------------

void testSwapMoveConstruction()
{
  RowFixture f = makeCoveredRow();
  std::string error;

  // Valid swap: filler 101 (w2 vt1) -> w2 vt2 master.
  auto move = fr::makeSwapMove(f.design, 101, fillerMaster(2, kVt2), &error);
  CHECK(move.has_value());
  CHECK_EQ(move->rowId, 0);
  CHECK(move->span == (fr::XInterval{4, 6}));
  CHECK_EQ(move->oldVt, kVt1);
  CHECK_EQ(move->newVt, kVt2);

  // Rejections, each with a reason.
  CHECK(!fr::makeSwapMove(f.design, 999, fillerMaster(2, kVt2), &error).has_value());
  CHECK(!fr::makeSwapMove(f.design, 101, fillerMaster(4, kVt2), &error).has_value());
  CHECK(!error.empty());  // size mismatch reason recorded
  CHECK(!fr::makeSwapMove(f.design, 101, fillerMaster(2, kVt1), &error).has_value());
  CHECK(!fr::makeSwapMove(f.design, 101, cellMaster(kVt2), &error).has_value());

  // Std cell is never a move target.
  f.design.remove(103).place(103, cellMaster(kVt2), 0, 10);
  CHECK(!fr::makeSwapMove(f.design, 103, fillerMaster(2, kVt1), &error).has_value());
}

void testCanonicalKeyOrderIndependent()
{
  RowFixture f = makeCoveredRow();
  auto m1 = *fr::makeSwapMove(f.design, 100, fillerMaster(4, kVt2));
  auto m2 = *fr::makeSwapMove(f.design, 101, fillerMaster(2, kVt2));

  CHECK(fr::canonicalKey({m1, m2}) == fr::canonicalKey({m2, m1}));
  CHECK(fr::canonicalKey({m1}) != fr::canonicalKey({m1, m2}));
  // Same instance, different target master => different overlay.
  auto m1b = *fr::makeSwapMove(f.design, 100, fillerMaster(4, kVt3));
  CHECK(fr::canonicalKey({m1}) != fr::canonicalKey({m1b}));
}

void testConflictDetection()
{
  RowFixture f = makeCoveredRow();
  auto m1 = *fr::makeSwapMove(f.design, 100, fillerMaster(4, kVt2));
  auto m1b = *fr::makeSwapMove(f.design, 100, fillerMaster(4, kVt3));
  auto m2 = *fr::makeSwapMove(f.design, 101, fillerMaster(2, kVt2));

  // Same instance twice: two swaps of one filler cannot be atomic.
  CHECK(fr::overlayHasConflict({m1, m1b}));
  // Different instances never conflict in swap-only mode.
  CHECK(!fr::overlayHasConflict({m1, m2}));
}

void testWireAdapter()
{
  RowFixture f = makeCoveredRow();
  auto m1 = *fr::makeSwapMove(f.design, 101, fillerMaster(2, kVt2));
  auto m2 = *fr::makeSwapMove(f.design, 100, fillerMaster(4, kVt3));

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
}

void testRelatedness()
{
  RowFixture f = makeCoveredRow();
  const auto move = *fr::makeSwapMove(f.design, 101, fillerMaster(2, kVt2));
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

// Two-row fixture from testCheckerTargetOverrideSeedsViolation: anchor cell
// 102 [10,14) row0, bridge filler 203 [9,11) row1.
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
  const auto generated = fr::generateSwapMoves(window, design, provider,
                                               fr::DebugLog(verbose()));

  // Full library: every editable filler has exactly 2 same-size candidates.
  CHECK_EQ(generated.moves.size(), window.editableFillers.size() * 2);

  // Deterministic order: window editable order (row, x), then master id.
  // First editable filler is 101 (row0, x=8, w2 vt1) -> masters 22, 23.
  CHECK_EQ(generated.moves[0].instanceId, 101);
  CHECK_EQ(generated.moves[0].newMasterId, fillerMaster(2, kVt2));
  CHECK_EQ(generated.moves[1].instanceId, 101);
  CHECK_EQ(generated.moves[1].newMasterId, fillerMaster(2, kVt3));

  // Every move targets an editable filler and never the current master.
  for (const auto& move : generated.moves) {
    CHECK(window.containsEditable(move.instanceId));
    CHECK(move.newMasterId != move.oldMasterId);
    CHECK(move.newVt != move.oldVt);
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

  const auto generated = fr::generateSwapMoves(window, design, provider,
                                               fr::DebugLog(verbose()));
  CHECK(generated.moves.empty());
  bool sawNoUsable = false;
  for (const auto& diag : generated.diagnostics) {
    sawNoUsable |= diag.code == "NoUsableMaster";
  }
  CHECK(sawNoUsable);
}

void testEngineReachesMoveGeneration()
{
  // Normal request passes precheck/normalize/window/movegen and stops at the
  // explicit NotImplemented of the pending search stages -- proving move
  // generation produced work without touching the checker yet.
  fr::FakeDesign design = makeTwoRowDesign();
  fr::FakeImplantChecker checker(design, {});
  fr::FakeCandidateProvider provider(design);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::FillerRepairEngine engine(design, checker, provider, config);

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

  const auto result = engine.repair(request);
  CHECK(!result.hasSolution);
  bool sawNotImplemented = false;
  bool sawNoMove = false;
  for (const auto& diag : result.diagnostics) {
    sawNotImplemented |= diag.code == "NotImplemented";
    sawNoMove |= diag.code == "NoMoveGenerated";
  }
  CHECK(sawNotImplemented);
  CHECK(!sawNoMove);
  CHECK_EQ(checker.requestCount(), 0);  // search not wired yet
}

}  // namespace

int main()
{
  const std::vector<Test> tests = {
      {"swap_move_construction", testSwapMoveConstruction},
      {"canonical_key_order_independent", testCanonicalKeyOrderIndependent},
      {"conflict_detection", testConflictDetection},
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
      {"engine_reaches_move_generation", testEngineReachesMoveGeneration},
  };

  for (const Test& test : tests) {
    g_current = test.name;
    test.fn();
    if (verbose()) {
      std::printf("ran  %s\n", test.name);
    }
  }

  if (g_failures == 0) {
    std::printf("OK: %zu tests passed\n", tests.size());
    return 0;
  }
  std::printf("FAILED: %d check(s) across %zu tests\n", g_failures, tests.size());
  return 1;
}
