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
#include "../fake/FakeUdmCandidateProvider.h"

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

bool sameCoverageIssues(const std::vector<fr::CoverageIssue>& a,
                        const std::vector<fr::CoverageIssue>& b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].kind != b[i].kind || a[i].rowId != b[i].rowId
        || a[i].xLo != b[i].xLo || a[i].xHi != b[i].xHi
        || a[i].siteCount != b[i].siteCount
        || a[i].instances != b[i].instances) {
      return false;
    }
  }
  return true;
}

void testPreCheckMultiRowIssuesDeterministic()
{
  // Vt Type: 1 | Widths: {2,4} | cell type: 0=filler
  // Row 2 has [2,4) uncovered; row 5 has [4,8) uncovered.
  fr::FakeDesign design = makeLibrary();
  design.addRow(2, 0, 8)
      .place(200, fillerMaster(2, kVt1), 2, 0)
      .place(201, fillerMaster(4, kVt1), 2, 4)
      .addRow(5, 0, 8)
      .place(500, fillerMaster(4, kVt1), 5, 0);

  const auto first = fr::runUtilityPreCheck(design, fr::DebugLog(verbose()));
  const auto second = fr::runUtilityPreCheck(design, fr::DebugLog(verbose()));
  CHECK(!first.isFullUtility);
  CHECK(sameCoverageIssues(first.issues, second.issues));
  CHECK_EQ(first.issues.size(), 2u);
  if (first.issues.size() != 2) {
    return;
  }
  CHECK_EQ(first.issues[0].rowId, 2);
  CHECK(first.issues[0].kind == fr::CoverageIssueKind::Gap);
  CHECK(first.issues[0].xLo == 2 && first.issues[0].xHi == 4);
  CHECK_EQ(first.issues[1].rowId, 5);
  CHECK(first.issues[1].xLo == 4 && first.issues[1].xHi == 8);
}

void testPreCheckGapAtRowEdges()
{
  // Vt Type: 1 | Widths: {4} | cell type: 0=filler
  // Row 0: gap[0,2), filler[2,6), gap[6,8).
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 8).place(100, fillerMaster(4, kVt1), 0, 2);

  const auto coverage = fr::runUtilityPreCheck(design, fr::DebugLog(verbose()));
  CHECK_EQ(coverage.issues.size(), 2u);
  if (coverage.issues.size() != 2) {
    return;
  }
  CHECK(coverage.issues[0].kind == fr::CoverageIssueKind::Gap);
  CHECK_EQ(coverage.issues[0].xLo, 0);
  CHECK_EQ(coverage.issues[0].xHi, 2);
  CHECK_EQ(coverage.issues[0].siteCount, 2);
  CHECK(coverage.issues[1].kind == fr::CoverageIssueKind::Gap);
  CHECK_EQ(coverage.issues[1].xLo, 6);
  CHECK_EQ(coverage.issues[1].xHi, 8);
  CHECK_EQ(coverage.issues[1].siteCount, 2);
}

void testPreCheckOverlapThreeInstances()
{
  // Vt Type: {1,2,3} | Widths: {4} | cell type: 0=filler
  // Three fillers occupy the same [0,4) segment.
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 4)
      .place(102, fillerMaster(4, kVt1), 0, 0)
      .place(100, fillerMaster(4, kVt2), 0, 0)
      .place(101, fillerMaster(4, kVt3), 0, 0);

  const auto coverage = fr::runUtilityPreCheck(design, fr::DebugLog(verbose()));
  CHECK_EQ(coverage.issues.size(), 1u);
  if (coverage.issues.size() != 1) {
    return;
  }
  const auto& overlap = coverage.issues.front();
  CHECK(overlap.kind == fr::CoverageIssueKind::Overlap);
  CHECK_EQ(overlap.xLo, 0);
  CHECK_EQ(overlap.xHi, 4);
  CHECK(overlap.instances == (std::vector<fr::InstanceId>{100, 101, 102}));
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

// --- Fake-UDM candidate provider (adapter rehearsal) --------------------------
//
// Derivation must match the checker's buildMasters/parseLayerName path: VT is
// the FAMILY of the implant layers under the master's shapes (VTS=0 VTL=1
// VTH=2 VTUL=3), never the master name; width is DBU, site-aligned.

// Appendix-A library: widths and VTs for every master id, derived from the
// band shapes' layers. Master names are decoration.
void testUdmProviderDescribeWidthsAndVts()
{
  fr::FakeDesign design;  // only needed to satisfy the provider's view
  fr::FakeUdmCandidateProvider provider(design, /*siteWidth=*/1,
                                        /*rowHeight=*/2);
  provider.addAppendixALibrary();

  // Batch query in a fixed order; input order must be preserved.
  std::vector<fr::MasterId> ids;
  for (const int w : {2, 3, 4, 8}) {
    for (const int fam : {0, 1, 3}) {  // VTS, VTL, VTUL
      ids.push_back(w * 10 + fam);
    }
  }
  ids.push_back(999);  // unknown

  const auto described = provider.describeMasters(ids);
  CHECK_EQ(described.size(), 13u);
  for (size_t i = 0; i + 1 < described.size(); ++i) {
    const auto& d = described[i];ßµÞÚ$z{-®éÜj×zero, smaller}, 3), 5);
}

fr::FillerDomain makeDomain(const fr::FakeDesign& design,
                            fr::InstanceId id,
                            std::initializer_list<fr::MasterId> targets)
{
  fr::FillerDomain domain;
  domain.instanceId = id;
  for (fr::MasterId target : targets) {
    domain.options.push_back(*fr::makeSwap(design, id, target));
  }
  return domain;
}

void testEnumerateCompleteBudgetBoundary()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 4)
      .place(500, fillerMaster(2, kVt1), 0, 0)
      .place(501, fillerMaster(2, kVt1), 0, 2);
  const std::vector<fr::FillerDomain> domains = {
      makeDomain(design, 500, {fillerMaster(2, kVt2), fillerMaster(2, kVt3)}),
      makeDomain(design, 501, {fillerMaster(2, kVt2), fillerMaster(2, kVt3)}),
  };
  fr::RepairConfig config;
  const auto exactBudget =
      fr::enumerateOverlays(domains, config, 8, fr::DebugLog(verbose()));
  CHECK_EQ(exactBudget.overlays.size(), 8u);
  CHECK(exactBudget.complete);

  const auto complete =
      fr::enumerateOverlays(domains, config, 9, fr::DebugLog(verbose()));
  CHECK(complete.complete);
  CHECK_EQ(complete.overlays.size(), 8u);

  const auto truncated =
      fr::enumerateOverlays(domains, config, 7, fr::DebugLog(verbose()));
  CHECK(!truncated.complete);
  CHECK_EQ(truncated.overlays.size(), 7u);
}

void testEnumerateOverflowClamp()
{
  fr::FakeDesign design = makeLibrary();
  for (int i = 0; i < 40; ++i) {
    design.place(700 + i, fillerMaster(2, kVt1), 0, i * 2);
  }
  design.addRow(0, 0, 80);
  std::vector<fr::FillerDomain> domains;
  for (int i = 0; i < 40; ++i) {
    domains.push_back(makeDomain(
        design, 700 + i, {fillerMaster(2, kVt2), fillerMaster(2, kVt3)}));
  }
  fr::RepairConfig config;
  const auto plan =
      fr::enumerateOverlays(domains, config, 32, fr::DebugLog(verbose()));
  CHECK(!plan.complete);
  CHECK_EQ(plan.overlays.size(), 32u);
}

void testEnumerateSize3CapAndProducts()
{
  fr::FakeDesign design = makeLibrary();
  for (int i = 0; i < 5; ++i) {
    design.place(800 + i, fillerMaster(2, kVt1), 0, i * 2);
  }
  design.addRow(0, 0, 10);
  std::vector<fr::FillerDomain> domains;
  for (int i = 0; i < 5; ++i) {
    domains.push_back(makeDomain(
        design, 800 + i, {fillerMaster(2, kVt2), fillerMaster(2, kVt3)}));
  }
  fr::RepairConfig config;
  config.maxSubsetSize = 3;
  config.memberCapSize2 = 5;
  config.memberCapSize3 = 3;
  const auto plan =
      fr::enumerateOverlays(domains, config, 100, fr::DebugLog(verbose()));
  CHECK(!plan.complete);
  CHECK_EQ(plan.overlays.size(), 58u);  // size1:10, size2:40, size3 cap C(3,3)*8
  CHECK_EQ(plan.overlays.back().size(), 3u);
  CHECK_EQ(plan.overlays.back()[0].instanceId, 800);
  CHECK_EQ(plan.overlays.back()[1].instanceId, 801);
  CHECK_EQ(plan.overlays.back()[2].instanceId, 802);
  CHECK_EQ(plan.overlays.back()[2].newMasterId, fillerMaster(2, kVt3));
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

  // Deterministic solution: this layout has several oracle-clean single
  // swaps; the engine returns the FIRST in the pinned enumeration order.
  // Under the V2.1 #9 filler-domain order that is the row2 width-4 vt1
  // filler 2012 -> vt0 (the pre-#9 flat-swap order surfaced 3013 -> vt1,
  // an equally clean alternative). Oracle-verified: residual=0,
  // newInWindow=0, relatedInHalo=0.
  CHECK(result.hasSolution);
  CHECK_EQ(result.changes.size(), 1u);
  CHECK_EQ(result.changes[0].instanceId, 2012);
  CHECK_EQ(result.changes[0].newMasterId, grid::filler(4, 0));

  // Same input -> identical result (planner determinism).
  fr::FakeImplantChecker checker2(design, rules);
  fr::FillerRepairEngine engine2(design, checker2, provider, config);
  const auto result2 = engine2.repair(request);
  CHECK(result2.hasSolution);
  CHECK_EQ(result2.changes.size(), 1u);
  CHECK_EQ(result2.changes[0].instanceId, 2012);
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
      {"precheck_multi_row_issues_deterministic",
       testPreCheckMultiRowIssuesDeterministic},
      {"precheck_gap_at_row_edges", testPreCheckGapAtRowEdges},
      {"precheck_overlap_three_instances", testPreCheckOverlapThreeInstances},
      {"engine_fatal_on_gap_without_checker_calls",
       testEngineFatalOnGapWithoutCheckerCalls},
      {"candidate_provider", testCandidateProvider},
      {"udm_provider_describe_widths_and_vts", testUdmProviderDescribeWidthsAndVts},
      {"udm_provider_rejects_malformed_masters",
       testUdmProviderRejectsMalformedMasters},
      {"udm_provider_candidates_contract", testUdmProviderCandidatesContract},
      {"engine_solves_with_udm_provider", testEngineSolvesWithUdmProvider},
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
      {"window_L0_exact_membership", testWindowL0ExactMembership},
      {"window_bridge_conditions_each", testWindowBridgeConditionsEach},
      {"window_at_design_edges", testWindowAtDesignEdges},
      {"unfixable_ring_boundary", testUnfixableRingBoundary},
      {"window_adaptive_adds_k_on_blocking_side",
       testWindowAdaptiveAddsKOnBlockingSide},
      {"window_adaptive_coupled_rows_and_fixed_boundary",
       testWindowAdaptiveCoupledRowsAndFixedBoundary},
      {"guard_region_two_cell_ring", testGuardRegionTwoCellRing},
      {"engine_unfixable_fast_fail", testEngineUnfixableFastFail},
      {"engine_empty_snapshot_is_success", testEngineEmptySnapshotIsSuccess},
      {"swap_generator_basic", testSwapGeneratorBasic},
      {"swap_generator_no_usable_master", testSwapGeneratorNoUsableMaster},
      {"swapgen_rejected_candidate_diag", testSwapgenRejectedCandidateDiag},
      {"ranker_order", testRankerOrder},
      {"ranker_filler_key_isolated", testRankerFillerKeyIsolated},
      {"ranker_domain_order_isolated", testRankerDomainOrderIsolated},
      {"enumeration_order_and_completeness", testEnumerationOrderAndCompleteness},
      {"enumeration_filler_domain_not_crowded_out",
       testEnumerationFillerDomainNotCrowdedOut},
      {"engine_solves_single_swap", testEngineSolvesSingleSwap},
      {"engine_solves_pair_non_monotone", testEngineSolvesPairNonMonotone},
      {"engine_ignores_unrelated_halo_violation",
       testEngineIgnoresUnrelatedHaloViolation},
      {"engine_no_solution_definitive", testEngineNoSolutionDefinitive},
      {"gate_cache_single_evaluation", testGateCacheSingleEvaluation},
      {"engine_detects_protocol_error", testEngineDetectsProtocolError},
      {"engine_order_independent_batches", testEngineOrderIndependentBatches},
      {"engine_batch_size_invariance", testEngineBatchSizeInvariance},
      {"engine_determinism_full_transcript",
       testEngineDeterminismFullTranscript},
      {"engine_never_edits_guard_only", testEngineNeverEditsGuardOnly},
      {"gate_delta_classification_branches", testGateDeltaClassificationBranches},
      {"gate_rejects_unexplained_illegal", testGateRejectsUnexplainedIllegal},
      {"gate_baseline_mismatch_aborts_search", testGateBaselineMismatchAbortsSearch},
      {"gate_multiset_new_violation_not_absorbed",
       testGateMultisetNewViolationNotAbsorbed},
      {"gate_per_violation_rule_distance", testGatePerViolationRuleDistance},
      {"engine_definitive_reflects_last_window",
       testEngineDefinitiveReflectsLastWindow},
      {"engine_budget_ceiling", testEngineBudgetCeiling},
      {"engine_unfixable_hint_but_solved",
       testEngineUnfixableHintButSolved},
      {"engine_adaptive_l1_finds_far_filler",
       testEngineAdaptiveL1FindsFarFiller},
      {"engine_adaptive_cutoff_unchanged_blocking",
       testEngineAdaptiveCutoffUnchangedBlocking},
      {"gate_baseline_unexpected_inwindow_aborts",
       testGateBaselineUnexpectedInWindowAborts},
      {"gate_baseline_halo_extra_allowed", testGateBaselineHaloExtraAllowed},
      {"gate_baseline_outside_guard_original_skipped",
       testGateBaselineOutsideGuardOriginalSkipped},
      {"gate_residual_one_to_one", testGateResidualOneToOne},
      {"gate_batch_extra_result_rejected", testGateBatchExtraResultRejected},
      {"gate_single_wrong_echo_on_baseline", testGateSingleWrongEchoOnBaseline},
      {"gate_status_not_checked_carries_on", testGateStatusNotCheckedCarriesOn},
      {"gate_fatal_diag_makes_unusable", testGateFatalDiagMakesUnusable},
      {"gate_new_violation_no_rows_goes_halo", testGateNewViolationNoRowsGoesHalo},
      {"signature_field_mismatch_each", testSignatureFieldMismatchEach},
      {"signature_xwindow_tolerance_edges", testSignatureXwindowToleranceEdges},
      {"relatedness_row_and_distance_edges", testRelatednessRowAndDistanceEdges},
      {"rule_distance_fallback", testRuleDistanceFallback},
      {"enumerate_complete_budget_boundary", testEnumerateCompleteBudgetBoundary},
      {"enumerate_overflow_clamp", testEnumerateOverflowClamp},
      {"enumerate_size3_cap_and_products", testEnumerateSize3CapAndProducts},
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
