#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "drc/ImplantLayerChecker.h"
#include "drc/ImplantLayerCheckerHelper.h"
#include "fillerRepair/FillerRepairPlanner.h"

namespace dpl2 {
namespace ipl {
namespace {

::Rect makeRect(Dbu xl, Dbu yl, Dbu xh, Dbu yh)
{
  return ::Rect(UvDist(static_cast<int64_t>(xl)),
                UvDist(static_cast<int64_t>(yl)),
                UvDist(static_cast<int64_t>(xh)),
                UvDist(static_cast<int64_t>(yh)));
}

constexpr Dbu SITE_WIDTH = 10;
constexpr Dbu ROW_HEIGHT = 100;
constexpr RowId ROW_COUNT = 8;
constexpr ColId SITE_COUNT = 200;
constexpr Dbu MIN_RULE = 20;

// Layer ids ARE indices into ImplantInput::layers, exactly like rule ids:
// the checker resolves them as layers_[id] (its own buildLayers assigns
// layers_.size()), so any other numbering indexes out of bounds.
constexpr LayerId F1_LAYER = 0;
constexpr LayerId F2_LAYER = 1;
constexpr LayerId F3_LAYER = 2;
// P-polarity partner layer per VT family (top band; checker slotPolar
// alternates band polarity per row, so a master carries an N bottom band and
// a P top band and odd-row placements are MX-flipped). Kept at +3 so
// master() can derive the partner from the family layer.
constexpr LayerId F1P_LAYER = 3;
constexpr LayerId F2P_LAYER = 4;
constexpr LayerId F3P_LAYER = 5;

// MasterIds are sequential Network indices (order matches input().masters)
constexpr MasterId C1_MASTER = 0;
constexpr MasterId C2_MASTER = 1;
constexpr MasterId C3_MASTER = 2;
constexpr MasterId F1_FILL_MASTER = 3;
constexpr MasterId F2_FILL_MASTER = 4;
constexpr MasterId F3_FILL_MASTER = 5;

// Rule ids ARE indices into ImplantInput::rules: the checker looks rules up
// as rules_[ruleId] (its own buildRules assigns rules_.size()), so any other
// numbering indexes out of bounds. Keep these in sync with input().rules.
constexpr int F1_WIDTH_RULE = 0;    // N-band width, F1 family
constexpr int F1_SPACING_RULE = 3;  // N-band spacing, F1 family
constexpr int P_RULE_OFFSET = 6;    // same rule on the P-band partner layer

// Inter-row interactions run through the facing band pair across the row
// boundary. An even->odd boundary faces two P-band shapes; an odd->even
// boundary faces two N-band shapes.
constexpr int interRule(int baseRule, RowId topRowOfBoundary)
{
  return (topRowOfBoundary % 2) == 0 ? baseRule + P_RULE_OFFSET : baseRule;
}

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

struct DensityCase
{
  int fillerPercent;
  int stdCellPercent;
  const char* name;
};

constexpr DensityCase FILLER_50_STD_50{50, 50, "Filler50Std50"};
constexpr DensityCase FILLER_30_STD_70{30, 70, "Filler30Std70"};
constexpr DensityCase FILLER_20_STD_80{20, 80, "Filler20Std80"};
constexpr DensityCase FILLER_10_STD_90{10, 90, "Filler10Std90"};
constexpr DensityCase FILLER_5_STD_95{5, 95, "Filler5Std95"};

const char* denseOverlaySchematic();

std::string densityTrace(const DensityCase& density)
{
  return std::string("Target-local density: filler:stdCell=")
         + std::to_string(density.fillerPercent) + ':'
         + std::to_string(density.stdCellPercent) + " (" + density.name
         + ")\n" + denseOverlaySchematic();
}

const char* denseOverlaySchematic()
{
  return R"(Schematic: dense_overlay_8x200
  Legend: [ 1F1 ] = non-filler one-site cell on F1.
          [ aF1 ] = filler one-site cell on F1.
        1/2/3 use layers F1/F2/F3; a/b/c are F1/F2/F3 fillers.
        * marks the filler changed by the tested candidate.
        Each row is 200 sites wide and 100% occupied.
        Only repair windows are shown.
        Each density window is 20 columns across target row +/-1 (clamped).
        Filler identity inside every density window follows the test ratio;
        implant geometry and the required editable fillers stay unchanged.

Background implant geometry (filler/std identity is density-dependent):
Sites:    ... [ 1F1 ][ aF1 ][ 2F2 ][ bF2 ][ 3F3 ][ cF3 ] ...

Intra-row width target, row0 sites 8..13:
Sites:       08      09      10      11      12      13
before:   [ 2F2 ][ bF2 ][ 1F1 ][ bF2*][ 2F2 ][ bF2 ]
clear:    [ 2F2 ][ bF2 ][ 1F1 ][ aF1*][ 2F2 ][ bF2 ]

Inter-row width target, rows1/2 sites 30..31:
Sites:       30      31
row2 before:[ 1F1 ][ bF2*]
row2 clear: [ 1F1 ][ aF1*]
row1 target:[ 1F1 ][ aF1 ]

Intra-row spacing target, row3 sites 48..56:
Sites:       48      49      50      51      52      53      54      55      56
before:   [ bF2 ][ bF2 ][ 1F1 ][ aF1 ][ bF2*][ 1F1 ][ aF1 ][ bF2 ][ 2F2 ]
clear:    [ bF2 ][ bF2 ][ 1F1 ][ aF1 ][ aF1*][ 1F1 ][ aF1 ][ bF2 ][ 2F2 ]

Inter-row spacing target, rows4/5 sites 70..75:
Sites:       70      71      72      73      74      75
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
  return rowId * SITE_COUNT + colId;
}

MasterId cellMaster(int layerIndex)
{
  return std::array<MasterId, 3>{C1_MASTER, C2_MASTER, C3_MASTER}[layerIndex];
}

MasterId fillerMaster(int layerIndex)
{
  return std::array<MasterId, 3>{
      F1_FILL_MASTER, F2_FILL_MASTER, F3_FILL_MASTER}[layerIndex];
}

MasterItem master(MasterId masterId,
                  ShapeId shapeBase,
                  LayerId layer,
                  bool isFiller)
{
  MasterItem master;
  master.masterId = masterId;
  master.width = SITE_WIDTH;
  master.height = ROW_HEIGHT;
  master.siteHeight = ROW_HEIGHT;
  master.isFiller = isFiller;
  // N-polar family layer on the bottom band, its P partner on the top band
  // (layer + 3 by construction above).
  master.shapes = {
      MasterShape{masterId,
                  shapeBase,
                  static_cast<LayerId>(layer + 3),
                  makeRect(0, 50, SITE_WIDTH, 100)},
      MasterShape{masterId,
                  static_cast<ShapeId>(shapeBase + 1),
                  layer,
                  makeRect(0, 0, SITE_WIDTH, 50)}};
  master.rawShapes = master.shapes;
  return master;
}

Rule rule(int ruleId, RuleSource source, LayerId layer)
{
  return Rule(ruleId, source, layer, MIN_RULE);
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

int masterLayerIndex(MasterId masterId)
{
  return masterId >= F1_FILL_MASTER ? masterId - F1_FILL_MASTER
                                    : masterId - C1_MASTER;
}

void setFillerIdentity(SiteSpec& site, bool isFiller)
{
  const int layerIndex = masterLayerIndex(site.masterId);
  site.masterId
      = isFiller ? fillerMaster(layerIndex) : cellMaster(layerIndex);
  site.isFiller = isFiller;
}

using SiteCoord = std::pair<RowId, ColId>;

struct LocalDensityWindow
{
  const char* name;
  RowId rowLo;
  RowId rowHi;
  ColId colLo;
  ColId colHi;
  std::vector<SiteCoord> requiredFillers;
  std::vector<SiteCoord> requiredStdCells;
};

const std::array<LocalDensityWindow, 4>& localDensityWindows()
{
  static const std::array<LocalDensityWindow, 4> windows{
      LocalDensityWindow{"IntraRowWidth",
                         0,
                         1,
                         2,
                         22,
                         {{INTRA_WIDTH_ROW, 11}},
                         {{0, 8},
                          {0, 9},
                          {0, 10},
                          {0, 12},
                          {0, 13},
                          {1, 8},
                          {1, 9},
                          {1, 10},
                          {1, 11},
                          {1, 12},
                          {1, 13}}},
      LocalDensityWindow{
          "InterRowWidth",
          0,
          2,
          22,
          42,
          {{INTER_WIDTH_NEIGHBOR_ROW, 31}},
          {{0, 30},
           {0, 31},
           {1, 30},
           {1, 31},
           {2, 30}}},
      LocalDensityWindow{
          "IntraRowSpacing",
          2,
          4,
          42,
          62,
          {{INTRA_SPACING_ROW, 51}, {INTRA_SPACING_ROW, 52}},
          {{2, 48},
           {2, 49},
           {2, 50},
           {2, 51},
           {2, 52},
           {2, 53},
           {2, 54},
           {2, 55},
           {2, 56},
           {3, 48},
           {3, 49},
           {3, 50},
           {3, 53},
           {3, 54},
           {3, 55},
           {3, 56},
           {4, 48},
           {4, 49},
           {4, 50},
           {4, 51},
           {4, 52},
           {4, 53},
           {4, 54},
           {4, 55},
           {4, 56}}},
      LocalDensityWindow{
          "InterRowSpacing",
          3,
          5,
          62,
          82,
          {{INTER_SPACING_NEIGHBOR_ROW, 72}},
          {{3, 70},
           {3, 71},
           {3, 72},
           {3, 73},
           {3, 74},
           {3, 75},
           {4, 70},
           {4, 71},
           {4, 72},
           {4, 73},
           {4, 74},
           {4, 75},
           {5, 70},
           {5, 71},
           {5, 73},
           {5, 74},
           {5, 75}}}};
  return windows;
}

bool containsSite(const std::vector<SiteCoord>& sites,
                  RowId rowId,
                  ColId colId)
{
  return std::find(sites.begin(), sites.end(), SiteCoord{rowId, colId})
         != sites.end();
}

void applyDensityToWindow(std::vector<SiteSpec>& sites,
                          const LocalDensityWindow& window,
                          const DensityCase& density)
{
  const size_t ratioTotal
      = static_cast<size_t>(density.fillerPercent + density.stdCellPercent);
  const size_t siteCount
      = static_cast<size_t>(window.rowHi - window.rowLo + 1)
        * static_cast<size_t>(window.colHi - window.colLo);
  const size_t scaledFillerCount
      = siteCount * static_cast<size_t>(density.fillerPercent);
  if (ratioTotal == 0 || scaledFillerCount % ratioTotal != 0) {
    throw std::logic_error("density ratio does not divide the local window");
  }
  const size_t desiredFillerCount = scaledFillerCount / ratioTotal;
  if (window.requiredFillers.size() > desiredFillerCount) {
    throw std::logic_error("required repair fillers exceed local density");
  }

  std::vector<size_t> available;
  for (RowId rowId = window.rowLo; rowId <= window.rowHi; ++rowId) {
    for (ColId colId = window.colLo; colId < window.colHi; ++colId) {
      SiteSpec& site = sites[siteIndex(rowId, colId)];
      setFillerIdentity(site, false);
      if (!containsSite(window.requiredFillers, rowId, colId)
          && !containsSite(window.requiredStdCells, rowId, colId)) {
        available.push_back(siteIndex(rowId, colId));
      }
    }
  }
  for (const SiteCoord& site : window.requiredFillers) {
    setFillerIdentity(sites[siteIndex(site.first, site.second)], true);
  }

  const size_t additionalFillers
      = desiredFillerCount - window.requiredFillers.size();
  if (additionalFillers > available.size()) {
    throw std::logic_error("local window cannot satisfy density ratio");
  }
  for (size_t rank = 0; rank < available.size(); ++rank) {
    // Bresenham-style distribution: exact count without clustering the
    // sparse fillers at one end of the target-local window.
    const size_t before = rank * additionalFillers / available.size();
    const size_t after = (rank + 1) * additionalFillers / available.size();
    if (after != before) {
      setFillerIdentity(sites[available[rank]], true);
    }
  }
}

void applyLocalDensities(std::vector<SiteSpec>& sites,
                         const DensityCase& density)
{
  for (const LocalDensityWindow& window : localDensityWindows()) {
    applyDensityToWindow(sites, window, density);
  }
}

std::vector<PlacedInst> densePlaced(const DensityCase& density)
{
  std::vector<SiteSpec> sites(static_cast<size_t>(ROW_COUNT * SITE_COUNT));
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    for (ColId colId = 0; colId < SITE_COUNT; ++colId) {
      const int layerIndex = (colId / 2) % 3;
      const bool isFiller = colId % 2 == 1;
      sites[siteIndex(rowId, colId)] = SiteSpec{
          isFiller ? fillerMaster(layerIndex) : cellMaster(layerIndex),
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

  applyLocalDensities(sites, density);

  std::vector<PlacedInst> placed;
  placed.reserve(static_cast<size_t>(ROW_COUNT * SITE_COUNT));
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    for (ColId colId = 0; colId < SITE_COUNT; ++colId) {
      const SiteSpec spec = sites[siteIndex(rowId, colId)];
      placed.push_back(PlacedInst{instId(rowId, colId),
                                  spec.masterId,
                                  rowId,
                                  colId,
                                  (rowId % 2) != 0 ? PhysOrientationE::MX
                                                   : PhysOrientationE::R0,
                                  spec.isFiller});
    }
  }
  return placed;
}

ImplantInput input(const DensityCase& density = FILLER_50_STD_50)
{
  ImplantInput input;
  input.layers = {Layer{F1_LAYER, "F1_N", Layer::Vt::L, Layer::Polar::N},
                  Layer{F2_LAYER, "F2_N", Layer::Vt::H, Layer::Polar::N},
                  Layer{F3_LAYER, "F3_N", Layer::Vt::UL, Layer::Polar::N},
                  Layer{F1P_LAYER, "F1_P", Layer::Vt::L, Layer::Polar::P},
                  Layer{F2P_LAYER, "F2_P", Layer::Vt::H, Layer::Polar::P},
                  Layer{F3P_LAYER, "F3_P", Layer::Vt::UL, Layer::Polar::P}};
  // Index == rule id (see F1_WIDTH_RULE above).
  input.rules = {rule(0, RuleSource::Width, F1_LAYER),
                 rule(1, RuleSource::Width, F2_LAYER),
                 rule(2, RuleSource::Width, F3_LAYER),
                 rule(3, RuleSource::Spacing, F1_LAYER),
                 rule(4, RuleSource::Spacing, F2_LAYER),
                 rule(5, RuleSource::Spacing, F3_LAYER),
                 rule(6, RuleSource::Width, F1P_LAYER),
                 rule(7, RuleSource::Width, F2P_LAYER),
                 rule(8, RuleSource::Width, F3P_LAYER),
                 rule(9, RuleSource::Spacing, F1P_LAYER),
                 rule(10, RuleSource::Spacing, F2P_LAYER),
                 rule(11, RuleSource::Spacing, F3P_LAYER)};
  input.masters = {master(C1_MASTER, 1, F1_LAYER, false),
                   master(C2_MASTER, 3, F2_LAYER, false),
                   master(C3_MASTER, 5, F3_LAYER, false),
                   master(F1_FILL_MASTER, 7, F1_LAYER, true),
                   master(F2_FILL_MASTER, 9, F2_LAYER, true),
                   master(F3_FILL_MASTER, 11, F3_LAYER, true)};
  input.placedInsts = densePlaced(density);
  input.rowCount = ROW_COUNT;
  input.colCount = SITE_COUNT;
  input.basePolar = Layer::Polar::N;
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
                      (rowId % 2) != 0 ? PhysOrientationE::MX
                                       : PhysOrientationE::R0};
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

void expectOldUnrelatedFiltered(const CheckResult& result)
{
  EXPECT_FALSE(hasViolation(result,
                            F1_WIDTH_RULE,
                            Relationship::IntraRow,
                            {instId(OLD_UNRELATED_ROW, OLD_UNRELATED_COL)}));
}

LeafCellID leafCellId(RowId rowId, ColId colId)
{
  return LeafCellID(0, instId(rowId, colId));
}

LibCellID libCellId(MasterId masterId)
{
  return LibCellID(0, masterId);
}

std::vector<CheckResult> check(const CheckRequest& request,
                               std::vector<FillerChanges> changes,
                               const DensityCase& density)
{
  SCOPED_TRACE(densityTrace(density));
  const ImplantInput in = input(density);
  ImplantLayerCheckerHelper helper;
  helper.initialize(in);
  ImplantLayerChecker checker(helper.getGrid(), helper.getNetwork());
  helper.initChecker(checker);
  EXPECT_TRUE(checker.getDiags().empty());
  return checker.checkPlaceWithOverlays(request, guard(), changes);
}

namespace fr = ::dpl2::fillerRepair;

fr::Orient toPlannerOrient(PhysOrientation orientation)
{
  if (orientation == PhysOrientationE::R180)
    return fr::Orient::R180;
  if (orientation == PhysOrientationE::MX)
    return fr::Orient::MX;
  if (orientation == PhysOrientationE::MY)
    return fr::Orient::MY;
  return fr::Orient::R0;
}

PhysOrientation toCheckerOrient(fr::Orient orientation)
{
  switch (orientation) {
    case fr::Orient::R180:
      return PhysOrientationE::R180;
    case fr::Orient::MX:
      return PhysOrientationE::MX;
    case fr::Orient::MY:
      return PhysOrientationE::MY;
    case fr::Orient::R0:
      break;
  }
  return PhysOrientationE::R0;
}

fr::VtId toPlannerVt(Layer::Vt vt)
{
  switch (vt) {
    case Layer::Vt::S:
      return 0;
    case Layer::Vt::L:
      return 1;
    case Layer::Vt::H:
      return 2;
    case Layer::Vt::UL:
      return 3;
    case Layer::Vt::Unknown:
      break;
  }
  return fr::kUnknownVt;
}

// Portable test view built from the same ImplantInput consumed by the final
// checker's official test helper.  It deliberately implements only the pure
// planner boundary: no UDM session, DEF/LEF reader, or database mutation is
// required to exercise planner -> overlay checker end to end.
class PortablePlannerDataSource final : public fr::PlannerDataSource
{
 public:
  explicit PortablePlannerDataSource(
      const ImplantInput& input,
      std::optional<std::vector<fr::MasterId>> configuredFillers = std::nullopt)
      : site_width_(input.siteWidth)
  {
    std::map<LayerId, Layer> layers;
    for (const Layer& layer : input.layers) {
      layers[layer.getId()] = layer;
    }
    for (size_t index = 0; index < input.masters.size(); ++index) {
      const MasterItem& master = input.masters[index];
      fr::MasterInfo info;
      info.id = master.masterId;
      info.width = master.width;
      info.height = std::max<Dbu>(
          1, input.rowHeight > 0 ? master.height / input.rowHeight : 1);
      info.isFiller = master.isFiller;
      if (!master.shapes.empty()) {
        const auto layer = layers.find(master.shapes.front().layer);
        if (layer != layers.end()) {
          info.vt = toPlannerVt(layer->second.getVt());
          info.bottomBandPolarity = layer->second.getPolar() == Layer::Polar::P
                                        ? fr::BandPolarity::P
                                        : fr::BandPolarity::N;
        }
      }
      masters_[info.id] = info;
      if (info.isFiller)
        filler_master_ids_.push_back(info.id);
    }
    if (configuredFillers.has_value()) {
      filler_master_ids_ = std::move(*configuredFillers);
    }

    for (RowId rowId = 0; rowId < input.rowCount; ++rowId) {
      rows_.push_back(static_cast<fr::RowId>(rowId));
      row_spans_[static_cast<fr::RowId>(rowId)]
          = fr::XInterval{0, SITE_COUNT * input.siteWidth};
    }
    std::sort(rows_.begin(), rows_.end());
    std::sort(filler_master_ids_.begin(), filler_master_ids_.end());

    for (const PlacedInst& placed : input.placedInsts) {
      fr::PlacedInstance instance;
      instance.id = placed.instanceId;
      instance.masterId = placed.masterId;
      instance.rowId = placed.rowId;
      instance.x = placed.colId * input.siteWidth;
      instance.orientation = toPlannerOrient(placed.orientation);
      instance.isFiller = placed.isFiller;
      instances_[instance.id] = instance;
      by_row_[instance.rowId].push_back(instance);
    }
    for (auto& [rowId, instances] : by_row_) {
      (void) rowId;
      std::sort(
          instances.begin(),
          instances.end(),
          [](const fr::PlacedInstance& left, const fr::PlacedInstance& right) {
            return left.x != right.x ? left.x < right.x : left.id < right.id;
          });
    }
  }

  const std::vector<fr::RowId>& rows() const override { return rows_; }

  fr::DbCoord siteWidth() const override { return site_width_; }

  const std::vector<fr::PlacedInstance>& instancesInRow(
      fr::RowId rowId) const override
  {
    const auto found = by_row_.find(rowId);
    return found == by_row_.end() ? emptyInstances() : found->second;
  }

  const fr::PlacedInstance* instance(fr::InstanceId id) const override
  {
    const auto found = instances_.find(id);
    return found == instances_.end() ? nullptr : &found->second;
  }

  const fr::MasterInfo* masterInfo(fr::MasterId id) const override
  {
    const auto found = masters_.find(id);
    return found == masters_.end() ? nullptr : &found->second;
  }

  const std::vector<fr::MasterId>& fillerMasterIds() const override
  {
    return filler_master_ids_;
  }

  FillerCellRecord fillerCellRecord(fr::InstanceId instanceId,
                                    fr::MasterId newMasterId) const override
  {
    const fr::PlacedInstance* placed = instance(instanceId);
    if (placed == nullptr) {
      return FillerCellRecord{OpType::Replace,
                              LeafCellID(0, 0),
                              UvDist(static_cast<int64_t>(0)),
                              UvDist(static_cast<int64_t>(0)),
                              LibCellID(0, 0),
                              LibCellID(0, 0)};
    }
    return FillerCellRecord{
        OpType::Replace,
        leafCellId(placed->rowId,
                   static_cast<ColId>(placed->x / siteWidth())),
        UvDist(placed->x),
        UvDist(placed->rowId * ROW_HEIGHT),
        libCellId(placed->masterId),
        libCellId(newMasterId)};
  }

 private:
  fr::DbCoord site_width_ = 0;
  std::vector<fr::RowId> rows_;
  std::map<fr::RowId, fr::XInterval> row_spans_;
  std::map<fr::MasterId, fr::MasterInfo> masters_;
  std::map<fr::InstanceId, fr::PlacedInstance> instances_;
  std::map<fr::RowId, std::vector<fr::PlacedInstance>> by_row_;
  std::vector<fr::MasterId> filler_master_ids_;
};

class PortableCheckerOracle final : public fr::PlannerOracle
{
 public:
  PortableCheckerOracle(const PortablePlannerDataSource& view,
                        const ImplantLayerChecker& checker)
      : view_(view), checker_(checker)
  {
  }

  fr::OracleResult checkPlaceWithOverlay(
      const fr::OracleRequest& request) override
  {
    std::vector<fr::OracleResult> results = checkPlaceWithOverlays({request});
    if (results.size() == 1) {
      return std::move(results.front());
    }
    fr::OracleResult failure;
    failure.requestId = request.requestId;
    failure.status = fr::OracleStatus::CheckerError;
    failure.diagnostics.push_back(fr::makeDiag(
        fr::Severity::Fatal, "CheckerProtocolError",
        "checker result count does not match the single oracle request"));
    return failure;
  }

  std::vector<fr::OracleResult> checkPlaceWithOverlays(
      const std::vector<fr::OracleRequest>& requests) override
  {
    ++batch_count_;
    request_count_ += static_cast<int>(requests.size());
    std::vector<fr::OracleResult> results(requests.size());
    if (requests.empty())
      return results;

    const fr::OracleRequest& first = requests.front();
    std::vector<FillerChanges> changes;
    changes.reserve(requests.size());
    for (const fr::OracleRequest& request : requests) {
      changes.push_back(request.fillerChanges);
    }

    const CheckRequest target{
        first.targetPlace.instanceId,
        first.targetPlace.masterId,
        first.targetPlace.rowId,
        static_cast<ColId>(first.targetPlace.x / view_.siteWidth()),
        toCheckerOrient(first.targetPlace.orientation)};
    const ::Rect guardRect
        = makeRect(first.guardRegion.x.xl,
                   first.guardRegion.rowLo * ROW_HEIGHT,
                   first.guardRegion.x.xh,
                   (first.guardRegion.rowHi + 1) * ROW_HEIGHT - 1);
    const std::vector<CheckResult> raw
        = checker_.checkPlaceWithOverlays(target, guardRect, changes);

    if (raw.size() != requests.size()) {
      return std::vector<fr::OracleResult>(raw.size());
    }

    for (size_t index = 0; index < requests.size(); ++index) {
      fr::OracleResult& result = results[index];
      result.requestId = requests[index].requestId;
      const CheckResult& checkerResult = raw[index];
      for (const Diagnostic& diagnostic : checkerResult.diagnostics) {
        result.diagnostics.push_back(fr::makeDiag(
            fr::Severity::Warning, diagnostic.status, diagnostic.message));
      }
      for (const Violation& violation : checkerResult.violations) {
        result.violations.push_back(
            toPlannerViolation(violation, first.targetPlace.instanceId));
      }
      if (result.violations.empty() && !checkerResult.isLegal
          && !result.diagnostics.empty()) {
        result.status = fr::OracleStatus::InvalidOverlay;
        result.isLegal = false;
      } else {
        result.status = fr::OracleStatus::Checked;
        result.isLegal
            = result.violations.empty() && result.diagnostics.empty();
      }
    }
    return results;
  }

  int requestCount() const { return request_count_; }
  int batchCount() const { return batch_count_; }

 private:
  fr::Violation toPlannerViolation(const Violation& violation,
                                   fr::InstanceId targetId) const
  {
    fr::Violation result;
    result.ruleId = violation.ruleId;
    result.kind = violation.ruleSource == RuleSource::Width
                          || violation.ruleSource == RuleSource::Lef58Width
                      ? fr::ViolationKind::MinWidth
                      : fr::ViolationKind::MinSpacing;
    result.relation = violation.relationship == Relationship::InterRow
                          ? fr::ViolationRelation::InterRow
                          : fr::ViolationRelation::IntraRow;
    result.primaryLayer = violation.primaryLayer;
    result.secondaryLayer = violation.secondaryLayer;
    result.xWindow = {violation.xWindow.xl, violation.xWindow.xh};
    result.measuredValue = violation.measuredValue;
    result.requiredValue = violation.requiredValue;
    result.rowIds.assign(violation.rowIds.begin(), violation.rowIds.end());
    for (InstanceId id : violation.instances) {
      fr::ViolationParticipant participant;
      participant.instanceId = id;
      participant.isTarget = id == targetId;
      if (const fr::PlacedInstance* instance = view_.instance(id)) {
        participant.masterId = instance->masterId;
        participant.rowId = instance->rowId;
        participant.isFiller = instance->isFiller;
        const fr::MasterInfo* master = view_.masterInfo(instance->masterId);
        const fr::DbCoord width = master == nullptr ? 0 : master->width;
        participant.xRange = {instance->x, instance->x + width};
      }
      result.participants.push_back(participant);
    }
    return result;
  }

  const PortablePlannerDataSource& view_;
  const ImplantLayerChecker& checker_;
  int request_count_ = 0;
  int batch_count_ = 0;
};

fr::TargetPlace plannerTarget(RowId rowId,
                              ColId colId,
                              MasterId masterId = C1_MASTER)
{
  // Odd rows are placed MX (band polarity alternates per row).
  return fr::TargetPlace{instId(rowId, colId),
                         masterId,
                         rowId,
                         colId * SITE_WIDTH,
                         (rowId % 2) != 0 ? fr::Orient::MX : fr::Orient::R0};
}

fr::Region snapshotRegion(RowId rowId, ColId colId)
{
  return fr::Region{
      fr::XInterval{std::max<Dbu>(0, colId * SITE_WIDTH - 4 * MIN_RULE),
                    std::min<Dbu>(SITE_COUNT * SITE_WIDTH,
                                  (colId + 1) * SITE_WIDTH + 4 * MIN_RULE)},
      std::max<RowId>(0, rowId - 1),
      std::min<RowId>(ROW_COUNT - 1, rowId + 1)};
}

bool hasPlannerDiagnostic(const fr::FillerRepairResult& result,
                          const std::string& code)
{
  return std::any_of(result.diagnostics.begin(),
                     result.diagnostics.end(),
                     [&code](const fr::Diagnostic& diagnostic) {
                       return diagnostic.code == code;
                     });
}

std::string plannerDiagnostics(const fr::FillerRepairResult& result)
{
  std::ostringstream stream;
  for (const fr::Diagnostic& diagnostic : result.diagnostics) {
    stream << '\n' << diagnostic.code << ": " << diagnostic.message;
  }
  return stream.str();
}

bool sameChanges(const dpl2::ipl::FillerChanges& left,
                 const dpl2::ipl::FillerChanges& right)
{
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t index = 0; index < left.size(); ++index) {
    if (!fr::sameFillerCellRecord(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

ImplantInput multiSwapWidthInput(int requiredFillers)
{
  ImplantInput result = input();
  for (Rule& candidate : result.rules) {
    if (candidate.getRuleId() == F1_WIDTH_RULE) {
      candidate.setMinValue((requiredFillers + 1) * SITE_WIDTH);
    }
  }
  for (RowId rowId = 0; rowId <= 1; ++rowId) {
    for (ColId colId = 0; colId <= 30; ++colId) {
      PlacedInst& placed = result.placedInsts[siteIndex(rowId, colId)];
      placed.masterId = colId % 2 == 0 ? C2_MASTER : F2_FILL_MASTER;
      placed.isFiller = colId % 2 != 0;
    }
  }
  PlacedInst& target
      = result.placedInsts[siteIndex(INTRA_WIDTH_ROW, INTRA_WIDTH_COL)];
  target.masterId = C1_MASTER;
  target.isFiller = false;
  for (ColId colId : {12, 13}) {
    PlacedInst& filler = result.placedInsts[siteIndex(INTRA_WIDTH_ROW, colId)];
    filler.masterId = F2_FILL_MASTER;
    filler.isFiller = true;
  }
  return result;
}

ImplantInput thirdVtTargetInput()
{
  // Track-slot layer demands no longer exist; the F3-family target and the
  // F3-restricted allow list alone express the third-VT scenario.
  return input();
}

class PlannerCheckerFixture
{
 public:
  explicit PlannerCheckerFixture(
      ImplantInput testInput = input(),
      std::optional<std::vector<fr::MasterId>> configuredFillers = std::nullopt)
      : input_(std::move(testInput)), before_(input_.placedInsts)
  {
    helper_.initialize(input_);
    checker_ = std::make_unique<ImplantLayerChecker>(helper_.getGrid(),
                                                     helper_.getNetwork());
    helper_.initChecker(*checker_);
    view_ = std::make_unique<PortablePlannerDataSource>(
        input_, std::move(configuredFillers));
    oracle_ = std::make_unique<PortableCheckerOracle>(*view_, *checker_);
  }

  const std::vector<Diagnostic>& checkerDiagnostics() const
  {
    return checker_->getDiags();
  }

  const PortablePlannerDataSource& view() const { return *view_; }
  PortableCheckerOracle& oracle() { return *oracle_; }

  fr::OracleResult baseline(RowId rowId,
                           ColId colId,
                           MasterId targetMaster = C1_MASTER)
  {
    const fr::TargetPlace target = plannerTarget(rowId, colId, targetMaster);
    return oracle_->checkPlaceWithOverlay(
        fr::OracleRequest{0, target, snapshotRegion(rowId, colId), {}});
  }

  fr::FillerRepairResult repair(RowId rowId,
                                ColId colId,
                                const std::vector<fr::Violation>& violations,
                                fr::RepairConfig config = {},
                                MasterId targetMaster = C1_MASTER)
  {
    fr::internal::FillerRepairPlanner planner(*view_, *oracle_, config);
    return planner.repair(fr::FillerRepairRequest{
        plannerTarget(rowId, colId, targetMaster), violations});
  }

  fr::OracleResult verify(RowId rowId,
                         ColId colId,
                         const dpl2::ipl::FillerChanges& changes,
                         MasterId targetMaster = C1_MASTER)
  {
    return oracle_->checkPlaceWithOverlay(
        fr::OracleRequest{100,
                                plannerTarget(rowId, colId, targetMaster),
                                snapshotRegion(rowId, colId),
                                changes});
  }

  bool inputUnchanged() const
  {
    if (input_.placedInsts.size() != before_.size()) {
      return false;
    }
    for (size_t index = 0; index < before_.size(); ++index) {
      const PlacedInst& left = input_.placedInsts[index];
      const PlacedInst& right = before_[index];
      if (left.instanceId != right.instanceId || left.masterId != right.masterId
          || left.rowId != right.rowId || left.colId != right.colId
          || left.orientation != right.orientation
          || left.isFiller != right.isFiller) {
        return false;
      }
    }
    return true;
  }

 private:
  ImplantInput input_;
  std::vector<PlacedInst> before_;
  ImplantLayerCheckerHelper helper_;
  std::unique_ptr<ImplantLayerChecker> checker_;
  std::unique_ptr<PortablePlannerDataSource> view_;
  std::unique_ptr<PortableCheckerOracle> oracle_;
};

void expectPlannerRepairsWithFinalChecker(RowId rowId,
                                          ColId colId,
                                          const DensityCase& density)
{
  SCOPED_TRACE(densityTrace(density));
  const ImplantInput immutableInput = input(density);
  const std::vector<PlacedInst> before = immutableInput.placedInsts;
  ImplantLayerCheckerHelper helper;
  helper.initialize(immutableInput);
  ImplantLayerChecker checker(helper.getGrid(), helper.getNetwork());
  helper.initChecker(checker);
  ASSERT_TRUE(checker.getDiags().empty());

  PortablePlannerDataSource view(immutableInput);
  PortableCheckerOracle oracle(view, checker);
  const fr::TargetPlace target = plannerTarget(rowId, colId);
  const fr::Region snapshot = snapshotRegion(rowId, colId);
  fr::OracleRequest baselineRequest{0, target, snapshot, {}};
  const fr::OracleResult baseline
      = oracle.checkPlaceWithOverlay(baselineRequest);
  ASSERT_EQ(baseline.status, fr::OracleStatus::Checked);
  ASSERT_FALSE(baseline.isLegal);
  ASSERT_FALSE(baseline.violations.empty());

  fr::internal::FillerRepairPlanner planner(view, oracle);
  const fr::FillerRepairResult repaired
      = planner.repair(fr::FillerRepairRequest{target, baseline.violations});
  ASSERT_TRUE(repaired.hasSolution) << plannerDiagnostics(repaired);
  ASSERT_FALSE(repaired.changes.empty());

  fr::OracleRequest verifyRequest{1, target, snapshot, repaired.changes};
  const fr::OracleResult verified = oracle.checkPlaceWithOverlay(verifyRequest);
  EXPECT_EQ(verified.status, fr::OracleStatus::Checked);
  EXPECT_TRUE(verified.isLegal);
  EXPECT_TRUE(verified.violations.empty());

  ASSERT_EQ(immutableInput.placedInsts.size(), before.size());
  for (size_t index = 0; index < before.size(); ++index) {
    EXPECT_EQ(immutableInput.placedInsts[index].instanceId,
              before[index].instanceId);
    EXPECT_EQ(immutableInput.placedInsts[index].masterId,
              before[index].masterId);
    EXPECT_EQ(immutableInput.placedInsts[index].rowId, before[index].rowId);
    EXPECT_EQ(immutableInput.placedInsts[index].colId, before[index].colId);
  }
}

class ImplantCheckerOverlayDensityTest
    : public ::testing::TestWithParam<DensityCase>
{
};

class FillerRepairCheckerDensityE2ETest
    : public ::testing::TestWithParam<DensityCase>
{
};

std::string densityCaseName(
    const ::testing::TestParamInfo<DensityCase>& info)
{
  return info.param.name;
}

TEST_P(ImplantCheckerOverlayDensityTest,
       UsesRequestedFillerToStdCellRatioInEveryRepairWindow)
{
  const DensityCase density = GetParam();
  const ImplantInput in = input(density);
  for (const LocalDensityWindow& window : localDensityWindows()) {
    SCOPED_TRACE(window.name);
    size_t fillerCount = 0;
    size_t stdCellCount = 0;
    for (RowId rowId = window.rowLo; rowId <= window.rowHi; ++rowId) {
      for (ColId colId = window.colLo; colId < window.colHi; ++colId) {
        if (in.placedInsts[siteIndex(rowId, colId)].isFiller) {
          ++fillerCount;
        } else {
          ++stdCellCount;
        }
      }
    }
    EXPECT_EQ(fillerCount * static_cast<size_t>(density.stdCellPercent),
              stdCellCount * static_cast<size_t>(density.fillerPercent));
    for (const SiteCoord& site : window.requiredFillers) {
      EXPECT_TRUE(in.placedInsts[siteIndex(site.first, site.second)].isFiller);
    }
    for (const SiteCoord& site : window.requiredStdCells) {
      EXPECT_FALSE(in.placedInsts[siteIndex(site.first, site.second)].isFiller);
    }
  }
  for (const PlacedInst& placed : in.placedInsts) {
    ASSERT_GE(placed.masterId, 0);
    ASSERT_LT(static_cast<size_t>(placed.masterId), in.masters.size());
    EXPECT_EQ(placed.isFiller, in.masters[placed.masterId].isFiller);
  }
}

TEST_P(ImplantCheckerOverlayDensityTest, IntraRowWidth)
{
  const CheckRequest target = request(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  const std::vector<CheckResult> results
      = check(target,
              {{FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_WIDTH_ROW, 11),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_WIDTH_ROW, 11),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F2_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_WIDTH_ROW, 11),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)},
                FillerCellRecord{OpType::Replace,
                                 leafCellId(NEW_INTRA_WIDTH_ROW, 101),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F2_FILL_MASTER)}}},
              GetParam());

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].violations.empty());

  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasViolation(
      results[1], F1_WIDTH_RULE, Relationship::IntraRow, {target.instanceId}));

  EXPECT_FALSE(results[2].isLegal);
  EXPECT_FALSE(hasViolation(
      results[2], F1_WIDTH_RULE, Relationship::IntraRow, {target.instanceId}));
  EXPECT_TRUE(hasViolation(results[2],
                           F1_WIDTH_RULE,
                           Relationship::IntraRow,
                           {instId(NEW_INTRA_WIDTH_ROW, NEW_INTRA_WIDTH_COL)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST_P(ImplantCheckerOverlayDensityTest, InterRowWidth)
{
  const CheckRequest target = request(INTER_WIDTH_TARGET_ROW, INTER_WIDTH_COL);
  const InstanceId neighbor = instId(INTER_WIDTH_NEIGHBOR_ROW, INTER_WIDTH_COL);
  const std::vector<CheckResult> results
      = check(target,
              {{FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_WIDTH_NEIGHBOR_ROW, 31),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_WIDTH_NEIGHBOR_ROW, 31),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F2_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_WIDTH_NEIGHBOR_ROW, 31),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)},
                FillerCellRecord{
                    OpType::Replace,
                    leafCellId(NEW_INTER_WIDTH_BOTTOM_ROW, NEW_INTER_WIDTH_COL),
                    UvDist(0),
                    UvDist(0),
                    LibCellID(),
                    libCellId(F1_FILL_MASTER)}}},
              GetParam());

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
  EXPECT_TRUE(
      hasViolation(results[2],
                   interRule(F1_WIDTH_RULE, NEW_INTER_WIDTH_TOP_ROW),
                   Relationship::InterRow,
                   {instId(NEW_INTER_WIDTH_TOP_ROW, NEW_INTER_WIDTH_COL),
                    instId(NEW_INTER_WIDTH_BOTTOM_ROW, NEW_INTER_WIDTH_COL)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST_P(ImplantCheckerOverlayDensityTest, IntraRowSpacing)
{
  const CheckRequest target = request(INTRA_SPACING_ROW, INTRA_SPACING_COL);
  const InstanceId neighbor = instId(INTRA_SPACING_ROW, INTRA_SPACING_COL + 3);
  const std::vector<CheckResult> results
      = check(target,
              {{FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_SPACING_ROW, 52),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_SPACING_ROW, 52),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F2_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_SPACING_ROW, 52),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)},
                FillerCellRecord{OpType::Replace,
                                 leafCellId(NEW_INTRA_SPACING_ROW, 143),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}}},
              GetParam());

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
  EXPECT_TRUE(
      hasViolation(results[2],
                   F1_SPACING_RULE,
                   Relationship::IntraRow,
                   {instId(NEW_INTRA_SPACING_ROW, NEW_INTRA_SPACING_COL),
                    instId(NEW_INTRA_SPACING_ROW, NEW_INTRA_SPACING_COL + 4)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST(ImplantCheckerOverlayTest,
     TargetViolationIsDetectedWhenChangedNeighborIsOutsideGuard)
{
  const ImplantInput in = input();
  ImplantLayerCheckerHelper helper;
  helper.initialize(in);
  ImplantLayerChecker checker(helper.getGrid(), helper.getNetwork());
  helper.initChecker(checker);
  ASSERT_TRUE(checker.getDiags().empty());

  const CheckRequest target = request(INTRA_SPACING_ROW, INTRA_SPACING_COL);
  const Dbu targetX = INTRA_SPACING_COL * SITE_WIDTH;
  const Dbu targetY = INTRA_SPACING_ROW * ROW_HEIGHT;
  const Rect targetOnlyGuard = makeRect(
      targetX, targetY, targetX + SITE_WIDTH, targetY + ROW_HEIGHT);
  const FillerChanges unchangedOutsideGuard{
      FillerCellRecord{OpType::Replace,
                       leafCellId(INTRA_SPACING_ROW, 52),
                       UvDist(0),
                       UvDist(0),
                       LibCellID(),
                       libCellId(F2_FILL_MASTER)}};
  const std::vector<CheckResult> results = checker.checkPlaceWithOverlays(
      target, targetOnlyGuard, {unchangedOutsideGuard});

  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results.front().isLegal);
  EXPECT_TRUE(hasViolation(results.front(),
                           F1_SPACING_RULE,
                           Relationship::IntraRow,
                           {target.instanceId,
                            instId(INTRA_SPACING_ROW,
                                   INTRA_SPACING_COL + 3)}));
}

TEST_P(ImplantCheckerOverlayDensityTest, InterRowSpacing)
{
  const CheckRequest target
      = request(INTER_SPACING_TARGET_ROW, INTER_SPACING_COL);
  const InstanceId neighbor
      = instId(INTER_SPACING_NEIGHBOR_ROW, INTER_SPACING_COL + 3);
  const std::vector<CheckResult> results
      = check(target,
              {{FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_SPACING_NEIGHBOR_ROW, 72),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_SPACING_NEIGHBOR_ROW, 72),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F2_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_SPACING_NEIGHBOR_ROW, 72),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)},
                FillerCellRecord{OpType::Replace,
                                 leafCellId(NEW_INTER_SPACING_BOTTOM_ROW, 163),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}}},
              GetParam());

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].violations.empty());

  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasViolation(results[1],
                           interRule(F1_SPACING_RULE, INTER_SPACING_TARGET_ROW),
                           Relationship::InterRow,
                           {target.instanceId, neighbor}));

  EXPECT_FALSE(results[2].isLegal);
  EXPECT_FALSE(hasViolation(results[2],
                            interRule(F1_SPACING_RULE, INTER_SPACING_TARGET_ROW),
                            Relationship::InterRow,
                            {target.instanceId, neighbor}));
  EXPECT_TRUE(hasViolation(
      results[2],
      interRule(F1_SPACING_RULE, NEW_INTER_SPACING_TOP_ROW),
      Relationship::InterRow,
      {instId(NEW_INTER_SPACING_TOP_ROW, NEW_INTER_SPACING_COL),
       instId(NEW_INTER_SPACING_BOTTOM_ROW, NEW_INTER_SPACING_COL + 4)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST_P(FillerRepairCheckerDensityE2ETest, RepairsIntraRowWidth)
{
  const RowId rowId = INTRA_WIDTH_ROW;
  const ColId colId = INTRA_WIDTH_COL;
  ASSERT_EQ(instId(rowId, colId), instId(INTRA_WIDTH_ROW, INTRA_WIDTH_COL));
  expectPlannerRepairsWithFinalChecker(rowId, colId, GetParam());
  SUCCEED();
}

TEST_P(FillerRepairCheckerDensityE2ETest, RepairsInterRowWidth)
{
  const RowId rowId = INTER_WIDTH_TARGET_ROW;
  const ColId colId = INTER_WIDTH_COL;
  ASSERT_EQ(instId(rowId, colId),
            instId(INTER_WIDTH_TARGET_ROW, INTER_WIDTH_COL));
  expectPlannerRepairsWithFinalChecker(rowId, colId, GetParam());
  SUCCEED();
}

TEST_P(FillerRepairCheckerDensityE2ETest, RepairsIntraRowSpacing)
{
  const RowId rowId = INTRA_SPACING_ROW;
  const ColId colId = INTRA_SPACING_COL;
  ASSERT_EQ(instId(rowId, colId), instId(INTRA_SPACING_ROW, INTRA_SPACING_COL));
  expectPlannerRepairsWithFinalChecker(rowId, colId, GetParam());
  SUCCEED();
}

TEST_P(FillerRepairCheckerDensityE2ETest, RepairsInterRowSpacing)
{
  const RowId rowId = INTER_SPACING_TARGET_ROW;
  const ColId colId = INTER_SPACING_COL;
  ASSERT_EQ(instId(rowId, colId),
            instId(INTER_SPACING_TARGET_ROW, INTER_SPACING_COL));
  expectPlannerRepairsWithFinalChecker(rowId, colId, GetParam());
  SUCCEED();
}

INSTANTIATE_TEST_SUITE_P(
    FillerStdRatios,
    ImplantCheckerOverlayDensityTest,
    ::testing::Values(FILLER_50_STD_50,
                      FILLER_30_STD_70,
                      FILLER_20_STD_80,
                      FILLER_10_STD_90,
                      FILLER_5_STD_95),
    densityCaseName);

INSTANTIATE_TEST_SUITE_P(
    FillerStdRatios,
    FillerRepairCheckerDensityE2ETest,
    ::testing::Values(FILLER_50_STD_50,
                      FILLER_30_STD_70,
                      FILLER_20_STD_80,
                      FILLER_10_STD_90,
                      FILLER_5_STD_95),
    densityCaseName);

TEST(FillerRepairCheckerE2ETest, CleanSnapshotReturnsEmptyRepair)
{
  PlannerCheckerFixture fixture;
  ASSERT_TRUE(fixture.checkerDiagnostics().empty());
  const fr::OracleResult clean = fixture.baseline(6, 20);
  ASSERT_EQ(clean.status, fr::OracleStatus::Checked);
  ASSERT_TRUE(clean.isLegal);
  ASSERT_TRUE(clean.violations.empty());
  const int beforeRequests = fixture.oracle().requestCount();
  const fr::FillerRepairResult result = fixture.repair(6, 20, {});
  EXPECT_TRUE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_EQ(fixture.oracle().requestCount(), beforeRequests);
  EXPECT_TRUE(fixture.inputUnchanged());
}

TEST(FillerRepairCheckerE2ETest, RepeatedRepairIsDeterministic)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline
      = fixture.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  ASSERT_FALSE(baseline.violations.empty());
  const fr::FillerRepairResult first
      = fixture.repair(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, baseline.violations);
  const fr::FillerRepairResult second
      = fixture.repair(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, baseline.violations);
  ASSERT_TRUE(first.hasSolution);
  ASSERT_TRUE(second.hasSolution);
  EXPECT_TRUE(sameChanges(first.changes, second.changes));
  EXPECT_TRUE(fixture.inputUnchanged());
}

TEST(FillerRepairCheckerE2ETest, BatchSizeOneStillFindsSameRepair)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline
      = fixture.baseline(INTER_WIDTH_TARGET_ROW, INTER_WIDTH_COL);
  ASSERT_FALSE(baseline.violations.empty());
  fr::RepairConfig config;
  config.batchSize = 1;
  const fr::FillerRepairResult result = fixture.repair(
      INTER_WIDTH_TARGET_ROW, INTER_WIDTH_COL, baseline.violations, config);
  ASSERT_TRUE(result.hasSolution) << plannerDiagnostics(result);
  ASSERT_FALSE(result.changes.empty());
  const fr::OracleResult verified
      = fixture.verify(INTER_WIDTH_TARGET_ROW, INTER_WIDTH_COL, result.changes);
  EXPECT_TRUE(verified.isLegal);
  EXPECT_GT(fixture.oracle().batchCount(), 1);
}

TEST(FillerRepairCheckerE2ETest, BatchSizeDoesNotChangeChosenOverlay)
{
  PlannerCheckerFixture smallBatch;
  PlannerCheckerFixture largeBatch;
  const fr::OracleResult smallBaseline
      = smallBatch.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  const fr::OracleResult largeBaseline
      = largeBatch.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  fr::RepairConfig smallConfig;
  smallConfig.batchSize = 1;
  fr::RepairConfig largeConfig;
  largeConfig.batchSize = 64;
  const fr::FillerRepairResult small = smallBatch.repair(
      INTRA_WIDTH_ROW, INTRA_WIDTH_COL, smallBaseline.violations, smallConfig);
  const fr::FillerRepairResult large = largeBatch.repair(
      INTRA_WIDTH_ROW, INTRA_WIDTH_COL, largeBaseline.violations, largeConfig);
  ASSERT_TRUE(small.hasSolution);
  ASSERT_TRUE(large.hasSolution);
  EXPECT_TRUE(sameChanges(small.changes, large.changes));
}

TEST(FillerRepairCheckerE2ETest, EmptyCandidateUniverseFailsWithoutPartial)
{
  PlannerCheckerFixture fixture(input(), std::vector<fr::MasterId>{});
  const fr::OracleResult baseline
      = fixture.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  ASSERT_FALSE(baseline.violations.empty());
  fr::RepairConfig config;
  config.adaptiveStepFillers = 100;
  config.checkerCallBudgetPerWindow = 32;
  const fr::FillerRepairResult result = fixture.repair(
      INTRA_WIDTH_ROW, INTRA_WIDTH_COL, baseline.violations, config);
  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_TRUE(hasPlannerDiagnostic(result, "NoSwapGenerated")
              || hasPlannerDiagnostic(result, "NoEditableFiller"));
  EXPECT_TRUE(fixture.inputUnchanged());
}

TEST(FillerRepairCheckerE2ETest, ThirdVtOnlyCandidateRemainsReachable)
{
  PlannerCheckerFixture fixture(thirdVtTargetInput(),
                                std::vector<fr::MasterId>{F3_FILL_MASTER});
  const fr::OracleResult baseline
      = fixture.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, C3_MASTER);
  ASSERT_FALSE(baseline.violations.empty());
  const fr::FillerRepairResult result = fixture.repair(
      INTRA_WIDTH_ROW, INTRA_WIDTH_COL, baseline.violations, {}, C3_MASTER);
  ASSERT_TRUE(result.hasSolution);
  ASSERT_FALSE(result.changes.empty());
  EXPECT_TRUE(std::all_of(result.changes.begin(),
                          result.changes.end(),
                          [](const dpl2::FillerCellRecord& change) {
                            return fr::fillerRecordNewMasterId(change)
                                   == F3_FILL_MASTER;
                          }));
  EXPECT_TRUE(
      fixture
          .verify(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, result.changes, C3_MASTER)
          .isLegal);
}

TEST(FillerRepairCheckerE2ETest, OneCallBudgetReturnsNoPartialRepair)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline
      = fixture.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  ASSERT_FALSE(baseline.violations.empty());
  fr::RepairConfig config;
  config.checkerCallBudgetPerWindow = 1;
  config.batchSize = 1;
  config.adaptiveStepFillers = 100;
  const fr::FillerRepairResult result = fixture.repair(
      INTRA_WIDTH_ROW, INTRA_WIDTH_COL, baseline.violations, config);
  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_TRUE(fixture.inputUnchanged());
}

TEST(FillerRepairCheckerE2ETest, FabricatedOriginalFailsBaselineGate)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline
      = fixture.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  ASSERT_FALSE(baseline.violations.empty());
  std::vector<fr::Violation> stale = baseline.violations;
  stale.front().ruleId += 10000;
  const fr::FillerRepairResult result
      = fixture.repair(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, stale);
  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_TRUE(hasPlannerDiagnostic(result, "BaselineMismatch"));
}

TEST(FillerRepairCheckerE2ETest, DuplicateOriginalFailsOneToOneBaselineGate)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline
      = fixture.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  ASSERT_FALSE(baseline.violations.empty());
  std::vector<fr::Violation> duplicated = baseline.violations;
  duplicated.push_back(duplicated.front());
  const fr::FillerRepairResult result
      = fixture.repair(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, duplicated);
  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_TRUE(hasPlannerDiagnostic(result, "BaselineMismatch"));
}

TEST(FillerRepairCheckerE2ETest, ReturnedChangesTouchOnlySameSizeFillers)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline
      = fixture.baseline(INTRA_SPACING_ROW, INTRA_SPACING_COL);
  const fr::FillerRepairResult result = fixture.repair(
      INTRA_SPACING_ROW, INTRA_SPACING_COL, baseline.violations);
  ASSERT_TRUE(result.hasSolution) << plannerDiagnostics(result);
  ASSERT_FALSE(result.changes.empty());
  for (const dpl2::FillerCellRecord& change : result.changes) {
    const fr::PlacedInstance* instance
        = fixture.view().instance(fr::fillerRecordInstanceId(change));
    ASSERT_NE(instance, nullptr);
    EXPECT_TRUE(instance->isFiller);
    const fr::MasterInfo* oldMaster
        = fixture.view().masterInfo(instance->masterId);
    const fr::MasterInfo* newMaster
        = fixture.view().masterInfo(fr::fillerRecordNewMasterId(change));
    ASSERT_NE(oldMaster, nullptr);
    ASSERT_NE(newMaster, nullptr);
    EXPECT_EQ(oldMaster->width, newMaster->width);
    EXPECT_EQ(oldMaster->height, newMaster->height);
  }
  EXPECT_TRUE(fixture.inputUnchanged());
}

TEST(FillerRepairCheckerE2ETest, PlannerAvoidsKnownNewViolationSites)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline
      = fixture.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  const fr::FillerRepairResult result
      = fixture.repair(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, baseline.violations);
  ASSERT_TRUE(result.hasSolution);
  const std::array<InstanceId, 4> unrelatedSites{
      instId(NEW_INTRA_WIDTH_ROW, 101),
      instId(NEW_INTER_WIDTH_BOTTOM_ROW, NEW_INTER_WIDTH_COL),
      instId(NEW_INTRA_SPACING_ROW, 143),
      instId(NEW_INTER_SPACING_BOTTOM_ROW, 163)};
  for (const dpl2::FillerCellRecord& change : result.changes) {
    EXPECT_EQ(
        std::find(
            unrelatedSites.begin(),
            unrelatedSites.end(),
            fr::fillerRecordInstanceId(change)),
        unrelatedSites.end());
  }
  EXPECT_TRUE(
      fixture.verify(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, result.changes).isLegal);
}

TEST(FillerRepairCheckerE2ETest, MinimumWidthCanRequireTwoAtomicSwaps)
{
  PlannerCheckerFixture fixture(multiSwapWidthInput(2),
                                std::vector<fr::MasterId>{F1_FILL_MASTER});
  const fr::OracleResult baseline
      = fixture.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  ASSERT_FALSE(baseline.violations.empty());
  fr::RepairConfig config;
  config.checkerCallBudgetPerWindow = 4096;
  config.memberCapSize3 = 100;
  config.batchSize = 64;
  const fr::FillerRepairResult result = fixture.repair(
      INTRA_WIDTH_ROW, INTRA_WIDTH_COL, baseline.violations, config);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(result.changes.size(), 2u);
  const fr::OracleResult verified
      = fixture.verify(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, result.changes);
  EXPECT_TRUE(verified.isLegal);
  EXPECT_TRUE(verified.violations.empty());
}

TEST(FillerRepairCheckerE2ETest,
     ThreeSwapSolutionSurvivesAdaptiveDirectionFallback)
{
  PlannerCheckerFixture fixture(multiSwapWidthInput(3),
                                std::vector<fr::MasterId>{F1_FILL_MASTER});
  const dpl2::ipl::FillerChanges expected{
      fixture.view().fillerCellRecord(instId(INTRA_WIDTH_ROW, 9),
                                      F1_FILL_MASTER),
      fixture.view().fillerCellRecord(instId(INTRA_WIDTH_ROW, 11),
                                      F1_FILL_MASTER),
      fixture.view().fillerCellRecord(instId(INTRA_WIDTH_ROW, 12),
                                      F1_FILL_MASTER)};
  ASSERT_TRUE(
      fixture.verify(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, expected).isLegal);
  const fr::OracleResult baseline
      = fixture.baseline(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  ASSERT_FALSE(baseline.violations.empty());
  const fr::FillerRepairResult result
      = fixture.repair(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, baseline.violations);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_TRUE(sameChanges(result.changes, expected));
  EXPECT_TRUE(
      fixture.verify(INTRA_WIDTH_ROW, INTRA_WIDTH_COL, result.changes).isLegal);
  EXPECT_TRUE(fixture.inputUnchanged());
}


}  // namespace
}  // namespace ipl
}  // namespace dpl2
