#include "drc/ImplantLayerChecker.h"
#include "fillerRepair/Types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <vector>

namespace dpl2 {
namespace ipl {
namespace {

::Rect makeRect(Dbu xl, Dbu yl, Dbu xh, Dbu yh)
{
  return ::Rect(UvDist(eUTL::DbuValueInt32(static_cast<int32_t>(xl))),
                UvDist(eUTL::DbuValueInt32(static_cast<int32_t>(yl))),
                UvDist(eUTL::DbuValueInt32(static_cast<int32_t>(xh))),
                UvDist(eUTL::DbuValueInt32(static_cast<int32_t>(yh))));
}

constexpr Dbu SITE_WIDTH = 10;
constexpr Dbu ROW_HEIGHT = 100;
constexpr RowId ROW_COUNT = 8;
constexpr ColId SITE_COUNT = 200;
constexpr Dbu MIN_RULE = 20;

constexpr LayerId F1_LAYER = 1;
constexpr LayerId F2_LAYER = 2;
constexpr LayerId F3_LAYER = 3;

constexpr MasterId C1_MASTER = 101;
constexpr MasterId C2_MASTER = 102;
constexpr MasterId C3_MASTER = 103;
constexpr MasterId F1_FILL_MASTER = 201;
constexpr MasterId F2_FILL_MASTER = 202;
constexpr MasterId F3_FILL_MASTER = 203;

constexpr int F1_WIDTH_RULE = 101;
constexpr int F1_SPACING_RULE = 201;

constexpr RowId INTRA_WIDTH_ROW = 0;
constexpr ColId INTRA_WIDTH_COL = 10;
constexpr RowId INTER_WIDTH_TARGET_ROW = 1;
constexpr RowId INTER_WIDTH_NEIGHBOR_ROW = 2;
constexpr ColId INTER_WIDTH_COL = 30;
constexpr RowId INTRA_SPACING_ROW = 3;
constexpr ColId INTRA_SPACING_COL = 50;
constexpr RowId INTER_SPACING_TARGET_ROW = 4;
constexpr RowId INTER_SPACING_NEIGHBOR_ROW = 5;
constexpr ColId INTER_SPACING_COL = 70;

constexpr RowId NEW_INTRA_WIDTH_ROW = 0;
constexpr ColId NEW_INTRA_WIDTH_COL = 100;
constexpr RowId NEW_INTER_WIDTH_TOP_ROW = 0;
constexpr RowId NEW_INTER_WIDTH_BOTTOM_ROW = 1;
constexpr ColId NEW_INTER_WIDTH_COL = 120;
constexpr RowId NEW_INTRA_SPACING_ROW = 0;
constexpr ColId NEW_INTRA_SPACING_COL = 140;
constexpr RowId NEW_INTER_SPACING_TOP_ROW = 2;
constexpr RowId NEW_INTER_SPACING_BOTTOM_ROW = 3;
constexpr ColId NEW_INTER_SPACING_COL = 160;

constexpr RowId OLD_UNRELATED_ROW = 7;
constexpr ColId OLD_UNRELATED_COL = 190;

const char* denseOverlaySchematic()
{
  return R"(Schematic: dense_overlay_8x200
Legend: [ 1F1 ] = non-filler one-site cell on F1.
        [ aF1 ] = filler one-site cell on F1.
        1/2/3 use layers F1/F2/F3; a/b/c are F1/F2/F3 fillers.
        * marks the filler changed by the tested candidate.
        Each row is 200 sites wide and 100% occupied. Only repair windows are shown.

Background all rows:
Sites:    ... [ 1F1 ][ aF1 ][ 2F2 ][ bF2 ][ 3F3 ][ cF3 ] ...

Intra-row width target, row0 sites 8..13:
Sites:        08     09     10     11     12     13
before:   [ 2F2 ][ bF2 ][ 1F1 ][ bF2*][ 2F2 ][ bF2 ]
clear:    [ 2F2 ][ bF2 ][ 1F1 ][ aF1*][ 2F2 ][ bF2 ]

Inter-row width target, rows1/2 sites 30..31:
Sites:        30     31
row2 before:[ 1F1 ][ bF2*]
row2 clear: [ 1F1 ][ aF1*]
row1 target:[ 1F1 ][ aF1 ]

Intra-row spacing target, row3 sites 48..56:
Sites:        48     49     50     51     52     53     54     55     56
before:   [ bF2 ][ bF2 ][ 1F1 ][ aF1 ][ bF2*][ 1F1 ][ aF1 ][ bF2 ][ 2F2 ]
clear:    [ bF2 ][ bF2 ][ 1F1 ][ aF1 ][ aF1*][ 1F1 ][ aF1 ][ bF2 ][ 2F2 ]

Inter-row spacing target, rows4/5 sites 70..75:
Sites:        70     71     72     73     74     75
row5 before:[ ---- ][ ---- ][ bF2*][ 1F1 ][ aF1 ][ bF2 ]
row5 clear: [ ---- ][ ---- ][ aF1*][ 1F1 ][ aF1 ][ bF2 ]
row4 target:[ 1F1 ][ aF1 ][ 2F2 ][ bF2 ][ 2F2 ][ bF2 ]

New-violation windows:
Intra-row width row0 sites 98..103:
safe:     [ 2F2 ][ bF2 ][ 1F1 ][ aF1*][ 2F2 ][ bF2 ]
bad:      [ 2F2 ][ bF2 ][ 1F1 ][ bF2*][ 2F2 ][ bF2 ]

Inter-row width rows0/1 sites 120..121:
row1 safe:[ bF2*][ bF2 ]
row1 bad: [ aF1*][ bF2 ]
row0 ref: [ 1F1 ][ aF1 ]

Intra-row spacing row0 sites 140..145:
safe:     [ 1F1 ][ aF1 ][ bF2 ][ cF3*][ 1F1 ][ aF1 ]
bad:      [ 1F1 ][ aF1 ][ bF2 ][ aF1*][ 1F1 ][ aF1 ]

Inter-row spacing rows2/3 sites 160..165:
row3 safe:[ ---- ][ ---- ][ bF2 ][ cF3*][ 1F1 ][ aF1 ]
row3 bad: [ ---- ][ ---- ][ bF2 ][ aF1*][ 1F1 ][ aF1 ]
row2 ref: [ 1F1 ][ aF1 ][ 2F2 ][ bF2 ][ 2F2 ][ bF2 ]

Old unrelated baseline violation, row7 sites 189..191:
before:   [ bF2 ][ 1F1 ][ bF2 ]
)";
}

InstanceId instId(RowId rowId, ColId colId)
{
  return 10000 + rowId * 1000 + colId;
}

MasterId cellMaster(int layerIndex)
{
  return std::array<MasterId, 3>{C1_MASTER, C2_MASTER, C3_MASTER}
      [layerIndex];
}

MasterId fillerMaster(int layerIndex)
{
  return std::array<MasterId, 3>{
      F1_FILL_MASTER, F2_FILL_MASTER, F3_FILL_MASTER}[layerIndex];
}

LayerId layerId(int layerIndex)
{
  return std::array<LayerId, 3>{F1_LAYER, F2_LAYER, F3_LAYER}[layerIndex];
}

MasterInput master(MasterId masterId,
                   ShapeId shapeBase,
                   LayerId layer,
                   bool isFiller)
{
  MasterInput master;
  master.masterId = masterId;
  master.width = SITE_WIDTH;
  master.height = ROW_HEIGHT;
  master.isFiller = isFiller;
  master.shapes = {
      MasterShape{
          masterId, shapeBase, layer, makeRect(0, 50, SITE_WIDTH, 100)},
      MasterShape{masterId,
                  static_cast<ShapeId>(shapeBase + 1),
                  layer,
                  makeRect(0, 0, SITE_WIDTH, 50)}};
  return master;
}

Rule rule(int ruleId, RuleSource source, LayerId layer)
{
  Rule rule;
  rule.ruleId = ruleId;
  rule.source = source;
  rule.primaryLayer = layer;
  rule.minValue = MIN_RULE;
  return rule;
}

TrackPattern tracks()
{
  TrackPattern tracks;
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    tracks.layerBySlot[{rowId, BandSlot::Bottom}] = F1_LAYER;
    tracks.layerBySlot[{rowId, BandSlot::Top}] = F1_LAYER;
  }
  for (RowId rowId = 0; rowId + 1 < ROW_COUNT; ++rowId) {
    tracks.activeKindByBoundary[{rowId, rowId + 1}] = Polarity::N;
  }
  return tracks;
}

struct SiteSpec
{
  MasterId masterId = C1_MASTER;
  bool isFiller = false;
};

size_t siteIndex(RowId rowId, ColId colId)
{
  return static_cast<size_t>(rowId * SITE_COUNT + colId);
}

void setSite(std::vector<SiteSpec>& sites,
             RowId rowId,
             ColId colId,
             MasterId masterId,
             bool isFiller)
{
  sites[siteIndex(rowId, colId)] = SiteSpec{masterId, isFiller};
}

void setCell(std::vector<SiteSpec>& sites,
             RowId rowId,
             ColId colId,
             MasterId masterId)
{
  setSite(sites, rowId, colId, masterId, false);
}

void setFiller(std::vector<SiteSpec>& sites,
               RowId rowId,
               ColId colId,
               MasterId masterId)
{
  setSite(sites, rowId, colId, masterId, true);
}

std::vector<PlacedInst> densePlaced()
{
  std::vector<SiteSpec> sites(
      static_cast<size_t>(ROW_COUNT * SITE_COUNT));
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    for (ColId colId = 0; colId < SITE_COUNT; ++colId) {
      const int layerIndex = (colId / 2) % 3;
      const bool isFiller = colId % 2 == 1;
      sites[siteIndex(rowId, colId)]
          = SiteSpec{isFiller ? fillerMaster(layerIndex)
                              : cellMaster(layerIndex),
                     isFiller};
    }
  }

  setCell(sites, INTRA_WIDTH_ROW, 8, C2_MASTER);
  setFiller(sites, INTRA_WIDTH_ROW, 9, F2_FILL_MASTER);
  setCell(sites, INTRA_WIDTH_ROW, 10, C1_MASTER);
  setFiller(sites, INTRA_WIDTH_ROW, 11, F2_FILL_MASTER);
  setCell(sites, INTRA_WIDTH_ROW, 12, C2_MASTER);
  setFiller(sites, INTRA_WIDTH_ROW, 13, F2_FILL_MASTER);

  setCell(sites, INTER_WIDTH_TARGET_ROW, 30, C1_MASTER);
  setFiller(sites, INTER_WIDTH_TARGET_ROW, 31, F1_FILL_MASTER);
  setCell(sites, INTER_WIDTH_NEIGHBOR_ROW, 30, C1_MASTER);
  setFiller(sites, INTER_WIDTH_NEIGHBOR_ROW, 31, F2_FILL_MASTER);

  for (RowId rowId : {INTRA_SPACING_ROW - 1, INTRA_SPACING_ROW + 1}) {
    for (ColId colId = 48; colId <= 56; ++colId) {
      if (colId % 2 == 0) {
        setCell(sites, rowId, colId, C2_MASTER);
      } else {
        setFiller(sites, rowId, colId, F2_FILL_MASTER);
      }
    }
  }
  setFiller(sites, INTRA_SPACING_ROW, 48, F2_FILL_MASTER);
  setFiller(sites, INTRA_SPACING_ROW, 49, F2_FILL_MASTER);
  setCell(sites, INTRA_SPACING_ROW, 50, C1_MASTER);
  setFiller(sites, INTRA_SPACING_ROW, 51, F1_FILL_MASTER);
  setFiller(sites, INTRA_SPACING_ROW, 52, F2_FILL_MASTER);
  setCell(sites, INTRA_SPACING_ROW, 53, C1_MASTER);
  setFiller(sites, INTRA_SPACING_ROW, 54, F1_FILL_MASTER);
  setFiller(sites, INTRA_SPACING_ROW, 55, F2_FILL_MASTER);
  setCell(sites, INTRA_SPACING_ROW, 56, C2_MASTER);

  setCell(sites, INTER_SPACING_TARGET_ROW, 70, C1_MASTER);
  setFiller(sites, INTER_SPACING_TARGET_ROW, 71, F1_FILL_MASTER);
  setCell(sites, INTER_SPACING_TARGET_ROW, 72, C2_MASTER);
  setFiller(sites, INTER_SPACING_TARGET_ROW, 73, F2_FILL_MASTER);
  setCell(sites, INTER_SPACING_TARGET_ROW, 74, C2_MASTER);
  setFiller(sites, INTER_SPACING_TARGET_ROW, 75, F2_FILL_MASTER);
  setFiller(sites, INTER_SPACING_NEIGHBOR_ROW, 72, F2_FILL_MASTER);
  setCell(sites, INTER_SPACING_NEIGHBOR_ROW, 73, C1_MASTER);
  setFiller(sites, INTER_SPACING_NEIGHBOR_ROW, 74, F1_FILL_MASTER);
  setFiller(sites, INTER_SPACING_NEIGHBOR_ROW, 75, F2_FILL_MASTER);

  setCell(sites, NEW_INTRA_WIDTH_ROW, 98, C2_MASTER);
  setFiller(sites, NEW_INTRA_WIDTH_ROW, 99, F2_FILL_MASTER);
  setCell(sites, NEW_INTRA_WIDTH_ROW, 100, C1_MASTER);
  setFiller(sites, NEW_INTRA_WIDTH_ROW, 101, F1_FILL_MASTER);
  setCell(sites, NEW_INTRA_WIDTH_ROW, 102, C2_MASTER);
  setFiller(sites, NEW_INTRA_WIDTH_ROW, 103, F2_FILL_MASTER);

  setCell(sites, NEW_INTER_WIDTH_TOP_ROW, 120, C1_MASTER);
  setFiller(sites, NEW_INTER_WIDTH_TOP_ROW, 121, F1_FILL_MASTER);
  setFiller(sites, NEW_INTER_WIDTH_BOTTOM_ROW, 120, F2_FILL_MASTER);
  setFiller(sites, NEW_INTER_WIDTH_BOTTOM_ROW, 121, F2_FILL_MASTER);

  setCell(sites, NEW_INTRA_SPACING_ROW, 140, C1_MASTER);
  setFiller(sites, NEW_INTRA_SPACING_ROW, 141, F1_FILL_MASTER);
  setFiller(sites, NEW_INTRA_SPACING_ROW, 142, F2_FILL_MASTER);
  setFiller(sites, NEW_INTRA_SPACING_ROW, 143, F3_FILL_MASTER);
  setCell(sites, NEW_INTRA_SPACING_ROW, 144, C1_MASTER);
  setFiller(sites, NEW_INTRA_SPACING_ROW, 145, F1_FILL_MASTER);

  setCell(sites, NEW_INTER_SPACING_TOP_ROW, 160, C1_MASTER);
  setFiller(sites, NEW_INTER_SPACING_TOP_ROW, 161, F1_FILL_MASTER);
  setCell(sites, NEW_INTER_SPACING_TOP_ROW, 162, C2_MASTER);
  setFiller(sites, NEW_INTER_SPACING_TOP_ROW, 163, F2_FILL_MASTER);
  setCell(sites, NEW_INTER_SPACING_TOP_ROW, 164, C2_MASTER);
  setFiller(sites, NEW_INTER_SPACING_TOP_ROW, 165, F2_FILL_MASTER);
  setFiller(sites, NEW_INTER_SPACING_BOTTOM_ROW, 162, F2_FILL_MASTER);
  setFiller(sites, NEW_INTER_SPACING_BOTTOM_ROW, 163, F3_FILL_MASTER);
  setCell(sites, NEW_INTER_SPACING_BOTTOM_ROW, 164, C1_MASTER);
  setFiller(sites, NEW_INTER_SPACING_BOTTOM_ROW, 165, F1_FILL_MASTER);

  // Old unrelated violation inside the full guard. It must be present in the
  // baseline but filtered from successful overlay results.
  setFiller(sites, OLD_UNRELATED_ROW, 189, F2_FILL_MASTER);
  setCell(sites, OLD_UNRELATED_ROW, 190, C1_MASTER);
  setFiller(sites, OLD_UNRELATED_ROW, 191, F2_FILL_MASTER);

  std::vector<PlacedInst> placed;
  placed.reserve(static_cast<size_t>(ROW_COUNT * SITE_COUNT));
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    for (ColId colId = 0; colId < SITE_COUNT; ++colId) {
      const SiteSpec spec = sites[siteIndex(rowId, colId)];
      placed.push_back(PlacedInst{instId(rowId, colId),
                                  spec.masterId,
                                  rowId,
                                  colId,
                                  PhysOrientationE::R0,
                                  spec.isFiller});
    }
  }
  return placed;
}

ImplantInput input()
{
  ImplantInput input;
  input.layers = {
      ImplantLayer{F1_LAYER, "F1", Family::VTL, Polarity::N},
      ImplantLayer{F2_LAYER, "F2", Family::VTH, Polarity::N},
      ImplantLayer{F3_LAYER, "F3", Family::VTUL, Polarity::N}};
  input.rules = {rule(F1_WIDTH_RULE, RuleSource::Width, F1_LAYER),
                 rule(102, RuleSource::Width, F2_LAYER),
                 rule(103, RuleSource::Width, F3_LAYER),
                 rule(F1_SPACING_RULE, RuleSource::Spacing, F1_LAYER),
                 rule(202, RuleSource::Spacing, F2_LAYER),
                 rule(203, RuleSource::Spacing, F3_LAYER)};
  input.masters = {master(C1_MASTER, 1, F1_LAYER, false),
                   master(C2_MASTER, 3, F2_LAYER, false),
                   master(C3_MASTER, 5, F3_LAYER, false),
                   master(F1_FILL_MASTER, 7, F1_LAYER, true),
                   master(F2_FILL_MASTER, 9, F2_LAYER, true),
                   master(F3_FILL_MASTER, 11, F3_LAYER, true)};
  input.placedInsts = densePlaced();
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    input.rows.push_back(rowId);
  }
  input.tracks = tracks();
  input.rowHeight = ROW_HEIGHT;
  input.siteWidth = SITE_WIDTH;
  return input;
}

Rect guard()
{
  return makeRect(0, 0, SITE_COUNT * SITE_WIDTH, ROW_COUNT * ROW_HEIGHT);
}

CheckRequest request(RowId rowId, ColId colId)
{
  return CheckRequest{instId(rowId, colId),
                      C1_MASTER,
                      rowId,
                      colId,
                      PhysOrientationE::R0};
}

bool hasViolation(const CheckResult& result,
                  int ruleId,
                  Relationship relationship,
                  std::initializer_list<InstanceId> instanceIds)
{
  return std::any_of(
      result.violations.begin(),
      result.violations.end(),
      [ruleId, relationship, instanceIds](const Violation& violation) {
        if (violation.ruleId != ruleId
            || violation.relationship != relationship) {
          return false;
        }
        for (InstanceId instanceId : instanceIds) {
          if (std::find(violation.instances.begin(),
                        violation.instances.end(),
                        instanceId)
              == violation.instances.end()) {
            return false;
          }
        }
        return true;
      });
}

bool hasDiagnostic(const CheckResult& result, const std::string& status)
{
  return std::any_of(result.diagnostics.begin(),
                     result.diagnostics.end(),
                     [&status](const Diagnostic& diagnostic) {
                       return diagnostic.status == status;
                     });
}

void expectOldUnrelatedFiltered(const CheckResult& result)
{
  EXPECT_FALSE(hasViolation(result,
                            F1_WIDTH_RULE,
                            Relationship::IntraRow,
                            {instId(OLD_UNRELATED_ROW, OLD_UNRELATED_COL)}));
}

std::vector<CheckResult> check(
    const CheckRequest& request,
    const std::vector<std::vector<FillerChange>>& changes)
{
  SCOPED_TRACE(denseOverlaySchematic());
  ImplantLayerChecker checker(nullptr);
  EXPECT_TRUE(checker.initialize(input()));
  EXPECT_TRUE(checker.initDiagnostics().empty());
  std::vector<CheckResult> results
      = checker.checkPlaceWithOverlays(request, guard(), changes);
  if (std::getenv("DPL2_CHECKER_TEST_DEBUG") != nullptr) {
    for (size_t index = 0; index < results.size(); ++index) {
      std::cerr << "candidate " << index << " legal=" << results[index].isLegal
                << " violations=" << results[index].violations.size() << '\n';
      for (const Violation& violation : results[index].violations) {
        std::cerr << violation.toString(SITE_WIDTH) << " rows=";
        for (RowId rowId : violation.rowIds) {
          std::cerr << rowId << ',';
        }
        std::cerr << '\n';
      }
    }
  }
  return results;
}

TEST(ImplantCheckerOverlayTest, DenseCaseIntraRowWidth)
{
  const CheckRequest target = request(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  const std::vector<CheckResult> results = check(
      target,
      {{FillerChange{instId(INTRA_WIDTH_ROW, 11), F1_FILL_MASTER}},
       {FillerChange{instId(INTRA_WIDTH_ROW, 11), F2_FILL_MASTER}},
       {FillerChange{instId(INTRA_WIDTH_ROW, 11), F1_FILL_MASTER},
        FillerChange{instId(NEW_INTRA_WIDTH_ROW, 101), F2_FILL_MASTER}}});

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].violations.empty());
  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasViolation(results[1],
                           F1_WIDTH_RULE,
                           Relationship::IntraRow,
                           {target.instanceId}));
  EXPECT_FALSE(results[2].isLegal);
  EXPECT_FALSE(hasViolation(results[2],
                            F1_WIDTH_RULE,
                            Relationship::IntraRow,
                            {target.instanceId}));
  EXPECT_TRUE(hasViolation(results[2],
                           F1_WIDTH_RULE,
                           Relationship::IntraRow,
                           {instId(NEW_INTRA_WIDTH_ROW, NEW_INTRA_WIDTH_COL)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST(ImplantCheckerOverlayTest, DenseCaseInterRowWidth)
{
  const CheckRequest target
      = request(INTER_WIDTH_TARGET_ROW, INTER_WIDTH_COL);
  const InstanceId neighbor
      = instId(INTER_WIDTH_NEIGHBOR_ROW, INTER_WIDTH_COL);
  const std::vector<CheckResult> results = check(
      target,
      {{FillerChange{instId(INTER_WIDTH_NEIGHBOR_ROW, 31), F1_FILL_MASTER}},
       {FillerChange{instId(INTER_WIDTH_NEIGHBOR_ROW, 31), F2_FILL_MASTER}},
       {FillerChange{instId(INTER_WIDTH_NEIGHBOR_ROW, 31), F1_FILL_MASTER},
        FillerChange{instId(NEW_INTER_WIDTH_BOTTOM_ROW, NEW_INTER_WIDTH_COL),
                     F1_FILL_MASTER}}});

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].violations.empty());
  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasViolation(results[1],
                           F1_WIDTH_RULE,
                           Relationship::InterRow,
                           {target.instanceId, neighbor}));
  EXPECT_FALSE(results[2].isLegal);
  EXPECT_FALSE(hasViolation(results[2],
                            F1_WIDTH_RULE,
                            Relationship::InterRow,
                            {target.instanceId, neighbor}));
  EXPECT_TRUE(hasViolation(
      results[2],
      F1_WIDTH_RULE,
      Relationship::InterRow,
      {instId(NEW_INTER_WIDTH_TOP_ROW, NEW_INTER_WIDTH_COL),
       instId(NEW_INTER_WIDTH_BOTTOM_ROW, NEW_INTER_WIDTH_COL)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST(ImplantCheckerOverlayTest, DenseCaseIntraRowSpacing)
{
  const CheckRequest target = request(INTRA_SPACING_ROW, INTRA_SPACING_COL);
  const InstanceId neighbor
      = instId(INTRA_SPACING_ROW, INTRA_SPACING_COL + 3);
  const std::vector<CheckResult> results = check(
      target,
      {{FillerChange{instId(INTRA_SPACING_ROW, 52), F1_FILL_MASTER}},
       {FillerChange{instId(INTRA_SPACING_ROW, 52), F2_FILL_MASTER}},
       {FillerChange{instId(INTRA_SPACING_ROW, 52), F1_FILL_MASTER},
        FillerChange{instId(NEW_INTRA_SPACING_ROW, 143),
                     F1_FILL_MASTER}}});

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].violations.empty());
  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasViolation(results[1],
                           F1_SPACING_RULE,
                           Relationship::IntraRow,
                           {target.instanceId, neighbor}));
  EXPECT_FALSE(results[2].isLegal);
  EXPECT_FALSE(hasViolation(results[2],
                            F1_SPACING_RULE,
                            Relationship::IntraRow,
                            {target.instanceId, neighbor}));
  EXPECT_TRUE(hasViolation(
      results[2],
      F1_SPACING_RULE,
      Relationship::IntraRow,
      {instId(NEW_INTRA_SPACING_ROW, NEW_INTRA_SPACING_COL),
       instId(NEW_INTRA_SPACING_ROW, NEW_INTRA_SPACING_COL + 4)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST(ImplantCheckerOverlayTest, DenseCaseInterRowSpacing)
{
  const CheckRequest target
      = request(INTER_SPACING_TARGET_ROW, INTER_SPACING_COL);
  const InstanceId neighbor
      = instId(INTER_SPACING_NEIGHBOR_ROW, INTER_SPACING_COL + 3);
  const std::vector<CheckResult> results = check(
      target,
      {{FillerChange{instId(INTER_SPACING_NEIGHBOR_ROW, 72), F1_FILL_MASTER}},
       {FillerChange{instId(INTER_SPACING_NEIGHBOR_ROW, 72), F2_FILL_MASTER}},
       {FillerChange{instId(INTER_SPACING_NEIGHBOR_ROW, 72), F1_FILL_MASTER},
        FillerChange{instId(NEW_INTER_SPACING_BOTTOM_ROW, 163),
                     F1_FILL_MASTER}}});

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].violations.empty());
  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasViolation(results[1],
                           F1_SPACING_RULE,
                           Relationship::InterRow,
                           {target.instanceId, neighbor}));
  EXPECT_FALSE(results[2].isLegal);
  EXPECT_FALSE(hasViolation(results[2],
                            F1_SPACING_RULE,
                            Relationship::InterRow,
                            {target.instanceId, neighbor}));
  EXPECT_TRUE(hasViolation(
      results[2],
      F1_SPACING_RULE,
      Relationship::InterRow,
      {instId(NEW_INTER_SPACING_TOP_ROW, NEW_INTER_SPACING_COL),
       instId(NEW_INTER_SPACING_BOTTOM_ROW, NEW_INTER_SPACING_COL + 4)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST(ImplantCheckerOverlayTest, RawKeepsUnrelatedBaselineWithRows)
{
  SCOPED_TRACE(denseOverlaySchematic());
  ImplantLayerChecker checker(nullptr);
  EXPECT_TRUE(checker.initialize(input()));

  const CheckRequest target = request(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  const std::vector<CheckResult> results = checker.checkPlaceWithOverlaysRaw(
      target,
      guard(),
      {{FillerChange{instId(INTRA_WIDTH_ROW, 11), F1_FILL_MASTER}}});

  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results[0].isLegal);
  size_t oldUnrelatedCount = 0;
  for (const Violation& violation : results[0].violations) {
    if (violation.ruleId == F1_WIDTH_RULE
        && violation.relationship == Relationship::IntraRow
        && std::find(violation.instances.begin(),
                     violation.instances.end(),
                     instId(OLD_UNRELATED_ROW, OLD_UNRELATED_COL))
               != violation.instances.end()) {
      ++oldUnrelatedCount;
      ASSERT_EQ(violation.rowIds.size(), 1u);
      EXPECT_TRUE(violation.rowIds.front() == OLD_UNRELATED_ROW);
    }
  }
  EXPECT_TRUE(oldUnrelatedCount >= 1u);
}

TEST(ImplantCheckerOverlayTest, PlannerTypesCoexistWithCheckerTypes)
{
  static_assert(
      !std::is_same_v<XInterval, dpl2::fillerRepair::XInterval>,
      "planner and checker intervals must be adapter-separated types");
  const XInterval checkerInterval{10, 20};
  const dpl2::fillerRepair::XInterval plannerInterval{10, 20};
  EXPECT_TRUE(checkerInterval.xl == plannerInterval.xl);
  EXPECT_TRUE(checkerInterval.xh == plannerInterval.xh);
}

TEST(ImplantCheckerOverlayTest, InvalidOverlayBatchIsolation)
{
  SCOPED_TRACE(denseOverlaySchematic());
  ImplantInput data = input();
  constexpr MasterId WIDE_FILL_MASTER = 299;
  MasterInput wide;
  wide.masterId = WIDE_FILL_MASTER;
  wide.width = 2 * SITE_WIDTH;
  wide.height = ROW_HEIGHT;
  wide.isFiller = true;
  wide.shapes = {
      MasterShape{WIDE_FILL_MASTER,
                  41,
                  F1_LAYER,
                  makeRect(0, 50, 2 * SITE_WIDTH, ROW_HEIGHT)},
      MasterShape{WIDE_FILL_MASTER,
                  42,
                  F1_LAYER,
                  makeRect(0, 0, 2 * SITE_WIDTH, 50)}};
  data.masters.push_back(wide);

  ImplantLayerChecker checker(nullptr);
  EXPECT_TRUE(checker.initialize(data));
  const CheckRequest target = request(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  const InstanceId validFiller = instId(INTRA_WIDTH_ROW, 11);
  const std::vector<CheckResult> results = checker.checkPlaceWithOverlays(
      target,
      guard(),
      {{FillerChange{validFiller, F1_FILL_MASTER}},
       {FillerChange{validFiller, F1_FILL_MASTER},
        FillerChange{validFiller, F2_FILL_MASTER}},
       {FillerChange{instId(INTRA_WIDTH_ROW, 8), F1_FILL_MASTER}},
       {FillerChange{-12345, F1_FILL_MASTER}},
       {FillerChange{validFiller, C1_MASTER}},
       {FillerChange{validFiller, WIDE_FILL_MASTER}}});

  ASSERT_EQ(results.size(), 6u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].diagnostics.empty());
  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasDiagnostic(results[1], "duplicate_filler_change"));
  EXPECT_FALSE(results[2].isLegal);
  EXPECT_TRUE(hasDiagnostic(results[2], "changed_instance_not_filler"));
  EXPECT_FALSE(results[3].isLegal);
  EXPECT_TRUE(hasDiagnostic(results[3], "unknown_filler_instance"));
  EXPECT_FALSE(results[4].isLegal);
  EXPECT_TRUE(hasDiagnostic(results[4], "replacement_master_not_filler"));
  EXPECT_FALSE(results[5].isLegal);
  EXPECT_TRUE(hasDiagnostic(results[5], "replacement_footprint_mismatch"));
}

TEST(ImplantCheckerOverlayTest, RowHashAndGuardClipping)
{
  SCOPED_TRACE(denseOverlaySchematic());
  ImplantInput data = input();
  for (PlacedInst& instance : data.placedInsts) {
    const bool rowMatch = instance.rowId == 6 || instance.rowId == 7;
    if (rowMatch && (instance.colId == 179 || instance.colId == 181)) {
      instance.masterId = F2_FILL_MASTER;
      instance.isFiller = true;
    } else if (rowMatch && instance.colId == 180) {
      instance.masterId = C1_MASTER;
      instance.isFiller = false;
    }
  }

  ImplantLayerChecker checker(nullptr);
  EXPECT_TRUE(checker.initialize(data));
  const CheckRequest target = request(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  const auto full = checker.checkPlaceWithOverlaysRaw(target, guard(), {{}});
  ASSERT_EQ(full.size(), 1u);

  const auto findAtRow = [](const CheckResult& result,
                            RowId rowId) -> const Violation* {
    const InstanceId center = instId(rowId, 180);
    for (const Violation& violation : result.violations) {
      if (violation.ruleId == F1_WIDTH_RULE
          && violation.relationship == Relationship::IntraRow
          && std::find(violation.instances.begin(),
                       violation.instances.end(),
                       center)
                 != violation.instances.end()) {
        return &violation;
      }
    }
    return nullptr;
  };

  const Violation* row6 = findAtRow(full[0], 6);
  const Violation* row7 = findAtRow(full[0], 7);
  EXPECT_TRUE(row6 != nullptr);
  EXPECT_TRUE(row7 != nullptr);
  if (row6 == nullptr || row7 == nullptr) {
    return;
  }
  EXPECT_TRUE(row6->xWindow.xl == row7->xWindow.xl);
  EXPECT_TRUE(row6->xWindow.xh == row7->xWindow.xh);
  EXPECT_TRUE(row6->rowIds == std::vector<RowId>{6});
  EXPECT_TRUE(row7->rowIds == std::vector<RowId>{7});
  EXPECT_TRUE(row6->hash != row7->hash);

  const Rect row6Guard = makeRect(0,
                                  6 * ROW_HEIGHT,
                                  SITE_COUNT * SITE_WIDTH,
                                  7 * ROW_HEIGHT - 1);
  const auto clipped
      = checker.checkPlaceWithOverlaysRaw(target, row6Guard, {{}});
  ASSERT_EQ(clipped.size(), 1u);
  EXPECT_TRUE(findAtRow(clipped[0], 6) != nullptr);
  EXPECT_TRUE(findAtRow(clipped[0], 7) == nullptr);
}

}  // namespace
}  // namespace ipl
}  // namespace dpl2
