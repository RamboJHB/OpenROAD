// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Unit tests for the fillerRepair planner: Swap primitives, fake checker /
// fake candidate provider protocol, and the
// placement coverage scanner. Each legacy case is registered as an individual
// GoogleTest so it remains independently filterable and reportable.
//
// Run: test/run_tests.sh   (FR_VERBOSE=1 prints the [fr] debug transcript)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "../PlannerEngine.h"
#include "../Swap.h"
#include "../Signature.h"
#include "../OracleGate.h"
#include "../Ranker.h"
#include "../SubsetSearch.h"
#include "../Window.h"
#include "../fake/FakeDesign.h"
#include "../fake/FakeImplantChecker.h"
#include "../fake/FakeUdmCandidateProvider.h"

namespace fr = dpl2::fillerRepair;

// --- GoogleTest registration support ---------------------------------------

namespace {

struct Test
{
  const char* name;
  void (*fn)();
};

bool verbose();

class PlannerTest final : public ::testing::Test
{
 public:
  explicit PlannerTest(void (*fn)()) : fn_(fn) {}

 private:
  void TestBody() override
  {
    if (verbose()) {
      std::printf("\n===== case: %s =====\n", ::testing::UnitTest::GetInstance()
                                                   ->current_test_info()
                                                   ->name());
    }
    fn_();
  }

  void (*fn_)();
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
  EXPECT_TRUE(move.has_value());
  EXPECT_EQ(move->rowId, 0);
  EXPECT_TRUE(move->span == (fr::XInterval{4, 6}));
  EXPECT_EQ(move->oldVt, kVt1);
  EXPECT_EQ(move->newVt, kVt2);

  // Rejections, each with a reason.
  EXPECT_TRUE(!fr::makeSwap(f.design, 999, fillerMaster(2, kVt2), &error).has_value());
  EXPECT_TRUE(!fr::makeSwap(f.design, 101, fillerMaster(4, kVt2), &error).has_value());
  EXPECT_TRUE(!error.empty());  // size mismatch reason recorded
  EXPECT_TRUE(!fr::makeSwap(f.design, 101, fillerMaster(2, kVt1), &error).has_value());
  EXPECT_TRUE(!fr::makeSwap(f.design, 101, cellMaster(kVt2), &error).has_value());

  // Std cell is never a move target.
  f.design.remove(103).place(103, cellMaster(kVt2), 0, 10);
  EXPECT_TRUE(!fr::makeSwap(f.design, 103, fillerMaster(2, kVt1), &error).has_value());
}

void testCanonicalKeyOrderIndependent()
{
  RowFixture f = makeCoveredRow();
  auto m1 = *fr::makeSwap(f.design, 100, fillerMaster(4, kVt2));
  auto m2 = *fr::makeSwap(f.design, 101, fillerMaster(2, kVt2));

  EXPECT_TRUE(fr::canonicalKey({m1, m2}) == fr::canonicalKey({m2, m1}));
  EXPECT_TRUE(fr::canonicalKey({m1}) != fr::canonicalKey({m1, m2}));
  // Same instance, different target master => different overlay.
  auto m1b = *fr::makeSwap(f.design, 100, fillerMaster(4, kVt3));
  EXPECT_TRUE(fr::canonicalKey({m1}) != fr::canonicalKey({m1b}));
}

void testWireConversion()
{
  RowFixture f = makeCoveredRow();
  auto m1 = *fr::makeSwap(f.design, 101, fillerMaster(2, kVt2));
  auto m2 = *fr::makeSwap(f.design, 100, fillerMaster(4, kVt3));

  const auto changes = fr::toFillerChanges({m1, m2});
  EXPECT_EQ(changes.size(), 2u);
  // Deterministic order: sorted by instanceId.
  EXPECT_EQ(changes[0].instanceId, 100);
  EXPECT_EQ(changes[0].newMasterId, fillerMaster(4, kVt3));
  EXPECT_EQ(changes[1].instanceId, 101);
  EXPECT_EQ(changes[1].newMasterId, fillerMaster(2, kVt2));
}

// --- TODO 3: utility pre-check ----------------------------------------------

void testPreCheckFullUtility()
{
  RowFixture f = makeCoveredRow();
  const auto coverage = f.design.checkSiteCoverage(fr::DebugLog(verbose()));
  EXPECT_TRUE(coverage.isFullUtility);
  EXPECT_TRUE(coverage.issues.empty());
}

void testPreCheckGap()
{
  RowFixture f = makeCoveredRow();
  f.design.remove(101);  // hole [4,6)

  const auto coverage = f.design.checkSiteCoverage(fr::DebugLog(verbose()));
  EXPECT_TRUE(!coverage.isFullUtility);
  EXPECT_EQ(coverage.issues.size(), 1u);
  EXPECT_TRUE(coverage.issues[0].kind == fr::CoverageIssueKind::Gap);
  EXPECT_EQ(coverage.issues[0].rowId, 0);
  EXPECT_EQ(coverage.issues[0].xLo, 4);
  EXPECT_EQ(coverage.issues[0].xHi, 6);
  EXPECT_EQ(coverage.issues[0].siteCount, 2);
}

void testPreCheckOnlyGapOverlap()
{
  RowFixture f = makeCoveredRow();
  // Overlap: extra filler on top of [4,6).
  f.design.place(300, fillerMaster(2, kVt2), 0, 5);
  auto coverage = f.design.checkSiteCoverage(fr::DebugLog(verbose()));
  EXPECT_TRUE(!coverage.isFullUtility);
  bool sawOverlap = false;
  for (const auto& issue : coverage.issues) {
    sawOverlap |= issue.kind == fr::CoverageIssueKind::Overlap;
  }
  EXPECT_TRUE(sawOverlap);
  for (const auto& diagnostic : coverage.diagnostics) {
    EXPECT_TRUE(diagnostic.severity == fr::Severity::Warning);
    EXPECT_TRUE(diagnostic.code == "Gap" || diagnostic.code == "Overlap");
  }
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

  const auto first = design.checkSiteCoverage(fr::DebugLog(verbose()));
  const auto second = design.checkSiteCoverage(fr::DebugLog(verbose()));
  EXPECT_TRUE(!first.isFullUtility);
  EXPECT_TRUE(sameCoverageIssues(first.issues, second.issues));
  EXPECT_EQ(first.issues.size(), 2u);
  if (first.issues.size() != 2) {
    return;
  }
  EXPECT_EQ(first.issues[0].rowId, 2);
  EXPECT_TRUE(first.issues[0].kind == fr::CoverageIssueKind::Gap);
  EXPECT_TRUE(first.issues[0].xLo == 2 && first.issues[0].xHi == 4);
  EXPECT_EQ(first.issues[1].rowId, 5);
  EXPECT_TRUE(first.issues[1].xLo == 4 && first.issues[1].xHi == 8);
}

void testPreCheckGapAtRowEdges()
{
  // Vt Type: 1 | Widths: {4} | cell type: 0=filler
  // Row 0: gap[0,2), filler[2,6), gap[6,8).
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 8).place(100, fillerMaster(4, kVt1), 0, 2);

  const auto coverage = design.checkSiteCoverage(fr::DebugLog(verbose()));
  EXPECT_EQ(coverage.issues.size(), 2u);
  if (coverage.issues.size() != 2) {
    return;
  }
  EXPECT_TRUE(coverage.issues[0].kind == fr::CoverageIssueKind::Gap);
  EXPECT_EQ(coverage.issues[0].xLo, 0);
  EXPECT_EQ(coverage.issues[0].xHi, 2);
  EXPECT_EQ(coverage.issues[0].siteCount, 2);
  EXPECT_TRUE(coverage.issues[1].kind == fr::CoverageIssueKind::Gap);
  EXPECT_EQ(coverage.issues[1].xLo, 6);
  EXPECT_EQ(coverage.issues[1].xHi, 8);
  EXPECT_EQ(coverage.issues[1].siteCount, 2);
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

  const auto coverage = design.checkSiteCoverage(fr::DebugLog(verbose()));
  EXPECT_EQ(coverage.issues.size(), 1u);
  if (coverage.issues.size() != 1) {
    return;
  }
  const auto& overlap = coverage.issues.front();
  EXPECT_TRUE(overlap.kind == fr::CoverageIssueKind::Overlap);
  EXPECT_EQ(overlap.xLo, 0);
  EXPECT_EQ(overlap.xHi, 4);
  EXPECT_TRUE(overlap.instances == (std::vector<fr::InstanceId>{100, 101, 102}));
}

void testPlannerDoesNotRunPlacementPrecheck()
{
  RowFixture f = makeCoveredRow();
  f.design.remove(101);  // hole [4,6)

  fr::FakeImplantChecker checker(f.design, {});
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(f.design, checker, config);

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(f.design, f.anchor);
  const auto result = engine.repair(request);

  // The production facade's public precheck owns the placement gate. The
  // pure planner sees an empty implant snapshot and succeeds without ever
  // inspecting placement coverage.
  EXPECT_TRUE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_EQ(checker.requestCount(), 0);
}

// --- TODO 2: fake candidate provider ----------------------------------------

void testCandidateProvider()
{
  RowFixture f = makeCoveredRow();

  // Filler 101 is w2 vt1 -> exactly the two other w2 VTs, ascending order.
  const auto result = f.design.getUsableMasterCandidates({101});
  EXPECT_EQ(result.candidates.size(), 2u);
  EXPECT_EQ(result.candidates[0].masterId, fillerMaster(2, kVt2));
  EXPECT_EQ(result.candidates[1].masterId, fillerMaster(2, kVt3));

  // Std cell input: empty + diagnostic, not an error.
  f.design.remove(103).place(103, cellMaster(kVt1), 0, 10);
  const auto cellResult = f.design.getUsableMasterCandidates({103});
  EXPECT_TRUE(cellResult.candidates.empty());
  EXPECT_TRUE(!cellResult.diagnostics.empty());
}

// --- Fake-UDM candidate provider (production-boundary rehearsal) -------------
//
// Derivation must match the checker's buildMasters/parseLayerName path: VT is
// the FAMILY of the implant layers under the master's shapes (VTS=0 VTL=1
// VTH=2 VTUL=3), never the master name; width is DBU, site-aligned.

// Appendix-A library: widths and VTs for every master id, derived from the
// band shapes' layers. Master names are decoration.
void testUdmProviderDescribeWidthsAndVts()
{
  fr::FakeDesign design;  // only needed to satisfy the provider's view
  fr::FakeUdmCandidateProvider provider(/*siteWidth=*/1, /*rowHeight=*/2);
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
  EXPECT_EQ(described.size(), 13u);
  for (size_t i = 0; i + 1 < described.size(); ++i) {
    const auto& d = described[i];
    EXPECT_EQ(d.masterId, ids[i]);
    EXPECT_TRUE(d.usable);
    EXPECT_TRUE(d.isFiller);
    EXPECT_EQ(d.width, static_cast<fr::DbCoord>(ids[i] / 10));  // sites*1
    EXPECT_EQ(d.vt, static_cast<fr::VtId>(ids[i] % 10));        // family index
    EXPECT_EQ(d.heightRows, 1);
  }
  EXPECT_TRUE(!described.back().usable);
  EXPECT_TRUE(described.back().reason == "unknown_master");

  // Name is carried through but never drives the derivation.
  const auto* w8ul = provider.describeMaster(83);
  EXPECT_TRUE(w8ul != nullptr);
  EXPECT_TRUE(w8ul->name == "F_FILL8_63S6T9UL_1");
  EXPECT_EQ(w8ul->vt, 3);  // VTUL family, from the layers
}

// Malformed masters are described but unusable, with the checker-style
// reason codes from buildMasters.
void testUdmProviderRejectsMalformedMasters()
{
  fr::FakeDesign design;
  fr::FakeUdmCandidateProvider provider(/*siteWidth=*/2, /*rowHeight=*/2);
  provider.addLayer(1, "VTS_N");
  provider.addLayer(2, "VTS_P");
  provider.addLayer(3, "VTL_N");

  // Width 3 not aligned to siteWidth 2.
  provider.addMaster({901, "BAD_WIDTH", 3, 2, true,
                      {{0, 1, 0, 0, 3, 1}, {1, 2, 0, 1, 3, 2}}});
  // Shapes on two different families.
  provider.addMaster({902, "MIXED_FAMILY", 4, 2, true,
                      {{0, 1, 0, 0, 4, 1}, {1, 3, 0, 1, 4, 2}}});
  // Shape on a layer the catalog does not know.
  provider.addMaster({903, "UNKNOWN_LAYER", 4, 2, true,
                      {{0, 99, 0, 0, 4, 2}}});
  // Implant shape narrower than the master width.
  provider.addMaster({904, "PARTIAL_SPAN", 4, 2, true,
                      {{0, 1, 0, 0, 2, 2}}});
  // No shapes at all.
  provider.addMaster({905, "NO_SHAPES", 4, 2, true, {}});

  const auto d = provider.describeMasters({901, 902, 903, 904, 905});
  EXPECT_TRUE(!d[0].usable);
  EXPECT_TRUE(d[0].reason == "master_width_not_site_aligned");
  EXPECT_TRUE(!d[1].usable);
  EXPECT_TRUE(d[1].reason == "master_implant_family_mismatch");
  EXPECT_TRUE(!d[2].usable);
  EXPECT_TRUE(d[2].reason == "skipped_missing_rule_parameter");
  EXPECT_TRUE(!d[3].usable);
  EXPECT_TRUE(d[3].reason == "implant_shape_width_mismatch");
  EXPECT_TRUE(!d[4].usable);
  EXPECT_TRUE(d[4].reason == "no_implant_shape");
  // Width is still reported even when unusable (it comes from the master
  // input, not the derivation).
  EXPECT_EQ(d[1].width, 4);
  EXPECT_EQ(d[1].vt, fr::kUnknownVt);
}

// Candidate query contract (spec 5.3) on the appendix-A library, plus the
// design sync via registerInto.
void testUdmProviderCandidatesContract()
{
  fr::FakeDesign design;
  design.setSiteWidth(1);
  fr::FakeUdmCandidateProvider provider(1, /*rowHeight=*/2);
  provider.addAppendixALibrary();
  // A non-filler master in the same catalog (same size as w4 fillers).
  provider.addLayer(7, "VTH_N");
  provider.addLayer(8, "VTH_P");
  provider.addBandMaster(942, "CELL4_VTH", 4, /*isFiller=*/false, "VTH");

  provider.registerInto(design);
  design.addRow(0, 0, 8)
      .place(500, 40, 0, 0)    // filler w4 VTS
      .place(501, 942, 0, 4);  // std cell w4 VTH

  // Filler: exactly the two other w4 filler VTs, ascending id; the same-size
  // NON-filler master 942 must not appear.
  const auto result = design.getUsableMasterCandidates({500});
  EXPECT_EQ(result.candidates.size(), 2u);
  EXPECT_EQ(result.candidates[0].masterId, 41);  // w4 VTL
  EXPECT_EQ(result.candidates[1].masterId, 43);  // w4 VTUL

  // Std cell input: empty + warning, not an error.
  const auto cellResult = design.getUsableMasterCandidates({501});
  EXPECT_TRUE(cellResult.candidates.empty());
  EXPECT_TRUE(!cellResult.diagnostics.empty());

  // Unknown instance: error diagnostic.
  const auto unknown = design.getUsableMasterCandidates({777});
  EXPECT_TRUE(unknown.candidates.empty());
  EXPECT_TRUE(!unknown.diagnostics.empty());

  // registerInto synced width/height/vt into the PlacementView.
  const fr::MasterInfo* info = design.masterInfo(43);
  EXPECT_TRUE(info != nullptr);
  EXPECT_EQ(info->width, 4);
  EXPECT_EQ(info->vt, 3);
  EXPECT_TRUE(info->isFiller);
}

// E2E smoke: the engine solves a single-swap case with the appendix-A
// catalog driving both the PlacementView master table and the candidates.
//
// Vt Type: 0=VTS, 1=VTL, 3=VTUL  |  Widths: {2, 4}
// cell type: 1=std cell, 0=filler  |  Format: (vt type, width, cell type)
// Row 0: (1,4,1) (0,2,0) (1,4,0) (1,4,0) (1,2,0)
//   ids:  600*    601     602     603     604     (* = anchor std cell)
// With mwIntra=msIntra=5 the snapshot carries three violations: MS between
// the VTL runs [0,4) and [6,16) (gap 2), MW on the VTS run [4,6) (len 2),
// and MW on the VTL run [0,4) (len 4). The ONLY single swap clearing all
// three is 601 -> VTL (master 21): the whole row merges into one VTL run.
// (602 -> VTS would fix the first two but leaves the VTL[0,4) MW residual.)
void testEngineSolvesWithUdmProvider()
{
  fr::FakeDesign design;
  design.setSiteWidth(1);
  fr::FakeUdmCandidateProvider provider(1, /*rowHeight=*/2);
  provider.addAppendixALibrary();
  provider.addBandMaster(941, "CELL4_VTL", 4, /*isFiller=*/false, "VTL");
  provider.registerInto(design);

  design.addRow(0, 0, 16)
      .place(600, 941, 0, 0)   // anchor std cell, VTL
      .place(601, 20, 0, 4)    // filler w2 VTS -- the one to swap
      .place(602, 41, 0, 6)    // filler w4 VTL
      .place(603, 41, 0, 10)   // filler w4 VTL
      .place(604, 21, 0, 14);  // filler w2 VTL

  fr::FakeImplantRules rules;
  rules.mwIntra = 5;
  rules.msIntra = 5;

  fr::TargetPlace anchor;
  anchor.instanceId = 600;
  anchor.masterId = 941;
  anchor.rowId = 0;
  anchor.x = 0;

  // Snapshot from the checker, as production does.
  fr::FakeImplantChecker snapshotChecker(design, rules);
  fr::OverlayCheckRequest snapReq;
  snapReq.requestId = 0;
  snapReq.targetPlace = anchor;
  snapReq.guardRegion = fr::Region{fr::XInterval{0, 16}, 0, 0};
  fr::FillerRepairRequest request;
  request.targetPlace = anchor;
  request.violations =
      snapshotChecker.checkPlaceWithOverlay(snapReq).violations;
  EXPECT_EQ(request.violations.size(), 3u);

  fr::FakeImplantChecker checker(design, rules);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);

  const auto result = engine.repair(request);
  EXPECT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 1u);
  EXPECT_EQ(result.changes[0].instanceId, 601);
  EXPECT_EQ(result.changes[0].newMasterId, 21);  // w2 VTL
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
  EXPECT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].requestId, 7);
  EXPECT_EQ(results[1].requestId, 3);
  EXPECT_EQ(results[2].requestId, 5);
  EXPECT_EQ(checker.batchCount(), 1);
  EXPECT_EQ(checker.requestCount(), 3);
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
  EXPECT_TRUE(results[0].status == fr::CheckStatus::Checked);
  EXPECT_TRUE(results[1].status == fr::CheckStatus::InvalidOverlay);
  EXPECT_TRUE(!results[1].diagnostics.empty());  // status != Checked carries diags
  EXPECT_TRUE(results[2].status == fr::CheckStatus::Checked);
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
  EXPECT_TRUE(baseline.status == fr::CheckStatus::Checked);
  EXPECT_TRUE(!baseline.isLegal);
  EXPECT_EQ(baseline.violations.size(), 1u);
  EXPECT_TRUE(baseline.violations[0].kind == fr::ViolationKind::MinSpacing);
  EXPECT_TRUE(baseline.violations[0].relation == fr::ViolationRelation::IntraRow);
  EXPECT_TRUE(baseline.violations[0].xWindow == (fr::XInterval{10, 12}));

  // Overlay: recolor 103 to VT1 -> single VT1 run [0,16) -> clean.
  auto overlay = baselineRequest(f.design, 100, 2);
  overlay.fillerChanges = {{103, fillerMaster(2, kVt1)}};
  const auto fixed = checker.checkPlaceWithOverlay(overlay);
  EXPECT_TRUE(fixed.status == fr::CheckStatus::Checked);
  EXPECT_TRUE(fr::isRawCheckerSnapshotClean(fixed));
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
  EXPECT_TRUE(result.status == fr::CheckStatus::Checked);
  EXPECT_EQ(result.violations.size(), 1u);
  EXPECT_TRUE(result.violations[0].kind == fr::ViolationKind::MinWidth);
  EXPECT_TRUE(result.violations[0].relation == fr::ViolationRelation::InterRow);
  EXPECT_TRUE(result.violations[0].xWindow == (fr::XInterval{3, 4}));
  EXPECT_EQ(result.violations[0].rowIds.size(), 2u);

  // Inter-row MS: shrink row1's VT2 to [6,10) so the shapes become disjoint
  // with distance 2 < msInter 3.
  design.remove(201).place(201, fillerMaster(3, kVt1), 1, 3);
  fr::FakeImplantRules msRules;
  msRules.msInter = 3;
  fr::FakeImplantChecker msChecker(design, msRules);
  const auto msResult = msChecker.checkPlaceWithOverlay(baselineRequest(design, 100, 2));
  EXPECT_EQ(msResult.violations.size(), 1u);
  EXPECT_TRUE(msResult.violations[0].kind == fr::ViolationKind::MinSpacing);
  EXPECT_TRUE(msResult.violations[0].relation == fr::ViolationRelation::InterRow);
  EXPECT_EQ(msResult.violations[0].measuredValue, 2);
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
  EXPECT_TRUE(result.status == fr::CheckStatus::Checked);
  EXPECT_TRUE(result.violations.empty());
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
  EXPECT_TRUE(fr::isRawCheckerSnapshotClean(before));

  // Opto change: anchor becomes VT2 -> its shape [10,14) overlaps filler
  // 203's shape [9,11) by 1 < 2 -> inter-row MW violation with the target.
  auto changed = baselineRequest(design, 102, 2);
  changed.targetPlace.masterId = cellMaster(kVt2);
  const auto after = checker.checkPlaceWithOverlay(changed);
  EXPECT_TRUE(!after.isLegal);
  EXPECT_EQ(after.violations.size(), 1u);
  bool targetSeen = false;
  for (const auto& p : after.violations[0].participants) {
    targetSeen |= p.isTarget;
  }
  EXPECT_TRUE(targetSeen);

  // Repair direction (spec anchor-follow): recolor bridge filler 203 to VT1
  // -> row1 becomes one VT1 run, anchor's VT2 shape has no partner -> clean.
  auto repaired = changed;
  repaired.requestId = 3;
  repaired.fillerChanges = {{203, fillerMaster(2, kVt1)}};
  const auto fixed = checker.checkPlaceWithOverlay(repaired);
  EXPECT_TRUE(fr::isRawCheckerSnapshotClean(fixed));
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
  EXPECT_EQ(normalized.size(), 2u);

  EXPECT_TRUE(normalized[0].xRange == (fr::XInterval{10, 16}));
  EXPECT_EQ(normalized[0].fillerParticipants.size(), 1u);
  EXPECT_EQ(normalized[0].fillerParticipants[0], 104);
  EXPECT_EQ(normalized[0].cellAnchors.size(), 1u);  // target == participant 103
  EXPECT_EQ(normalized[0].cellAnchors[0], 103);
  EXPECT_TRUE(!normalized[0].rowIdFallback);

  EXPECT_TRUE(normalized[1].rowIdFallback);
  EXPECT_EQ(normalized[1].rowIds.size(), 1u);
  EXPECT_EQ(normalized[1].rowIds[0], 0);  // anchor row
}

void testSignatureMatching()
{
  const auto base = makeViolation(3, fr::ViolationKind::MinWidth,
                                  fr::ViolationRelation::InterRow, {0, 1},
                                  {10, 14});

  // Identical -> match; rows in different order -> still match.
  auto same = base;
  same.rowIds = {1, 0};
  EXPECT_TRUE(fr::sameSignature(base, same, 1));

  // Shifted by one site -> match (jitter tolerance).
  auto shifted = base;
  shifted.xWindow = {11, 15};
  EXPECT_TRUE(fr::sameSignature(base, shifted, 1));

  // Far away -> no match even with identical ids.
  auto far = base;
  far.xWindow = {30, 34};
  EXPECT_TRUE(!fr::sameSignature(base, far, 1));

  // Different rule / kind / relation / rows -> no match.
  auto rule = base;
  rule.ruleId = 4;
  EXPECT_TRUE(!fr::sameSignature(base, rule, 1));
  auto kind = base;
  kind.kind = fr::ViolationKind::MinSpacing;
  EXPECT_TRUE(!fr::sameSignature(base, kind, 1));
  auto rel = base;
  rel.relation = fr::ViolationRelation::IntraRow;
  EXPECT_TRUE(!fr::sameSignature(base, rel, 1));
  auto rows = base;
  rows.rowIds = {0};
  EXPECT_TRUE(!fr::sameSignature(base, rows, 1));

  // P/N band: same rule/kind/relation/rows/xWindow but different implant
  // layer -> distinct violations (spec 6.2, no dedup by position).
  fr::Violation pband = base;
  pband.primaryLayer = 10;  // e.g. P-band implant
  fr::Violation nband = base;
  nband.primaryLayer = 11;  // e.g. N-band implant at the same x gap
  EXPECT_TRUE(!fr::sameSignature(pband, nband, 1));
  EXPECT_TRUE(fr::sameSignature(pband, pband, 1));  // same layer still matches
  // secondaryLayer also participates (MS uses primary/secondary).
  fr::Violation sec = pband;
  sec.secondaryLayer = 12;
  EXPECT_TRUE(!fr::sameSignature(pband, sec, 1));
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
  EXPECT_TRUE(fr::isRelatedToOverlay(direct, overlay, 1));

  // Geometric proximity on the same row -> related.
  auto near = makeViolation(2, fr::ViolationKind::MinSpacing,
                            fr::ViolationRelation::IntraRow, {0}, {6, 7});
  EXPECT_TRUE(fr::isRelatedToOverlay(near, overlay, 1));

  // Same row but far in x -> unrelated.
  auto farX = makeViolation(2, fr::ViolationKind::MinSpacing,
                            fr::ViolationRelation::IntraRow, {0}, {12, 14});
  EXPECT_TRUE(!fr::isRelatedToOverlay(farX, overlay, 1));

  // Near in x but two rows away -> unrelated (rules couple adjacent rows).
  auto farRow = makeViolation(2, fr::ViolationKind::MinSpacing,
                              fr::ViolationRelation::IntraRow, {2}, {5, 6});
  EXPECT_TRUE(!fr::isRelatedToOverlay(farRow, overlay, 1));
}

// --- TODO 5: window builder + guard --------------------------------------

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
  EXPECT_TRUE(window.containsEditable(203));
  EXPECT_TRUE(window.containsEditable(101));
  EXPECT_TRUE(window.containsEditable(103));
  EXPECT_TRUE(window.containsEditable(204));
  EXPECT_TRUE(!window.containsEditable(100));  // [0,8) does not overlap [8,16)
  EXPECT_TRUE(!window.containsEditable(202));  // [6,9) touches 9 only
  // Bridge subset flagged.
  bool bridge203 = false;
  for (const auto id : window.bridgeFillers) {
    bridge203 |= id == 203;
  }
  EXPECT_TRUE(bridge203);
  EXPECT_EQ(window.rows.size(), 2u);
}

void testWindowL0ExactMembership()
{
  fr::FakeDesign design = makeTwoRowDesign();
  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  request.targetPlace.masterId = cellMaster(kVt2);
  auto violation = makeViolation(3,
                                 fr::ViolationKind::MinWidth,
                                 fr::ViolationRelation::InterRow,
                                 {0, 1},
                                 {10, 11});
  fr::ViolationParticipant participant;
  participant.instanceId = 203;
  participant.rowId = 1;
  participant.xRange = {9, 11};
  participant.isFiller = true;
  violation.participants = {participant};
  request.violations = {violation};

  const auto normalized =
      fr::normalizeViolations(request, design, fr::DebugLog(verbose()));
  const auto window = fr::buildWindow(0,
                                      request.targetPlace,
                                      normalized,
                                      design,
                                      1,
                                      fr::DebugLog(verbose()));

  EXPECT_TRUE(window.rows == (std::vector<fr::RowId>{0, 1}));
  EXPECT_TRUE(window.x == (fr::XInterval{8, 16}));
  EXPECT_TRUE(window.editableFillers
        == (std::vector<fr::InstanceId>{101, 103, 203, 204, 205}));
  EXPECT_TRUE(window.bridgeFillers
        == (std::vector<fr::InstanceId>{101, 103, 203, 204, 205}));
  EXPECT_TRUE(!window.containsEditable(100));
  EXPECT_TRUE(!window.containsEditable(202));
}

void testWindowBridgeConditionsEach()
{
  // Vt Type: {1,2} | Widths: {2,4} | cell type: 1=std, 0=filler
  // Three sparse sub-layouts isolate left-touch, right-touch, and adjacent-row
  // overlap with the widened anchor span.
  const auto makeWindow = [](fr::FakeDesign& design,
                             fr::InstanceId anchor,
                             fr::XInterval footprint) {
    fr::FillerRepairRequest request;
    request.targetPlace = anchorPlace(design, anchor);
    request.violations = {makeViolation(1,
                                        fr::ViolationKind::MinWidth,
                                        fr::ViolationRelation::IntraRow,
                                        {request.targetPlace.rowId},
                                        footprint)};
    const auto normalized = fr::normalizeViolations(
        request, design, fr::DebugLog(verbose()));
    return fr::buildWindow(0,
                           request.targetPlace,
                           normalized,
                           design,
                           1,
                           fr::DebugLog(verbose()));
  };

  fr::FakeDesign left = makeLibrary();
  left.addRow(0, 0, 8)
      .place(10, fillerMaster(2, kVt1), 0, 2)
      .place(11, cellMaster(kVt2), 0, 4);
  const auto leftWindow = makeWindow(left, 11, {4, 5});
  EXPECT_TRUE(leftWindow.containsEditable(10));

  fr::FakeDesign right = makeLibrary();
  right.addRow(0, 0, 8)
      .place(20, cellMaster(kVt2), 0, 0)
      .place(21, fillerMaster(2, kVt1), 0, 4);
  const auto rightWindow = makeWindow(right, 20, {0, 1});
  EXPECT_TRUE(rightWindow.containsEditable(21));

  fr::FakeDesign adjacent = makeLibrary();
  adjacent.addRow(0, 0, 10)
      .place(30, fillerMaster(2, kVt1), 0, 2)
      .place(31, fillerMaster(2, kVt1), 0, 0)
      .addRow(1, 0, 10)
      .place(32, cellMaster(kVt2), 1, 4);
  const auto adjacentWindow = makeWindow(adjacent, 32, {4, 5});
  EXPECT_TRUE(adjacentWindow.containsEditable(30));  // overlaps widened [3,9)
  EXPECT_TRUE(!adjacentWindow.containsEditable(31)); // only touches x=3
}

void testWindowAtDesignEdges()
{
  // Vt Type: {1,2} | Widths: {4} | cell type: 1=std, 0=filler
  // Anchors sit at bottom/left and top/right design boundaries.
  fr::FakeDesign bottom = makeLibrary();
  bottom.addRow(0, 0, 8)
      .place(100, cellMaster(kVt2), 0, 0)
      .place(101, fillerMaster(4, kVt1), 0, 4)
      .addRow(1, 0, 8)
      .place(200, fillerMaster(4, kVt1), 1, 0)
      .addRow(2, 0, 8)
      .place(300, fillerMaster(4, kVt1), 2, 0);
  fr::FillerRepairRequest bottomRequest;
  bottomRequest.targetPlace = anchorPlace(bottom, 100);
  bottomRequest.violations = {makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {0, 1})};
  auto normalized = fr::normalizeViolations(
      bottomRequest, bottom, fr::DebugLog(verbose()));
  const auto bottomWindow = fr::buildWindow(0,
                                            bottomRequest.targetPlace,
                                            normalized,
                                            bottom,
                                            1,
                                            fr::DebugLog(verbose()));
  EXPECT_EQ(bottomWindow.guardRegion.rowLo, 0);
  EXPECT_EQ(bottomWindow.guardRegion.rowHi, 2);
  EXPECT_TRUE(bottomWindow.guardRegion.x.xl >= 0);

  fr::FakeDesign top = makeLibrary();
  top.addRow(0, 0, 8)
      .place(400, fillerMaster(4, kVt1), 0, 4)
      .addRow(1, 0, 8)
      .place(500, fillerMaster(4, kVt1), 1, 4)
      .addRow(2, 0, 8)
      .place(600, fillerMaster(4, kVt1), 2, 0)
      .place(601, cellMaster(kVt2), 2, 4);
  fr::FillerRepairRequest topRequest;
  topRequest.targetPlace = anchorPlace(top, 601);
  topRequest.violations = {makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {2}, {7, 8})};
  normalized =
      fr::normalizeViolations(topRequest, top, fr::DebugLog(verbose()));
  const auto topWindow = fr::buildWindow(0,
                                         topRequest.targetPlace,
                                         normalized,
                                         top,
                                         1,
                                         fr::DebugLog(verbose()));
  EXPECT_EQ(topWindow.guardRegion.rowLo, 0);
  EXPECT_EQ(topWindow.guardRegion.rowHi, 2);
  EXPECT_TRUE(topWindow.guardRegion.x.xh <= 8);
}

void testWindowAdaptiveAddsKOnBlockingSide()
{
  fr::FakeDesign design = makeTwoRowDesign();
  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);

  auto v = makeViolation(3, fr::ViolationKind::MinWidth,
                         fr::ViolationRelation::InterRow, {0, 1}, {10, 11});
  request.violations = {v};
  const auto normalized =
      fr::normalizeViolations(request, design, fr::DebugLog(verbose()));

  const auto l0 = fr::buildWindow(0, request.targetPlace, normalized,
                                  design, 1, fr::DebugLog(verbose()));
  // Blocking lies closer to L0's left edge. One adaptive step grows left by
  // K=2 fillers per relevant row, not to the fixed/row boundary.
  const auto window = fr::expandWindowAdaptive(
      l0, request.targetPlace, {v}, design, 2, fr::DebugLog(verbose()));
  EXPECT_TRUE(window.x == (fr::XInterval{0, 16}));
  EXPECT_TRUE(window.containsEditable(100));  // row0: only one filler before boundary
  EXPECT_TRUE(window.containsEditable(201));  // row1: second filler added
  EXPECT_TRUE(window.containsEditable(202));  // row1: nearest filler added
  EXPECT_TRUE(!window.containsEditable(200));  // third filler: proves no whole-run sweep
}

void testWindowAdaptiveCoupledRowsAndFixedBoundary()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20)
      .place(100, fillerMaster(8, kVt1), 0, 0)
      .place(101, fillerMaster(4, kVt1), 0, 8)
      .place(102, fillerMaster(2, kVt1), 0, 12)
      .place(103, cellMaster(kVt1), 0, 14)  // fixed boundary
      .place(104, fillerMaster(2, kVt1), 0, 18)
      .addRow(1, 0, 20)
      .place(200, fillerMaster(8, kVt1), 1, 0)
      .place(201, cellMaster(kVt2), 1, 8)  // anchor
      .place(202, fillerMaster(2, kVt1), 1, 12)
      .place(203, fillerMaster(2, kVt1), 1, 14)
      .place(204, fillerMaster(4, kVt1), 1, 16)
      .addRow(2, 0, 20)
      .place(300, fillerMaster(8, kVt1), 2, 0)
      .place(301, fillerMaster(4, kVt1), 2, 8)
      .place(302, fillerMaster(2, kVt1), 2, 12)
      .place(303, fillerMaster(2, kVt1), 2, 14)
      .place(304, fillerMaster(4, kVt1), 2, 16);

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 201);
  const fr::Violation original = makeViolation(
      7,
      fr::ViolationKind::MinSpacing,
      fr::ViolationRelation::InterRow,
      {1},
      {11, 14});
  request.violations = {original};
  const auto normalized =
      fr::normalizeViolations(request, design, fr::DebugLog(verbose()));
  const fr::RepairWindow l0 = fr::buildWindow(
      0, request.targetPlace, normalized, design, 1, fr::DebugLog(verbose()));
  const fr::RepairWindow expanded = fr::expandWindowAdaptive(
      l0,
      request.targetPlace,
      {original},
      design,
      1,
      fr::DebugLog(verbose()));

  EXPECT_TRUE(expanded.containsEditable(203));  // anchor row, first right filler
  EXPECT_TRUE(expanded.containsEditable(303));  // coupled row +1
  EXPECT_TRUE(!expanded.containsEditable(304)); // K=1, no run sweep
  EXPECT_TRUE(!expanded.containsEditable(104)); // row -1 stopped at fixed cell 103
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
  EXPECT_EQ(window.guardRegion.rowLo, 0);
  EXPECT_EQ(window.guardRegion.rowHi, 1);
  EXPECT_TRUE(window.guardRegion.x.xl <= 3);   // two instances left of x=8 on row1
  EXPECT_TRUE(window.guardRegion.x.xh >= 16);  // right edge of both rows
  // Guard must always contain the window itself.
  EXPECT_TRUE(window.guardRegion.x.xl <= window.x.xl);
  EXPECT_TRUE(window.guardRegion.x.xh >= window.x.xh);
}

void testEngineNoEditableFillerZeroCalls()
{
  // A row of std cells only: no filler can enter any window. The engine
  // returns no solution with ZERO checker calls -- because the search finds
  // no editable filler, never via an early abort (V2.1 #6 dropped the
  // ring-based fast-fail; the hint itself was later removed as noise).
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 16)
      .place(100, cellMaster(kVt1), 0, 0)
      .place(101, cellMaster(kVt1), 0, 4)
      .place(102, cellMaster(kVt2), 0, 8)
      .place(103, cellMaster(kVt1), 0, 12);

  fr::FakeImplantChecker checker(design, {});
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  auto v = makeViolation(2, fr::ViolationKind::MinSpacing,
                         fr::ViolationRelation::IntraRow, {0}, {8, 9});
  request.violations = {v};

  const auto result = engine.repair(request);
  EXPECT_TRUE(!result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_EQ(checker.requestCount(), 0);  // no editable filler -> no calls
}

class ReentrantChecker : public fr::ImplantOverlayChecker
{
 public:
  fr::internal::PlannerEngine* engine = nullptr;
  const fr::FillerRepairRequest* request = nullptr;
  fr::Violation original;
  fr::FillerRepairResult inner;
  bool fired = false;

  fr::CheckResult checkPlaceWithOverlay(
      const fr::OverlayCheckRequest& r) override
  {
    if (!fired && engine != nullptr && request != nullptr) {
      fired = true;
      inner = engine->repair(*request);  // illegal callback (spec 3.3)
    }
    fr::CheckResult result;
    result.requestId = r.requestId;
    result.status = fr::CheckStatus::Checked;
    if (r.fillerChanges.empty()) {
      result.violations = {original};  // baseline reproduces the snapshot
    }
    result.isLegal = result.violations.empty();
    return result;
  }
  std::vector<fr::CheckResult> checkPlaceWithOverlays(
      const std::vector<fr::OverlayCheckRequest>& requests) override
  {
    std::vector<fr::CheckResult> results;
    for (const auto& r : requests) {
      results.push_back(checkPlaceWithOverlay(r));
    }
    return results;
  }
};

void testEngineReentrantRepairRefused()
{
  // Spec 3.3: the overlay API is a pure query; a checker calling back into
  // repair() on the same engine gets a fatal ReentrantRepair result, and
  // the outer repair completes normally.
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 12)
      .place(100, cellMaster(kVt1), 0, 0)
      .place(140, fillerMaster(4, kVt1), 0, 4)
      .place(103, cellMaster(kVt2), 0, 8);
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0},
      {4, 8});

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 103);
  request.violations = {original};

  ReentrantChecker checker;
  checker.original = original;
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);
  checker.engine = &engine;
  checker.request = &request;

  const auto result = engine.repair(request);
  EXPECT_TRUE(checker.fired);
  EXPECT_TRUE(!checker.inner.hasSolution);
  bool sawReentrant = false;
  for (const auto& diag : checker.inner.diagnostics) {
    sawReentrant |= diag.severity == fr::Severity::Fatal
                    && diag.code == "ReentrantRepair";
  }
  EXPECT_TRUE(sawReentrant);
  EXPECT_TRUE(result.hasSolution);  // the outer repair is unaffected
}

void testEngineEmptySnapshotIsSuccess()
{
  RowFixture f = makeCoveredRow();
  fr::FakeImplantChecker checker(f.design, {});
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(f.design, checker, config);

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(f.design, f.anchor);
  const auto result = engine.repair(request);
  EXPECT_TRUE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_EQ(checker.requestCount(), 0);
}


// --- TODO 6: swap generator ---------------------------------------------------

void testSwapGeneratorBasic()
{
  fr::FakeDesign design = makeTwoRowDesign();
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
  const auto generated = fr::generateSwaps(window, design,
                                               fr::DebugLog(verbose()));

  // Full library: every editable filler has exactly 2 same-size candidates.
  EXPECT_EQ(generated.swaps.size(), window.editableFillers.size() * 2);

  // Deterministic order: window editable order (row, x), then master id.
  // First editable filler is 101 (row0, x=8, w2 vt1) -> masters 22, 23.
  EXPECT_EQ(generated.swaps[0].instanceId, 101);
  EXPECT_EQ(generated.swaps[0].newMasterId, fillerMaster(2, kVt2));
  EXPECT_EQ(generated.swaps[1].instanceId, 101);
  EXPECT_EQ(generated.swaps[1].newMasterId, fillerMaster(2, kVt3));

  // Every swap targets an editable filler and never the current master.
  for (const auto& swap : generated.swaps) {
    EXPECT_TRUE(window.containsEditable(swap.instanceId));
    EXPECT_TRUE(swap.newMasterId != swap.oldMasterId);
    EXPECT_TRUE(swap.newVt != swap.oldVt);
  }
}

void testSwapGeneratorNoUsableMaster()
{
  // A width-5 filler exists in exactly one VT: no same-size replacement.
  fr::FakeDesign design = makeLibrary();
  design.addMaster(51, 5, 1, /*isFiller=*/true, kVt1);
  design.addRow(0, 0, 5).place(100, 51, 0, 0);
  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 5};
  window.editableFillers = {100};

  const auto generated = fr::generateSwaps(window, design,
                                               fr::DebugLog(verbose()));
  EXPECT_TRUE(generated.swaps.empty());
  bool sawNoUsable = false;
  for (const auto& diag : generated.diagnostics) {
    sawNoUsable |= diag.code == "NoUsableMaster";
  }
  EXPECT_TRUE(sawNoUsable);
}

class MixedValidityPlacementView : public fr::FakeDesign
{
 public:
  fr::MasterCandidateResult getUsableMasterCandidates(
      const fr::MasterCandidateRequest&) const override
  {
    fr::MasterCandidateResult result;
    result.candidates = {{fillerMaster(4, kVt2)},
                         {fillerMaster(2, kVt2)}};
    return result;
  }
};

void testSwapgenRejectedCandidateDiag()
{
  // Vt Type: 1 | Widths: {2} | cell type: 0=filler
  // Provider returns one valid width-2 and one invalid width-4 replacement.
  MixedValidityPlacementView design;
  design.setSiteWidth(1)
      .addMaster(fillerMaster(2, kVt1), 2, 1, true, kVt1)
      .addMaster(fillerMaster(2, kVt2), 2, 1, true, kVt2)
      .addMaster(fillerMaster(4, kVt2), 4, 1, true, kVt2);
  design.addRow(0, 0, 2).place(100, fillerMaster(2, kVt1), 0, 0);
  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 2};
  window.editableFillers = {100};

  const auto generated = fr::generateSwaps(
      window, design, fr::DebugLog(verbose()));
  EXPECT_EQ(generated.swaps.size(), 1u);
  EXPECT_EQ(generated.swaps.front().newMasterId, fillerMaster(2, kVt2));
  bool sawRejected = false;
  for (const auto& diagnostic : generated.diagnostics) {
    sawRejected |= diagnostic.severity == fr::Severity::Warning
                   && diagnostic.code == "RejectedCandidate"
                   && diagnostic.message.find("size mismatch")
                          != std::string::npos;
  }
  EXPECT_TRUE(sawRejected);
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
  const auto generated = fr::generateSwaps(window, sc.design,
                                           fr::DebugLog(verbose()));
  const auto ranked =
      fr::rankFillers(generated.swaps, sc.request.targetPlace, normalized,
                      window, sc.design, fr::DebugLog(verbose()));

  // V2.1 #9: the ranker returns filler DOMAINS. 203 is the only direct
  // participant -> its domain ranks first; within the domain the
  // neighbor-majority (VT1) target beats the demoted third VT (VT3).
  EXPECT_EQ(ranked[0].instanceId, 203);
  EXPECT_EQ(ranked[0].options.front().newVt, kVt1);
  // Third-VT options are demoted to the domain tail but never removed.
  EXPECT_EQ(ranked[0].options.back().newVt, kVt3);
  // Grouping preserves every generated swap and never truncates a domain.
  size_t optionTotal = 0;
  for (const auto& domain : ranked) {
    EXPECT_TRUE(!domain.options.empty());
    for (const auto& option : domain.options) {
      EXPECT_EQ(option.instanceId, domain.instanceId);
    }
    optionTotal += domain.options.size();
  }
  EXPECT_EQ(optionTotal, generated.swaps.size());
}

void testRankerFillerKeyIsolated()
{
  // Vt Type: 1 | Widths: {2,4,8} | cell type: 0=filler
  // Sparse rows isolate direct, bridge, width, x, and row tie-break keys.
  fr::FakeDesign design = makeLibrary();
  design.addRow(1, 0, 120)
      .place(700, fillerMaster(8, kVt1), 1, 100)  // direct
      .place(701, fillerMaster(8, kVt1), 1, 80)   // bridge
      .place(702, fillerMaster(2, kVt1), 1, 60)   // width
      .place(703, fillerMaster(4, kVt1), 1, 0)    // x
      .place(704, fillerMaster(4, kVt1), 1, 10)   // row tie: lower row
      .addRow(2, 0, 120)
      .place(705, fillerMaster(4, kVt1), 2, 10);

  std::vector<fr::Swap> swaps;
  for (const fr::InstanceId id : {705, 703, 701, 704, 700, 702}) {
    const fr::DbCoord width = design.masterInfo(design.instance(id)->masterId)->width;
    swaps.push_back(*fr::makeSwap(design, id, fillerMaster(width, kVt2)));
  }

  fr::NormalizedViolation direct;
  direct.fillerParticipants = {700};
  fr::RepairWindow window;
  window.bridgeFillers = {701};
  fr::TargetPlace anchor;
  anchor.masterId = cellMaster(kVt2);
  const auto ranked = fr::rankFillers(swaps,
                                      anchor,
                                      {direct},
                                      window,
                                      design,
                                      fr::DebugLog(verbose()));
  std::vector<fr::InstanceId> order;
  for (const auto& domain : ranked) {
    order.push_back(domain.instanceId);
  }
  EXPECT_TRUE(order == (std::vector<fr::InstanceId>{700, 701, 702, 703, 704, 705}));
}

void testRankerDomainOrderIsolated()
{
  // Vt Type: {1,2,3,4} | Widths: {2,4} | cell type: 1=std, 0=filler
  // VT4 filler has VT1 neighbors while the changed anchor master is VT2.
  fr::FakeDesign design = makeLibrary();
  design.addMaster(fillerMaster(2, 4), 2, 1, /*isFiller=*/true, 4);
  design.addRow(0, 0, 12)
      .place(800, cellMaster(kVt1), 0, 0)
      .place(801, fillerMaster(2, 4), 0, 4)
      .place(802, cellMaster(kVt1), 0, 6);

  const std::vector<fr::Swap> swaps = {
      *fr::makeSwap(design, 801, fillerMaster(2, kVt3)),
      *fr::makeSwap(design, 801, fillerMaster(2, kVt1)),
      *fr::makeSwap(design, 801, fillerMaster(2, kVt2)),
  };
  fr::TargetPlace anchor;
  anchor.masterId = cellMaster(kVt2);
  const auto ranked = fr::rankFillers(swaps,
                                      anchor,
                                      {},
                                      {},
                                      design,
                                      fr::DebugLog(verbose()));
  EXPECT_EQ(ranked.size(), 1u);
  if (ranked.size() != 1) {
    return;
  }
  EXPECT_EQ(ranked[0].options.size(), 3u);
  if (ranked[0].options.size() != 3) {
    return;
  }
  EXPECT_EQ(ranked[0].options[0].newVt, kVt2);  // anchor vote
  EXPECT_EQ(ranked[0].options[1].newVt, kVt1);  // neighbor majority
  EXPECT_EQ(ranked[0].options[2].newVt, kVt3);  // third VT, retained last
}

void testRankerMajorityPerBand()
{
  // Vt Type: {1,2,3,4} | Widths: {2,4} | cell type: 1=std, 0=filler
  // Spec 6.6: majority counts PER BAND SLOT, not per cell. A same-row
  // abutting neighbor faces the filler on BOTH half-row bands (2 band
  // votes); a row +-1 neighbor shares only the facing band pair (1 vote).
  // Per-cell counting ties kVt1/kVt2 at 2 cells each and picks kVt1;
  // per-band counting picks kVt2 (4 band votes vs 2).
  fr::FakeDesign design = makeLibrary();
  design.addMaster(fillerMaster(2, 4), 2, 1, /*isFiller=*/true, 4);
  design.addRow(0, 0, 12).addRow(1, 0, 12).addRow(2, 0, 12);
  design.place(900, cellMaster(kVt2), 1, 0)   // same row, abuts at x=4
      .place(901, fillerMaster(2, 4), 1, 4)   // the ranked filler, span [4,6)
      .place(902, cellMaster(kVt2), 1, 6)     // same row, abuts at x=6
      .place(903, cellMaster(kVt1), 0, 4)     // row below, overlaps the span
      .place(904, cellMaster(kVt1), 2, 4);    // row above, overlaps the span

  const std::vector<fr::Swap> swaps = {
      *fr::makeSwap(design, 901, fillerMaster(2, kVt1)),
      *fr::makeSwap(design, 901, fillerMaster(2, kVt2)),
      *fr::makeSwap(design, 901, fillerMaster(2, kVt3)),
  };
  fr::TargetPlace anchor;
  anchor.masterId = cellMaster(kVt3);
  const auto ranked = fr::rankFillers(swaps,
                                      anchor,
                                      {},
                                      {},
                                      design,
                                      fr::DebugLog(verbose()));
  EXPECT_EQ(ranked.size(), 1u);
  if (ranked.size() != 1 || ranked[0].options.size() != 3) {
    return;
  }
  EXPECT_EQ(ranked[0].options[0].newVt, kVt3);  // anchor vote
  EXPECT_EQ(ranked[0].options[1].newVt, kVt2);  // per-band majority
  EXPECT_EQ(ranked[0].options[2].newVt, kVt1);  // third VT, retained last
}

void testRankerMajoritySkipsMissingMaster()
{
  // Defensive: an instance whose master is unknown to the view must not
  // crash the majority vote. The zero-width ghost abuts the filler exactly
  // at its left edge (span [4,4) -> xh == filler.xl), which is the shape
  // that dereferenced a null MasterInfo before the guard.
  fr::FakeDesign design = makeLibrary();
  design.addMaster(fillerMaster(2, 4), 2, 1, /*isFiller=*/true, 4);
  design.addRow(0, 0, 12)
      .place(800, cellMaster(kVt1), 0, 0)
      .place(801, fillerMaster(2, 4), 0, 4)
      .place(666, /*masterId=*/9999, 0, 4)  // unknown master, zero width
      .place(802, cellMaster(kVt1), 0, 6);

  const std::vector<fr::Swap> swaps = {
      *fr::makeSwap(design, 801, fillerMaster(2, kVt1)),
      *fr::makeSwap(design, 801, fillerMaster(2, kVt2)),
  };
  fr::TargetPlace anchor;
  anchor.masterId = cellMaster(kVt2);
  const auto ranked = fr::rankFillers(swaps,
                                      anchor,
                                      {},
                                      {},
                                      design,
                                      fr::DebugLog(verbose()));
  EXPECT_EQ(ranked.size(), 1u);
  if (ranked.size() != 1 || ranked[0].options.size() != 2) {
    return;
  }
  EXPECT_EQ(ranked[0].options[0].newVt, kVt2);  // anchor vote
  EXPECT_EQ(ranked[0].options[1].newVt, kVt1);  // majority from real neighbors
}

void testCandidatesBandPolarityLayoutMustMatch()
{
  // Same size and a known different VT is not enough: a swap keeps position
  // and orientation, so a candidate whose R0-frame bottom band has the
  // opposite polarity would land every band on the wrong track -- the
  // provider must not offer it (the checker would reject the overlay).
  fr::FakeDesign design;
  design.setSiteWidth(1);
  design.addMaster(10, 2, 1, /*isFiller=*/true, kVt1, fr::BandPolarity::N)
      .addMaster(11, 2, 1, /*isFiller=*/true, kVt2, fr::BandPolarity::N)
      .addMaster(12, 2, 1, /*isFiller=*/true, kVt3, fr::BandPolarity::P);
  design.addRow(0, 0, 2).place(100, 10, 0, 0);

  const auto result = design.getUsableMasterCandidates({100});
  EXPECT_EQ(result.candidates.size(), 1u);
  if (!result.candidates.empty()) {
    EXPECT_EQ(result.candidates.front().masterId, 11);
  }
}

void testCandidatesPolarityOnlyFilterDiagnosed()
{
  // When every size/VT-compatible replacement is dropped ONLY by the
  // polarity-layout filter, the result must say so (broken polarity
  // metadata would otherwise hide behind a generic NoUsableMaster).
  fr::FakeDesign design;
  design.setSiteWidth(1);
  design.addMaster(10, 2, 1, /*isFiller=*/true, kVt1, fr::BandPolarity::N)
      .addMaster(12, 2, 1, /*isFiller=*/true, kVt3, fr::BandPolarity::P);
  design.addRow(0, 0, 2).place(100, 10, 0, 0);

  const auto result = design.getUsableMasterCandidates({100});
  EXPECT_TRUE(result.candidates.empty());
  bool sawPolarity = false;
  for (const auto& diag : result.diagnostics) {
    sawPolarity |= diag.code == "PolarityLayoutFiltered";
  }
  EXPECT_TRUE(sawPolarity);
}

void testFakeDesignCachesFollowMutation()
{
  // The reference-returning queries are served from caches; every mutator
  // must invalidate them (this locks the dirty-flag contract).
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 8).place(100, cellMaster(kVt1), 0, 0);
  EXPECT_EQ(design.instancesInRow(0).size(), 1u);
  design.place(101, fillerMaster(4, kVt1), 0, 4);
  EXPECT_EQ(design.instancesInRow(0).size(), 2u);
  design.remove(100);
  EXPECT_EQ(design.instancesInRow(0).size(), 1u);
  EXPECT_EQ(design.rows().size(), 1u);
  design.addRow(1, 0, 8);
  EXPECT_EQ(design.rows().size(), 2u);
}

void testFakeUdmBottomPolarityDerived()
{
  // The band anchor mirrors rebuildMasterShapes: the bottommost shape's
  // layer polarity is the master's R0-frame bottom band.
  fr::FakeUdmCandidateProvider provider(/*siteWidth=*/1, /*rowHeight=*/8);
  provider.addLayer(1, "VTL_N");
  provider.addLayer(2, "VTL_P");
  provider.addBandMaster(500, "FIL_N_BOTTOM", 2, /*isFiller=*/true, "VTL");

  fr::FakeUdmMaster flipped;  // same bands with P at the bottom
  flipped.masterId = 501;
  flipped.name = "FIL_P_BOTTOM";
  flipped.width = 2;
  flipped.height = 8;
  flipped.isFiller = true;
  flipped.shapes.push_back(fr::FakeUdmShape{0, 2, 0, 0, 2, 4});
  flipped.shapes.push_back(fr::FakeUdmShape{1, 1, 0, 4, 2, 8});
  provider.addMaster(flipped);

  const auto* nBottom = provider.describeMaster(500);
  const auto* pBottom = provider.describeMaster(501);
  EXPECT_TRUE(nBottom != nullptr && nBottom->usable);
  EXPECT_TRUE(pBottom != nullptr && pBottom->usable);
  if (nBottom == nullptr || pBottom == nullptr) {
    return;
  }
  EXPECT_TRUE(nBottom->bottomBandPolarity == fr::BandPolarity::N);
  EXPECT_TRUE(pBottom->bottomBandPolarity == fr::BandPolarity::P);

  fr::FakeDesign design;
  design.setSiteWidth(1);
  provider.registerInto(design);
  EXPECT_TRUE(design.masterInfo(500) != nullptr
        && design.masterInfo(500)->bottomBandPolarity == fr::BandPolarity::N);
  EXPECT_TRUE(design.masterInfo(501) != nullptr
        && design.masterInfo(501)->bottomBandPolarity == fr::BandPolarity::P);
}

void testEnumerationOrderAndCompleteness()
{
  RowFixture f = makeCoveredRow();
  // V2.1 #9: enumeration input is ranked filler domains.
  fr::FillerDomain d100;
  d100.instanceId = 100;
  d100.options = {*fr::makeSwap(f.design, 100, fillerMaster(4, kVt2)),
                  *fr::makeSwap(f.design, 100, fillerMaster(4, kVt3))};
  fr::FillerDomain d101;
  d101.instanceId = 101;
  d101.options = {*fr::makeSwap(f.design, 101, fillerMaster(2, kVt2)),
                  *fr::makeSwap(f.design, 101, fillerMaster(2, kVt3))};
  const std::vector<fr::FillerDomain> ranked = {d100, d101};

  fr::RepairConfig config;
  const auto plan =
      fr::enumerateOverlays(ranked, config, 512, fr::DebugLog(verbose()));
  // Space = (1+2)(1+2)-1 = 8: 4 singles + 4 cross-filler pairs.
  EXPECT_TRUE(plan.complete);
  EXPECT_EQ(plan.overlays.size(), 8u);
  // Size 1 walks fillers in rank order, each full domain in domain order.
  EXPECT_EQ(plan.overlays[0].size(), 1u);
  EXPECT_EQ(plan.overlays[0][0].instanceId, 100);
  EXPECT_EQ(plan.overlays[1][0].instanceId, 100);
  EXPECT_EQ(plan.overlays[1][0].newMasterId, fillerMaster(4, kVt3));
  EXPECT_EQ(plan.overlays[2][0].instanceId, 101);
  // First pair = both fillers' first choices (anchor-follow leads); the last
  // filler's option varies fastest across the Cartesian product.
  EXPECT_EQ(plan.overlays[4].size(), 2u);
  EXPECT_EQ(plan.overlays[4][0].instanceId, 100);
  EXPECT_EQ(plan.overlays[4][1].instanceId, 101);
  EXPECT_EQ(plan.overlays[4][1].newMasterId, fillerMaster(2, kVt2));
  EXPECT_EQ(plan.overlays[5][1].newMasterId, fillerMaster(2, kVt3));

  // Tiny budget truncates and clears the completeness claim.
  const auto truncated =
      fr::enumerateOverlays(ranked, config, 3, fr::DebugLog(verbose()));
  EXPECT_TRUE(!truncated.complete);
  EXPECT_EQ(truncated.overlays.size(), 3u);
}

// V2.1 #9 regression: member caps count FILLERS, not options. Three ranked
// domains, truncated mode, memberCapSize2=2: size-2 subsets draw from the
// first TWO fillers with their FULL domains. Under the old flat-swap-prefix
// semantics a cap of 2 covered only filler 100's two options, so no valid
// size-2 subset existed at all and fillers were crowded out by options.
void testEnumerationFillerDomainNotCrowdedOut()
{
  RowFixture f = makeCoveredRow();
  fr::FillerDomain d100;
  d100.instanceId = 100;
  d100.options = {*fr::makeSwap(f.design, 100, fillerMaster(4, kVt2)),
                  *fr::makeSwap(f.design, 100, fillerMaster(4, kVt3))};
  fr::FillerDomain d101;
  d101.instanceId = 101;
  d101.options = {*fr::makeSwap(f.design, 101, fillerMaster(2, kVt2)),
                  *fr::makeSwap(f.design, 101, fillerMaster(2, kVt3))};
  fr::FillerDomain d104;
  d104.instanceId = 104;
  d104.options = {*fr::makeSwap(f.design, 104, fillerMaster(4, kVt2)),
                  *fr::makeSwap(f.design, 104, fillerMaster(4, kVt3))};
  const std::vector<fr::FillerDomain> ranked = {d100, d101, d104};

  fr::RepairConfig config;
  config.memberCapSize2 = 2;
  config.memberCapSize3 = 2;  // size 3 needs 3 fillers -> none emitted
  // Space = 3*3*3-1 = 26 > budget 20 -> truncated mode, caps active.
  const auto plan =
      fr::enumerateOverlays(ranked, config, 20, fr::DebugLog(verbose()));
  EXPECT_TRUE(!plan.complete);

  // Size 1 is never capped: all three fillers' full domains appear --
  // including the last-ranked filler 104 and its second (demoted) option.
  int singles = 0;
  bool saw104Second = false;
  for (const auto& overlay : plan.overlays) {
    if (overlay.size() == 1) {
      ++singles;
      saw104Second |= overlay[0].instanceId == 104
                      && overlay[0].newMasterId == fillerMaster(4, kVt3);
    }
  }
  EXPECT_EQ(singles, 6);
  EXPECT_TRUE(saw104Second);

  // Size 2: exactly the (100,101) cross-filler products -- 4 of them, every
  // domain option reachable; filler 104 is excluded by the FILLER cap.
  int pairs = 0;
  for (const auto& overlay : plan.overlays) {
    if (overlay.size() == 2) {
      ++pairs;
      EXPECT_EQ(overlay[0].instanceId, 100);
      EXPECT_EQ(overlay[1].instanceId, 101);
    }
  }
  EXPECT_EQ(pairs, 4);
}

void testEngineSolvesSingleSwap()
{
  ScenarioA sc = makeScenarioA();
  fr::FakeImplantChecker checker(sc.design, sc.rules);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(sc.design, checker, config);

  const auto result = engine.repair(sc.request);
  EXPECT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 1u);
  EXPECT_EQ(result.changes[0].instanceId, 203);
  EXPECT_EQ(result.changes[0].newMasterId, fillerMaster(2, kVt1));
  // One baseline + at most one batch (clean overlay is the top-ranked
  // candidate; the final check is a cache hit, not a new request).
  EXPECT_TRUE(checker.requestCount() <= 1 + config.batchSize);

  // Determinism: same input -> identical outcome and identical call count.
  fr::FakeImplantChecker checker2(sc.design, sc.rules);
  fr::internal::PlannerEngine engine2(sc.design, checker2, config);
  const auto result2 = engine2.repair(sc.request);
  EXPECT_TRUE(result2.hasSolution);
  EXPECT_EQ(result2.changes.size(), result.changes.size());
  EXPECT_EQ(result2.changes[0].instanceId, result.changes[0].instanceId);
  EXPECT_EQ(result2.changes[0].newMasterId, result.changes[0].newMasterId);
  EXPECT_EQ(checker2.requestCount(), checker.requestCount());
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
  EXPECT_EQ(request.violations.size(), 2u);  // VT2 MS + VT1 MS

  fr::FakeImplantChecker checker(design, rules);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);

  const auto result = engine.repair(request);
  EXPECT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 2u);
  EXPECT_EQ(result.changes[0].instanceId, 110);
  EXPECT_EQ(result.changes[0].newMasterId, fillerMaster(2, kVt2));
  EXPECT_EQ(result.changes[1].instanceId, 111);
  EXPECT_EQ(result.changes[1].newMasterId, fillerMaster(2, kVt2));
  // All size-1 candidates were evaluated and rejected before the pair won:
  // 6 swaps (3 fillers x 2 usable VTs) + baseline at least.
  EXPECT_TRUE(checker.requestCount() >= 7);
}

// A protocol-honest oracle for complex search-order tests. Baseline and every
// partial candidate retain the original violation multiset; an overlay becomes
// clean only after it contains every required (instance, master) assignment.
class RequiredChangesChecker : public fr::ImplantOverlayChecker
{
 public:
  std::vector<fr::Violation> originals;
  std::vector<fr::FillerChange> required;

  fr::CheckResult checkPlaceWithOverlay(
      const fr::OverlayCheckRequest& request) override
  {
    ++request_count_;
    fr::CheckResult result;
    result.requestId = request.requestId;
    result.status = fr::CheckStatus::Checked;
    const bool solved = std::all_of(
        required.begin(),
        required.end(),
        [&](const fr::FillerChange& need) {
          return std::any_of(
              request.fillerChanges.begin(),
              request.fillerChanges.end(),
              [&](const fr::FillerChange& change) {
                return change.instanceId == need.instanceId
                       && change.newMasterId == need.newMasterId;
              });
        });
    if (!solved) {
      result.violations = originals;
    }
    result.isLegal = result.violations.empty();
    return result;
  }

  std::vector<fr::CheckResult> checkPlaceWithOverlays(
      const std::vector<fr::OverlayCheckRequest>& requests) override
  {
    ++batch_count_;
    std::vector<fr::CheckResult> results;
    results.reserve(requests.size());
    for (const fr::OverlayCheckRequest& request : requests) {
      results.push_back(checkPlaceWithOverlay(request));
    }
    return results;
  }

  int requestCount() const { return request_count_; }
  int batchCount() const { return batch_count_; }

 private:
  int request_count_ = 0;
  int batch_count_ = 0;
};

struct ComplexSearchFixture
{
  fr::FakeDesign design;
  fr::FillerRepairRequest request;
};

ComplexSearchFixture makeComplexSearchFixture()
{
  ComplexSearchFixture fixture;
  fixture.design = makeLibrary();

  // Vt Type: 1=VT1, 2=VT2 | Widths: {2,4}
  // cell type: 1=std cell, 0=filler | Format: (vt type,width,cell type)
  // Rows 0/2: (1,4,0)(1,2,0)(1,2,0)(1,4,1)(1,2,0)(1,2,0)(1,4,0)(1,4,0)
  // Row 1:    (1,4,0)(1,2,0)(1,2,0)(2,4,1)(1,2,0)(1,2,0)(1,4,0)(1,4,0)
  // The two required fillers are 102/202 at x=[6,8): both direct participants
  // and bridges into the anchor boundary. 101/201/104/204 are direct decoys,
  // while coupled-row fillers enlarge L0 further. All rows cover [0,24).
  for (fr::RowId row = 0; row < 3; ++row) {
    const fr::InstanceId base = 100 + row * 100;
    fixture.design.addRow(row, 0, 24)
        .place(base + 0, fillerMaster(4, kVt1), row, 0)
        .place(base + 1, fillerMaster(2, kVt1), row, 4)
        .place(base + 2, fillerMaster(2, kVt1), row, 6)
        .place(base + 3,
               cellMaster(row == 1 ? kVt2 : kVt1),
               row,
               8)
        .place(base + 4, fillerMaster(2, kVt1), row, 12)
        .place(base + 5, fillerMaster(2, kVt1), row, 14)
        .place(base + 6, fillerMaster(4, kVt1), row, 16)
        .place(base + 7, fillerMaster(4, kVt1), row, 20);
  }

  auto participant = [&](fr::InstanceId id) {
    const fr::PlacedInstance* instance = fixture.design.instance(id);
    fr::ViolationParticipant value;
    value.instanceId = id;
    value.masterId = instance->masterId;
    value.rowId = instance->rowId;
    value.xRange = fr::instanceSpan(fixture.design, *instance);
    value.isFiller = true;
    return value;
  };

  fr::Violation left = makeViolation(
      31,
      fr::ViolationKind::MinWidth,
      fr::ViolationRelation::InterRow,
      {0, 1},
      {4, 8});
  left.requiredValue = 2;
  left.participants = {
      participant(101), participant(201), participant(102), participant(202)};

  fr::Violation right = makeViolation(
      32,
      fr::ViolationKind::MinSpacing,
      fr::ViolationRelation::InterRow,
      {1, 2},
      {12, 16});
  right.requiredValue = 2;
  right.participants = {participant(104), participant(204)};

  fixture.request.targetPlace = anchorPlace(fixture.design, 203);
  fixture.request.violations = {left, right};
  return fixture;
}

void testEngineComplexRankedPairFast()
{
  ComplexSearchFixture fixture = makeComplexSearchFixture();
  RequiredChangesChecker checker;
  checker.originals = fixture.request.violations;
  checker.required = {{102, fillerMaster(2, kVt2)},
                      {202, fillerMaster(2, kVt2)}};
  fr::RepairConfig config;
  config.batchSize = 4;
  config.checkerCallBudgetPerWindow = 128;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(
      fixture.design, checker, config);

  const fr::FillerRepairResult result = engine.repair(fixture.request);
  EXPECT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 2u);
  if (result.changes.size() == 2) {
    EXPECT_EQ(result.changes[0].instanceId, 102);
    EXPECT_EQ(result.changes[0].newMasterId, fillerMaster(2, kVt2));
    EXPECT_EQ(result.changes[1].instanceId, 202);
    EXPECT_EQ(result.changes[1].newMasterId, fillerMaster(2, kVt2));
  }
  // The correct fillers are the two highest-ranked direct participants and
  // VT2 is the anchor-follow first option. Even with many decoys, the engine
  // reaches the first size-2 assignment without approaching the 128-call cap.
  EXPECT_TRUE(checker.requestCount() <= 21);
  EXPECT_TRUE(checker.batchCount() <= 6);

  // Reordering the incoming snapshot must not change success, solution, or
  // the deterministic checker-call transcript.
  std::reverse(fixture.request.violations.begin(),
               fixture.request.violations.end());
  RequiredChangesChecker checkerAgain;
  checkerAgain.originals = fixture.request.violations;
  checkerAgain.required = checker.required;
  fr::internal::PlannerEngine engineAgain(
      fixture.design, checkerAgain, config);
  const fr::FillerRepairResult again = engineAgain.repair(fixture.request);
  EXPECT_TRUE(again.hasSolution);
  EXPECT_EQ(again.changes.size(), result.changes.size());
  if (again.changes.size() == result.changes.size()) {
    for (size_t index = 0; index < result.changes.size(); ++index) {
      EXPECT_EQ(again.changes[index].instanceId,
               result.changes[index].instanceId);
      EXPECT_EQ(again.changes[index].newMasterId,
               result.changes[index].newMasterId);
    }
  }
  EXPECT_EQ(checkerAgain.requestCount(), checker.requestCount());
  EXPECT_EQ(checkerAgain.batchCount(), checker.batchCount());
}

void testEngineComplexThirdVtStillSucceeds()
{
  ComplexSearchFixture fixture = makeComplexSearchFixture();
  RequiredChangesChecker checker;
  checker.originals = fixture.request.violations;
  // Both fillers require the domain-tail third VT. This is deliberately a
  // harder solution than anchor-follow and proves demotion does not prune it.
  checker.required = {{102, fillerMaster(2, kVt3)},
                      {202, fillerMaster(2, kVt3)}};
  fr::RepairConfig config;
  config.batchSize = 4;
  config.checkerCallBudgetPerWindow = 128;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(
      fixture.design, checker, config);

  const fr::FillerRepairResult result = engine.repair(fixture.request);
  EXPECT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 2u);
  if (result.changes.size() == 2) {
    EXPECT_EQ(result.changes[0].instanceId, 102);
    EXPECT_EQ(result.changes[0].newMasterId, fillerMaster(2, kVt3));
    EXPECT_EQ(result.changes[1].instanceId, 202);
    EXPECT_EQ(result.changes[1].newMasterId, fillerMaster(2, kVt3));
  }
  EXPECT_TRUE(checker.requestCount() <= 21);
  EXPECT_TRUE(checker.batchCount() <= 7);
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
  EXPECT_EQ(request.violations.size(), 1u);  // only the anchor-caused MW

  fr::FakeImplantChecker checker(design, rules);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);

  const auto result = engine.repair(request);
  // The pre-existing VT3 MW sits in the baseline of the same guard region;
  // being unrelated to any changed filler it must not veto the fix.
  EXPECT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 1u);
  EXPECT_EQ(result.changes[0].instanceId, 203);
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
  EXPECT_TRUE(!request.violations.empty());

  fr::FakeImplantChecker checker(design, rules);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);

  const auto result = engine.repair(request);
  EXPECT_TRUE(!result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
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
  EXPECT_TRUE(sawNoClean);
  EXPECT_TRUE(sawDefinitive);
  EXPECT_TRUE(sawCutoff);
  EXPECT_TRUE(sawBest);
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
  EXPECT_TRUE(gate.runBaseline(window, budget));
  EXPECT_TRUE(gate.runBaseline(window, budget));  // cache hit
  (void) gate.search({o1}, window, window.guardRegion, budget);
  (void) gate.search({o1}, window, window.guardRegion, budget);  // cache hit
  EXPECT_EQ(checker.requestCount(), 2);  // baseline + o1, each exactly once
  EXPECT_TRUE(gate.cacheHits() >= 2);
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
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(sc.design, checker, config);

  const auto result = engine.repair(sc.request);
  EXPECT_TRUE(!result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  bool sawProtocol = false;
  for (const auto& diag : result.diagnostics) {
    sawProtocol |= diag.code == "CheckerProtocolError";
  }
  EXPECT_TRUE(sawProtocol);
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
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(sc.design, checker, config);

  const auto result = engine.repair(sc.request);
  EXPECT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 1u);
  EXPECT_EQ(result.changes[0].instanceId, 203);
  EXPECT_EQ(result.changes[0].newMasterId, fillerMaster(2, kVt1));
}

bool sameChanges(const std::vector<fr::FillerChange>& a,
                 const std::vector<fr::FillerChange>& b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].instanceId != b[i].instanceId
        || a[i].newMasterId != b[i].newMasterId) {
      return false;
    }
  }
  return true;
}

bool sameDiagnostics(const std::vector<fr::Diagnostic>& a,
                     const std::vector<fr::Diagnostic>& b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].severity != b[i].severity || a[i].code != b[i].code
        || a[i].message != b[i].message) {
      return false;
    }
  }
  return true;
}

class RecordingChecker : public fr::ImplantOverlayChecker
{
 public:
  explicit RecordingChecker(fr::ImplantOverlayChecker& inner) : inner_(inner) {}

  fr::CheckResult checkPlaceWithOverlay(
      const fr::OverlayCheckRequest& request) override
  {
    requests.push_back(request);
    return inner_.checkPlaceWithOverlay(request);
  }

  std::vector<fr::CheckResult> checkPlaceWithOverlays(
      const std::vector<fr::OverlayCheckRequest>& batch) override
  {
    requests.insert(requests.end(), batch.begin(), batch.end());
    return inner_.checkPlaceWithOverlays(batch);
  }

  std::vector<fr::OverlayCheckRequest> requests;

 private:
  fr::ImplantOverlayChecker& inner_;
};

void testEngineBatchSizeInvariance()
{
  ScenarioA sc = makeScenarioA();

  fr::FakeImplantChecker checkerOne(sc.design, sc.rules);
  fr::RepairConfig one;
  one.batchSize = 1;
  one.verbose = verbose();
  fr::internal::PlannerEngine engineOne(sc.design, checkerOne, one);
  const auto resultOne = engineOne.repair(sc.request);

  fr::FakeImplantChecker checkerMany(sc.design, sc.rules);
  fr::RepairConfig many;
  many.batchSize = 32;
  many.verbose = verbose();
  fr::internal::PlannerEngine engineMany(sc.design, checkerMany, many);
  const auto resultMany = engineMany.repair(sc.request);

  EXPECT_TRUE(resultOne.hasSolution == resultMany.hasSolution);
  EXPECT_TRUE(sameChanges(resultOne.changes, resultMany.changes));
}

void testEngineDeterminismFullTranscript()
{
  ScenarioA sc = makeScenarioA();
  fr::RepairConfig config;
  config.batchSize = 3;
  config.verbose = verbose();

  fr::FakeImplantChecker checkerA(sc.design, sc.rules);
  fr::internal::PlannerEngine engineA(sc.design, checkerA, config);
  const auto resultA = engineA.repair(sc.request);

  fr::FakeImplantChecker checkerB(sc.design, sc.rules);
  fr::internal::PlannerEngine engineB(sc.design, checkerB, config);
  const auto resultB = engineB.repair(sc.request);

  EXPECT_TRUE(resultA.hasSolution == resultB.hasSolution);
  EXPECT_TRUE(sameChanges(resultA.changes, resultB.changes));
  EXPECT_TRUE(sameDiagnostics(resultA.diagnostics, resultB.diagnostics));
  EXPECT_EQ(checkerA.requestCount(), checkerB.requestCount());
  EXPECT_EQ(checkerA.batchCount(), checkerB.batchCount());
}

void testEngineNeverEditsGuardOnly()
{
  ScenarioA sc = makeScenarioA();
  const auto normalized = fr::normalizeViolations(
      sc.request, sc.design, fr::DebugLog(verbose()));
  const auto l0 = fr::buildWindow(0,
                                  sc.request.targetPlace,
                                  normalized,
                                  sc.design,
                                  2,
                                  fr::DebugLog(verbose()));
  EXPECT_TRUE(!l0.containsEditable(100));
  EXPECT_TRUE(!l0.containsEditable(200));
  EXPECT_TRUE(l0.guardRegion.containsRow(sc.design.instance(100)->rowId));
  EXPECT_TRUE(l0.guardRegion.x.overlaps(fr::instanceSpan(sc.design, *sc.design.instance(100))));
  EXPECT_TRUE(l0.guardRegion.x.overlaps(fr::instanceSpan(sc.design, *sc.design.instance(200))));

  fr::FakeImplantChecker inner(sc.design, sc.rules);
  RecordingChecker checker(inner);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(sc.design, checker, config);
  const auto result = engine.repair(sc.request);

  EXPECT_TRUE(result.hasSolution);
  EXPECT_TRUE(!checker.requests.empty());
  for (const auto& request : checker.requests) {
    for (const auto& change : request.fillerChanges) {
      EXPECT_TRUE(l0.containsEditable(change.instanceId));
      EXPECT_TRUE(change.instanceId != 100);
      EXPECT_TRUE(change.instanceId != 200);
    }
  }
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

class ResultScriptedChecker : public fr::ImplantOverlayChecker
{
 public:
  std::map<std::string, fr::CheckResult> byKey;
  bool extraBatchResult = false;
  bool wrongSingleEcho = false;

  static fr::CheckResult checked(std::vector<fr::Violation> violations = {})
  {
    fr::CheckResult result;
    result.status = fr::CheckStatus::Checked;
    result.violations = std::move(violations);
    result.isLegal = result.violations.empty();
    return result;
  }

  fr::CheckResult checkPlaceWithOverlay(const fr::OverlayCheckRequest& request) override
  {
    const std::string key = ScriptedChecker::keyOf(request.fillerChanges);
    fr::CheckResult result;
    const auto it = byKey.find(key);
    if (it != byKey.end()) {
      result = it->second;
    } else {
      result = checked();
    }
    result.requestId = wrongSingleEcho ? request.requestId + 1000 : request.requestId;
    return result;
  }

  std::vector<fr::CheckResult> checkPlaceWithOverlays(
      const std::vector<fr::OverlayCheckRequest>& requests) override
  {
    std::vector<fr::CheckResult> results;
    for (const auto& request : requests) {
      results.push_back(checkPlaceWithOverlay(request));
    }
    if (extraBatchResult) {
      fr::CheckResult extra = checked();
      extra.requestId = 999999;
      results.push_back(extra);
    }
    return results;
  }
};

bool hasDiagCode(const std::vector<fr::Diagnostic>& diagnostics,
                 const std::string& code)
{
  for (const auto& diag : diagnostics) {
    if (diag.code == code) {
      return true;
    }
  }
  return false;
}

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
  EXPECT_TRUE(gate.runBaseline(window, budget));
  const auto sr = gate.search({o1, o2, o3}, window, window.guardRegion, budget);

  // o1/o2 rejected for the pinned reasons; o3 accepted despite the unrelated
  // halo violation (and despite isLegal=false in its raw result).
  EXPECT_TRUE(sr.foundClean);
  EXPECT_EQ(sr.cleanOverlay.size(), 1u);
  EXPECT_EQ(sr.cleanOverlay[0].instanceId, 500);
  EXPECT_EQ(sr.cleanOverlay[0].newMasterId, fillerMaster(2, kVt3));
  EXPECT_TRUE(sr.hasBest);
  EXPECT_EQ(sr.bestSummary.newInWindow, 1);  // o1 was the best-tracked reject
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
  EXPECT_TRUE(gate.runBaseline(window, budget));
  const fr::Overlay o = {*fr::makeSwap(design, 500, fillerMaster(2, kVt2))};
  const auto sr = gate.search({o}, window, window.guardRegion, budget);
  EXPECT_TRUE(!sr.foundClean);               // NOT accepted
  EXPECT_TRUE(sr.hasBest);
  EXPECT_TRUE(sr.bestSummary.inconsistent);  // rejected for self-inconsistency
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
  EXPECT_TRUE(!gate.runBaseline(window, budget));  // consistency gate fails
  bool sawMismatch = false;
  for (const auto& d : gate.diagnostics()) {
    sawMismatch |= d.code == "BaselineMismatch";
  }
  EXPECT_TRUE(sawMismatch);
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
  EXPECT_TRUE(gate.runBaseline(window, budget));  // H is out-of-window -> baseline consistent
  const auto sr = gate.search({o}, window, window.guardRegion, budget);
  EXPECT_TRUE(!sr.foundClean);                     // the second H is not absorbed
  EXPECT_EQ(sr.bestSummary.relatedInHalo, 1);
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
  EXPECT_TRUE(gate.runBaseline(window, budget));
  const auto sr = gate.search({o}, window, window.guardRegion, budget);
  EXPECT_TRUE(!sr.foundClean);
  EXPECT_EQ(sr.bestSummary.relatedInHalo, 1);
  EXPECT_EQ(sr.bestSummary.unrelatedInHalo, 0);
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

void testEngineBudgetCeiling()
{
  // Vt Type: {1,2} | Widths: {2,4} | cell type: 1=std, 0=filler
  // One editable filler has exactly two options: baseline + 2 == budget 3.
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 6)
      .place(100, cellMaster(kVt2), 0, 0)
      .place(140, fillerMaster(2, kVt1), 0, 4);
  fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {4, 6});
  fr::ViolationParticipant participant;
  participant.instanceId = 140;
  participant.masterId = fillerMaster(2, kVt1);
  participant.rowId = 0;
  participant.xRange = {4, 6};
  participant.isFiller = true;
  original.participants = {participant};

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 100);
  request.violations = {original};
  AlwaysUnsolvedChecker inner;
  inner.original = original;
  RecordingChecker checker(inner);
  fr::RepairConfig config;
  config.checkerCallBudgetPerWindow = 3;  // baseline + exact two-option space
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);

  const auto result = engine.repair(request);
  EXPECT_TRUE(!result.hasSolution);
  EXPECT_EQ(checker.requests.size(), 3u);
  bool sawDefinitive = false;
  for (const auto& diagnostic : result.diagnostics) {
    sawDefinitive |= diagnostic.code == "NoCleanOverlay"
                     && diagnostic.message.find("definitively")
                            != std::string::npos;
  }
  EXPECT_TRUE(sawDefinitive);
}

// Scripted oracle for adaptive-L1: every overlay remains blocked until it
// changes `solutionInstance`. This isolates window-growth control flow from
// the fake DRC model while preserving the real baseline-delta protocol.
class AdaptiveSolutionChecker : public fr::ImplantOverlayChecker
{
 public:
  fr::Violation original;
  fr::InstanceId solutionInstance = 0;

  fr::CheckResult checkPlaceWithOverlay(
      const fr::OverlayCheckRequest& request) override
  {
    fr::CheckResult result;
    result.requestId = request.requestId;
    result.status = fr::CheckStatus::Checked;
    const bool solved = std::any_of(
        request.fillerChanges.begin(),
        request.fillerChanges.end(),
        [&](const fr::FillerChange& change) {
          return change.instanceId == solutionInstance;
        });
    if (!solved) {
      result.violations = {original};
    }
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

void testEngineAdaptiveSolvesBeyondRing()
{
  // Vt Type: {1,2} | Widths: {2,4} | cell type: 1=std, 0=filler
  // The violation ring covers three std cells; L0 starts at filler 142 and
  // adaptive-L1 reaches the oracle-clean filler 141.
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 22)
      .place(100, cellMaster(kVt1), 0, 0)
      .place(101, cellMaster(kVt1), 0, 4)
      .place(102, cellMaster(kVt1), 0, 8)
      .place(140, fillerMaster(2, kVt1), 0, 12)
      .place(141, fillerMaster(2, kVt1), 0, 14)  // adaptive solution
      .place(142, fillerMaster(2, kVt1), 0, 16)  // L0 bridge
      .place(103, cellMaster(kVt2), 0, 18);      // anchor

  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {0, 4});
  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 103);
  request.violations = {original};

  AdaptiveSolutionChecker checker;
  checker.original = original;
  checker.solutionInstance = 141;
  fr::RepairConfig config;
  config.adaptiveStepFillers = 1;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);

  const auto result = engine.repair(request);
  EXPECT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 1u);
  if (!result.changes.empty()) {
    EXPECT_EQ(result.changes.front().instanceId, 141);
  }
  bool sawAdaptiveSolution = false;
  for (const auto& diagnostic : result.diagnostics) {
    sawAdaptiveSolution |= diagnostic.code == "Solution"
                           && diagnostic.message.find("adaptive-L1")
                                  != std::string::npos;
  }
  EXPECT_TRUE(sawAdaptiveSolution);
}

void testEngineAdaptiveL1FindsFarFiller()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20)
      .place(100, fillerMaster(4, kVt1), 0, 0)
      .place(101, fillerMaster(4, kVt1), 0, 4)
      .place(102, cellMaster(kVt2), 0, 8)       // anchor [8,12)
      .place(140, fillerMaster(2, kVt1), 0, 12) // L0 adjacent [12,14)
      .place(141, fillerMaster(2, kVt1), 0, 14) // adaptive solution
      .place(142, fillerMaster(4, kVt1), 0, 16);

  fr::Violation original = makeViolation(
      1,
      fr::ViolationKind::MinWidth,
      fr::ViolationRelation::IntraRow,
      {0},
      {11, 14});
  fr::ViolationParticipant participant;
  participant.instanceId = 140;
  participant.masterId = fillerMaster(2, kVt1);
  participant.rowId = 0;
  participant.xRange = {12, 14};
  participant.isFiller = true;
  original.participants = {participant};

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  request.violations = {original};

  AdaptiveSolutionChecker checker;
  checker.original = original;
  checker.solutionInstance = 141;
  fr::RepairConfig config;
  config.adaptiveStepFillers = 1;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);

  const fr::FillerRepairResult result = engine.repair(request);
  EXPECT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 1u);
  EXPECT_EQ(result.changes.front().instanceId, 141);
  bool sawAdaptiveSolution = false;
  for (const fr::Diagnostic& diagnostic : result.diagnostics) {
    sawAdaptiveSolution |= diagnostic.code == "Solution"
                           && diagnostic.message.find("adaptive-L1 step 1")
                                  != std::string::npos;
  }
  EXPECT_TRUE(sawAdaptiveSolution);
}

void testEngineAdaptiveCutoffUnchangedBlocking()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20)
      .place(100, fillerMaster(4, kVt1), 0, 0)
      .place(101, fillerMaster(4, kVt1), 0, 4)
      .place(102, cellMaster(kVt2), 0, 8)
      .place(140, fillerMaster(2, kVt1), 0, 12)
      .place(141, fillerMaster(2, kVt1), 0, 14)
      .place(142, fillerMaster(4, kVt1), 0, 16);

  fr::Violation original = makeViolation(
      1,
      fr::ViolationKind::MinWidth,
      fr::ViolationRelation::IntraRow,
      {0},
      {11, 14});
  fr::ViolationParticipant participant;
  participant.instanceId = 140;
  participant.rowId = 0;
  participant.xRange = {12, 14};
  participant.isFiller = true;
  original.participants = {participant};

  fr::FillerRepairRequest request;
  request.targetPlace = anchorPlace(design, 102);
  request.violations = {original};

  AlwaysUnsolvedChecker checker;
  checker.original = original;
  fr::RepairConfig config;
  config.adaptiveStepFillers = 1;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);

  const fr::FillerRepairResult result = engine.repair(request);
  EXPECT_TRUE(!result.hasSolution);
  bool sawUnchangedCutoff = false;
  for (const fr::Diagnostic& diagnostic : result.diagnostics) {
    sawUnchangedCutoff |= diagnostic.code == "ExpansionCutoff"
                          && diagnostic.message.find("unchanged blocking")
                                 != std::string::npos;
  }
  EXPECT_TRUE(sawUnchangedCutoff);
}

// V2.1 #10: "definitive no solution" must reflect the LAST searched window.
// A single contiguous filler run: L0 is just the anchor-touching participant
// filler (space 2, fully enumerated) while one configured adaptive step pulls
// in six more fillers (7 fillers, space 3^7 -> budget-truncated). The failure must read
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
  fr::RepairConfig config;
  config.verbose = verbose();
  // L0 (1 filler, space 2) fits; adaptive step (7 fillers) far exceeds 50.
  config.checkerCallBudgetPerWindow = 50;
  config.adaptiveStepFillers = 6;
  fr::internal::PlannerEngine engine(design, checker, config);

  const auto result = engine.repair(request);
  EXPECT_TRUE(!result.hasSolution);
  bool sawTruncated = false;
  bool sawDefinitive = false;
  for (const auto& d : result.diagnostics) {
    if (d.code == "NoCleanOverlay") {
      sawTruncated |= d.message.find("truncated") != std::string::npos;
      sawDefinitive |= d.message.find("definitively") != std::string::npos;
    }
  }
  EXPECT_TRUE(sawTruncated);
  EXPECT_TRUE(!sawDefinitive);
}

void testGateBaselineUnexpectedInWindowAborts()
{
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  const fr::Violation extra = makeViolation(
      2, fr::ViolationKind::MinSpacing, fr::ViolationRelation::IntraRow, {0}, {6, 7});

  ScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] = {original, extra};

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original};
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  EXPECT_TRUE(!gate.runBaseline(window, budget));
  EXPECT_TRUE(hasDiagCode(gate.diagnostics(), "BaselineMismatch"));
}

void testGateBaselineHaloExtraAllowed()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20).place(500, fillerMaster(2, kVt1), 0, 4);
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  const fr::Violation haloExtra = makeViolation(
      9, fr::ViolationKind::MinSpacing, fr::ViolationRelation::IntraRow, {0}, {16, 17});
  const fr::Overlay overlay = {*fr::makeSwap(design, 500, fillerMaster(2, kVt2))};

  ScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] = {original, haloExtra};
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(overlay))] = {};

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original};
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  EXPECT_TRUE(gate.runBaseline(window, budget));
  const auto sr = gate.search({overlay}, window, window.guardRegion, budget);
  EXPECT_TRUE(sr.foundClean);
  EXPECT_EQ(sr.cleanOverlay[0].instanceId, 500);
}

void testGateBaselineOutsideGuardOriginalSkipped()
{
  const fr::Violation inGuard = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  const fr::Violation outsideGuard = makeViolation(
      2, fr::ViolationKind::MinSpacing, fr::ViolationRelation::IntraRow, {0}, {50, 51});

  ScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] = {inGuard};

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {inGuard, outsideGuard};
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  EXPECT_TRUE(gate.runBaseline(window, budget));
  EXPECT_TRUE(!hasDiagCode(gate.diagnostics(), "BaselineMismatch"));
}

void testGateResidualOneToOne()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20).place(500, fillerMaster(2, kVt1), 0, 4);
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  const fr::Overlay overlay = {*fr::makeSwap(design, 500, fillerMaster(2, kVt2))};

  ScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] = {original, original};
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(overlay))] = {original};

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original, original};
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  EXPECT_TRUE(gate.runBaseline(window, budget));
  const auto sr = gate.search({overlay}, window, window.guardRegion, budget);
  EXPECT_TRUE(!sr.foundClean);
  EXPECT_TRUE(sr.hasBest);
  EXPECT_EQ(sr.bestSummary.residualOriginals, 1);
}

void testGateBatchExtraResultRejected()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20).place(500, fillerMaster(2, kVt1), 0, 4);
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  const fr::Overlay overlay = {*fr::makeSwap(design, 500, fillerMaster(2, kVt2))};

  ResultScriptedChecker checker;
  checker.extraBatchResult = true;
  checker.byKey[ScriptedChecker::keyOf({})] =
      ResultScriptedChecker::checked({original});
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(overlay))] =
      ResultScriptedChecker::checked();

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original};
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  EXPECT_TRUE(gate.runBaseline(window, budget));
  const auto sr = gate.search({overlay}, window, window.guardRegion, budget);
  EXPECT_TRUE(sr.protocolError);
  EXPECT_TRUE(hasDiagCode(gate.diagnostics(), "CheckerProtocolError"));
}

void testGateSingleWrongEchoOnBaseline()
{
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  ResultScriptedChecker checker;
  checker.wrongSingleEcho = true;
  checker.byKey[ScriptedChecker::keyOf({})] =
      ResultScriptedChecker::checked({original});

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original};
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  EXPECT_TRUE(!gate.runBaseline(window, budget));
  EXPECT_TRUE(hasDiagCode(gate.diagnostics(), "CheckerProtocolError"));
}

void testGateStatusNotCheckedCarriesOn()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20)
      .place(500, fillerMaster(2, kVt1), 0, 4)
      .place(501, fillerMaster(2, kVt1), 0, 8);
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  const fr::Overlay bad = {*fr::makeSwap(design, 500, fillerMaster(2, kVt2))};
  const fr::Overlay clean = {*fr::makeSwap(design, 501, fillerMaster(2, kVt2))};

  fr::CheckResult invalid;
  invalid.status = fr::CheckStatus::InvalidOverlay;
  invalid.isLegal = false;

  ResultScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] =
      ResultScriptedChecker::checked({original});
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(bad))] = invalid;
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(clean))] =
      ResultScriptedChecker::checked();

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 12};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  fr::RepairConfig config;
  config.batchSize = 2;
  fr::DebugLog log(verbose());
  fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original};
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  EXPECT_TRUE(gate.runBaseline(window, budget));
  const auto sr = gate.search({bad, clean}, window, window.guardRegion, budget);
  EXPECT_TRUE(sr.foundClean);
  EXPECT_EQ(sr.cleanOverlay[0].instanceId, 501);
}

void testGateFatalDiagMakesUnusable()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20).place(500, fillerMaster(2, kVt1), 0, 4);
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  const fr::Overlay overlay = {*fr::makeSwap(design, 500, fillerMaster(2, kVt2))};

  fr::CheckResult fatal = ResultScriptedChecker::checked();
  fatal.diagnostics.push_back(
      fr::makeDiag(fr::Severity::Fatal, "CheckerFatal", "scripted fatal"));

  ResultScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] =
      ResultScriptedChecker::checked({original});
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(overlay))] = fatal;

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original};
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  EXPECT_TRUE(gate.runBaseline(window, budget));
  const auto sr = gate.search({overlay}, window, window.guardRegion, budget);
  EXPECT_TRUE(!sr.foundClean);
  EXPECT_TRUE(sr.hasBest);
  EXPECT_TRUE(!sr.bestSummary.usable);
}

void testGateNewViolationNoRowsGoesHalo()
{
  fr::FakeDesign design = makeLibrary();
  design.addRow(0, 0, 20).place(500, fillerMaster(2, kVt1), 0, 4);
  const fr::Violation original = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {2, 4});
  fr::Violation noRows = makeViolation(
      9, fr::ViolationKind::MinSpacing, fr::ViolationRelation::IntraRow, {}, {6, 7});
  const fr::Overlay overlay = {*fr::makeSwap(design, 500, fillerMaster(2, kVt2))};

  ScriptedChecker checker;
  checker.byKey[ScriptedChecker::keyOf({})] = {original};
  checker.byKey[ScriptedChecker::keyOf(fr::toFillerChanges(overlay))] = {noRows};

  fr::RepairWindow window;
  window.rows = {0};
  window.x = {0, 10};
  window.guardRegion = fr::Region{{0, 20}, 0, 0};
  fr::RepairConfig config;
  fr::DebugLog log(verbose());
  fr::TargetPlace anchor{};
  const std::vector<fr::Violation> originals = {original};
  fr::OracleGate gate(checker, anchor, originals, 1, 2, config, log);

  int budget = 100;
  EXPECT_TRUE(gate.runBaseline(window, budget));
  const auto sr = gate.search({overlay}, window, window.guardRegion, budget);
  EXPECT_TRUE(!sr.foundClean);
  EXPECT_TRUE(sr.hasBest);
  EXPECT_EQ(sr.bestSummary.newInWindow, 0);
  EXPECT_EQ(sr.bestSummary.relatedInHalo, 1);
}

void testSignatureFieldMismatchEach()
{
  const auto base = makeViolation(3, fr::ViolationKind::MinWidth,
                                  fr::ViolationRelation::InterRow, {0, 1},
                                  {10, 14});
  auto changed = base;
  changed.ruleId = 4;
  EXPECT_TRUE(!fr::sameSignature(base, changed, 1));
  changed = base;
  changed.kind = fr::ViolationKind::MinSpacing;
  EXPECT_TRUE(!fr::sameSignature(base, changed, 1));
  changed = base;
  changed.relation = fr::ViolationRelation::IntraRow;
  EXPECT_TRUE(!fr::sameSignature(base, changed, 1));
  changed = base;
  changed.primaryLayer = 7;
  EXPECT_TRUE(!fr::sameSignature(base, changed, 1));
  changed = base;
  changed.secondaryLayer = 8;
  EXPECT_TRUE(!fr::sameSignature(base, changed, 1));
  changed = base;
  changed.rowIds = {0};
  EXPECT_TRUE(!fr::sameSignature(base, changed, 1));
}

void testSignatureXwindowToleranceEdges()
{
  const auto base = makeViolation(3, fr::ViolationKind::MinWidth,
                                  fr::ViolationRelation::InterRow, {0, 1},
                                  {10, 14});
  auto halfOverlap = base;
  halfOverlap.xWindow = {12, 16};  // overlap 2, shorter 4 -> match
  EXPECT_TRUE(fr::sameSignature(base, halfOverlap, 1));

  auto zeroLengthNear = base;
  zeroLengthNear.xWindow = {15, 15};
  EXPECT_TRUE(fr::sameSignature(base, zeroLengthNear, 1));

  auto exactlyOneSiteAway = base;
  exactlyOneSiteAway.xWindow = {15, 17};
  EXPECT_TRUE(fr::sameSignature(base, exactlyOneSiteAway, 1));

  auto justPastTolerance = base;
  justPastTolerance.xWindow = {16, 18};
  EXPECT_TRUE(!fr::sameSignature(base, justPastTolerance, 1));
}

void testRelatednessRowAndDistanceEdges()
{
  RowFixture f = makeCoveredRow();
  const auto swap = *fr::makeSwap(f.design, 101, fillerMaster(2, kVt2));
  const fr::Overlay overlay = {swap};  // span [4,6) row 0

  auto rowPlusOne = makeViolation(2, fr::ViolationKind::MinSpacing,
                                  fr::ViolationRelation::InterRow, {1}, {7, 8});
  EXPECT_TRUE(fr::isRelatedToOverlay(rowPlusOne, overlay, 1));

  auto rowPlusTwo = rowPlusOne;
  rowPlusTwo.rowIds = {2};
  EXPECT_TRUE(!fr::isRelatedToOverlay(rowPlusTwo, overlay, 1));

  auto exactDistance = rowPlusOne;
  exactDistance.rowIds = {0};
  exactDistance.xWindow = {7, 8};  // distance 1 from [4,6)
  EXPECT_TRUE(fr::isRelatedToOverlay(exactDistance, overlay, 1));

  auto pastDistance = exactDistance;
  pastDistance.xWindow = {8, 9};  // distance 2
  EXPECT_TRUE(!fr::isRelatedToOverlay(pastDistance, overlay, 1));
}

void testRuleDistanceFallback()
{
  fr::Violation zero = makeViolation(
      1, fr::ViolationKind::MinWidth, fr::ViolationRelation::IntraRow, {0}, {0, 1});
  zero.requiredValue = 0;
  fr::Violation smaller = zero;
  smaller.requiredValue = 2;

  EXPECT_EQ(fr::estimateRuleDistance({}, 3), 3);
  EXPECT_EQ(fr::estimateRuleDistance({zero}, 3), 3);
  EXPECT_EQ(fr::estimateRuleDistance({zero, smaller}, 3), 3);
  smaller.requiredValue = 5;
  EXPECT_EQ(fr::estimateRuleDistance({zero, smaller}, 3), 5);
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
  EXPECT_EQ(exactBudget.overlays.size(), 8u);
  EXPECT_TRUE(exactBudget.complete);

  const auto complete =
      fr::enumerateOverlays(domains, config, 9, fr::DebugLog(verbose()));
  EXPECT_TRUE(complete.complete);
  EXPECT_EQ(complete.overlays.size(), 8u);

  const auto truncated =
      fr::enumerateOverlays(domains, config, 7, fr::DebugLog(verbose()));
  EXPECT_TRUE(!truncated.complete);
  EXPECT_EQ(truncated.overlays.size(), 7u);
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
  EXPECT_TRUE(!plan.complete);
  EXPECT_EQ(plan.overlays.size(), 32u);
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
  EXPECT_TRUE(!plan.complete);
  EXPECT_EQ(plan.overlays.size(), 58u);  // size1:10, size2:40, size3 cap C(3,3)*8
  EXPECT_EQ(plan.overlays.back().size(), 3u);
  EXPECT_EQ(plan.overlays.back()[0].instanceId, 800);
  EXPECT_EQ(plan.overlays.back()[1].instanceId, 801);
  EXPECT_EQ(plan.overlays.back()[2].instanceId, 802);
  EXPECT_EQ(plan.overlays.back()[2].newMasterId, fillerMaster(2, kVt3));
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
  EXPECT_EQ(snapshot.size(), 2u);  // two corner-touch inter-row MS at [49,50)

  fr::FakeImplantChecker checker(design, rules);
  fr::RepairConfig config;
  config.verbose = verbose();
  fr::internal::PlannerEngine engine(design, checker, config);

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
  EXPECT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 1u);
  EXPECT_EQ(result.changes[0].instanceId, 2012);
  EXPECT_EQ(result.changes[0].newMasterId, grid::filler(4, 0));

  // Same input -> identical result (planner determinism).
  fr::FakeImplantChecker checker2(design, rules);
  fr::internal::PlannerEngine engine2(design, checker2, config);
  const auto result2 = engine2.repair(request);
  EXPECT_TRUE(result2.hasSolution);
  EXPECT_EQ(result2.changes.size(), 1u);
  EXPECT_EQ(result2.changes[0].instanceId, 2012);
}

void registerPlannerTests()
{
  const std::vector<Test> tests = {
      {"swap_construction", testSwapConstruction},
      {"canonical_key_order_independent", testCanonicalKeyOrderIndependent},
      {"wire_conversion", testWireConversion},
      {"precheck_full_utility", testPreCheckFullUtility},
      {"precheck_gap", testPreCheckGap},
      {"precheck_only_gap_overlap", testPreCheckOnlyGapOverlap},
      {"precheck_multi_row_issues_deterministic",
       testPreCheckMultiRowIssuesDeterministic},
      {"precheck_gap_at_row_edges", testPreCheckGapAtRowEdges},
      {"precheck_overlap_three_instances", testPreCheckOverlapThreeInstances},
      {"planner_does_not_run_placement_precheck",
       testPlannerDoesNotRunPlacementPrecheck},
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
      {"window_adaptive_adds_k_on_blocking_side",
       testWindowAdaptiveAddsKOnBlockingSide},
      {"window_adaptive_coupled_rows_and_fixed_boundary",
       testWindowAdaptiveCoupledRowsAndFixedBoundary},
      {"guard_region_two_cell_ring", testGuardRegionTwoCellRing},
      {"engine_no_editable_filler_zero_calls", testEngineNoEditableFillerZeroCalls},
      {"engine_reentrant_repair_refused", testEngineReentrantRepairRefused},
      {"engine_empty_snapshot_is_success", testEngineEmptySnapshotIsSuccess},
      {"swap_generator_basic", testSwapGeneratorBasic},
      {"swap_generator_no_usable_master", testSwapGeneratorNoUsableMaster},
      {"swapgen_rejected_candidate_diag", testSwapgenRejectedCandidateDiag},
      {"ranker_order", testRankerOrder},
      {"ranker_filler_key_isolated", testRankerFillerKeyIsolated},
      {"ranker_domain_order_isolated", testRankerDomainOrderIsolated},
      {"ranker_majority_per_band", testRankerMajorityPerBand},
      {"ranker_majority_skips_missing_master",
       testRankerMajoritySkipsMissingMaster},
      {"candidates_band_polarity_layout_must_match",
       testCandidatesBandPolarityLayoutMustMatch},
      {"candidates_polarity_only_filter_diagnosed",
       testCandidatesPolarityOnlyFilterDiagnosed},
      {"fake_design_caches_follow_mutation",
       testFakeDesignCachesFollowMutation},
      {"fake_udm_bottom_polarity_derived", testFakeUdmBottomPolarityDerived},
      {"enumeration_order_and_completeness", testEnumerationOrderAndCompleteness},
      {"enumeration_filler_domain_not_crowded_out",
       testEnumerationFillerDomainNotCrowdedOut},
      {"engine_solves_single_swap", testEngineSolvesSingleSwap},
      {"engine_solves_pair_non_monotone", testEngineSolvesPairNonMonotone},
      {"engine_complex_ranked_pair_fast", testEngineComplexRankedPairFast},
      {"engine_complex_third_vt_still_succeeds",
       testEngineComplexThirdVtStillSucceeds},
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
      {"engine_adaptive_solves_beyond_ring",
       testEngineAdaptiveSolvesBeyondRing},
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

  for (const Test& test : tests) {
    ::testing::RegisterTest(
        "FillerRepairPlanner", test.name, nullptr, nullptr, __FILE__, __LINE__,
        [fn = test.fn]() -> ::testing::Test* { return new PlannerTest(fn); });
  }
}

[[maybe_unused]] const bool kPlannerTestsRegistered = []() {
  registerPlannerTests();
  return true;
}();

}  // namespace
