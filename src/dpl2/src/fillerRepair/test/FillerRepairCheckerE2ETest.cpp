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

#include <drc/ImplantLayerChecker.h>
#include <drc/ImplantLayerCheckerHelper.h>
#include <fillerRepair/RepairPlanner.h>
#include <infrastructure/Grid.h>
#include <infrastructure/network.h>

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

bool hasDiagnostic(const std::vector<Diagnostic>& diagnostics,
                   const std::string& status)
{
  return std::any_of(
      diagnostics.begin(),
      diagnostics.end(),
      [&](const Diagnostic& diagnostic) {
        return diagnostic.status == status;
      });
}

TEST(ImplantLayerCheckerInitializationTest,
     MissingGridNetworkOrManagerFailsClosed)
{
  Grid grid;
  Network network;
  CheckRequest request;

  ImplantLayerChecker missingGrid(nullptr, &network);
  EXPECT_TRUE(hasDiagnostic(missingGrid.getDiags(), "missing_grid"));
  EXPECT_FALSE(missingGrid.checkDirect(request).isLegal);

  ImplantLayerChecker missingNetwork(&grid, nullptr);
  EXPECT_TRUE(hasDiagnostic(missingNetwork.getDiags(), "missing_network"));
  EXPECT_FALSE(missingNetwork.checkDirect(request).isLegal);

  ImplantLayerChecker missingManager(&grid, &network);
  EXPECT_TRUE(hasDiagnostic(missingManager.getDiags(),
                            "missing_grid_phys_des_mgr"));
  const CheckResult result = missingManager.checkDirect(request);
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics,
                            "missing_grid_phys_des_mgr"));
}

constexpr Dbu SITE_WIDTH = 10;
constexpr Dbu ROW_HEIGHT = 100;
constexpr RowId ROW_COUNT = 7;
// Middle row: three rows of context above and below every target.
constexpr RowId TARGET_ROW = 3;
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

// --- Scenarios ------------------------------------------------------------
//
// The placed layout is LEGAL as built: columns run in same-VT pairs
// (std cell + filler), so every implant run is exactly MIN_RULE wide and
// two runs of one VT family sit four sites apart. Nothing is planted.
//
// A scenario is what opto actually does: take one std cell and give it a
// different VT. Changing the cell at an even column to the VT of the pair on
// its RIGHT isolates it -- its own run collapses to one site (below min
// width) and it lands one site away from that neighbouring run (below min
// spacing), on the N band and on its P partner, intra-row and across both
// row boundaries. One realistic edit, ten violations, none of them
// hand-placed.
//
// The natural repair is the filler between the two runs -- the BRIDGE:
// recolour it to the new VT and the runs merge into one four-site run.
//
// Every scenario runs on a design of ROW_COUNT (7) rows with its target on an
// interior row, so each case carries real rows of context above and below --
// enough for the guard (window +/- two rows) and for an adaptive step before
// it clamps.
struct Scenario
{
  const char* name;
  RowId row;
  ColId col;          // even column: a std cell site
  MasterId newMaster;  // the VT opto retargets it to
  ColId bridgeCol;     // the one filler whose recolour merges the two runs
  ColId windowLo;      // local-density window: 20 columns from here
};

// (col/2)%3 is a site's VT family, so the pair to the right of column c is
// family ((c/2)+1)%3 -- that is the master that isolates the target.
constexpr MasterId rightNeighborCellMaster(ColId col)
{
  return static_cast<MasterId>(C1_MASTER + (((col / 2) + 1) % 3));
}

// Family index of a cell/filler master (C1/F1 -> 0, C2/F2 -> 1, ...).
constexpr int familyOf(MasterId masterId)
{
  return masterId >= F1_FILL_MASTER ? masterId - F1_FILL_MASTER
                                    : masterId - C1_MASTER;
}

constexpr MasterId fillerOfFamily(int family)
{
  return static_cast<MasterId>(F1_FILL_MASTER + family);
}

// N-band rule ids per family; the P partner is the same rule + P_RULE_OFFSET.
constexpr int widthRule(int family)
{
  return F1_WIDTH_RULE + family;
}

constexpr int spacingRule(int family)
{
  return F1_SPACING_RULE + family;
}

constexpr Scenario scenario(const char* name,
                            RowId row,
                            ColId col,
                            ColId windowLo)
{
  return Scenario{name,
                  row,
                  col,
                  rightNeighborCellMaster(col),
                  static_cast<ColId>(col + 1),
                  windowLo};
}

// Four positions, all on a 7-row design: an odd (MX) row and an even (R0)
// row, one target near the left edge of the core and one far along the row.
constexpr Scenario SCN_MID = scenario("mid-row", TARGET_ROW, 50, 42);
constexpr Scenario SCN_EVEN_ROW
    = scenario("even-row", static_cast<RowId>(TARGET_ROW - 1), 70, 62);
constexpr Scenario SCN_LEFT_EDGE = scenario("near-left-edge", TARGET_ROW, 4, 0);
constexpr Scenario SCN_FAR = scenario("far-column", TARGET_ROW, 150, 142);

constexpr std::array<Scenario, 4> SCENARIOS{
    SCN_MID, SCN_EVEN_ROW, SCN_LEFT_EDGE, SCN_FAR};

// The bridge filler recoloured to the target's new VT: the repair.
constexpr MasterId bridgeRepairMaster(const Scenario& scn)
{
  return fillerOfFamily(familyOf(scn.newMaster));
}

// A third VT on the bridge: a plausible-looking swap that fixes nothing.
constexpr MasterId bridgeWrongMaster(const Scenario& scn)
{
  return fillerOfFamily((familyOf(scn.newMaster) + 1) % 3);
}

// A pre-existing, unrelated violation far from every scenario, planted the
// same way a real one arrives: one std cell left carrying the wrong VT, with
// no bridge next to it. It must never surface on another target's check.
constexpr RowId OLD_UNRELATED_ROW = 0;
constexpr ColId OLD_UNRELATED_COL = 190;
constexpr MasterId OLD_UNRELATED_MASTER
    = rightNeighborCellMaster(OLD_UNRELATED_COL);

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
  return R"(Schematic: 7 rows x 200 sites, 100% occupied, LEGAL as built.
  Legend: [ 1F1 ] = std cell on the F1 VT family.
          [ aF1 ] = filler on the F1 VT family.
        1/2/3 are std cells on F1/F2/F3; a/b/c are F1/F2/F3 fillers.
        ^ marks the retargeted std cell, * the bridge filler that repairs it.
        Rows alternate R0 / MX, so a master's N band faces N across an
        odd->even row boundary and P across an even->odd one.

Background: same-VT pairs marching along the row, so every implant run is
exactly MIN_RULE (two sites) wide and the next run of that family starts four
sites later.
Sites:    ... [ 1F1 ][ aF1 ][ 2F2 ][ bF2 ][ 3F3 ][ cF3 ][ 1F1 ][ aF1 ] ...

One scenario, at the mid-row target (row 3, sites 46..55). Opto retargets the
std cell at site 50 from F2 to F3 -- the VT of the pair on its right:
Sites:       46      47      48      49      50      51      52      53
built:    [ 3F3 ][ cF3 ][ 1F1 ][ aF1 ][ 2F2 ][ bF2 ][ 3F3 ][ cF3 ]
                                        ^
retargeted:                           [ 3F3 ][ bF2 ][ 3F3 ][ cF3 ]
  F3 runs are now [500,510) and [520,540): the first is one site wide
  (min width) and the gap between them is one site (min spacing). Both fire
  on the F3 N band and on its P partner, intra-row and across both row
  boundaries -- ten violations from one edit.
repaired:                             [ 3F3 ][ cF3*][ 3F3 ][ cF3 ]
  one bridge swap at site 51 merges them into [500,540).

The other three scenarios are the same edit at (row 2, site 70), (row 3,
site 4) near the left edge, and (row 3, site 150).

Local density: each scenario owns a 20-column window across its target row
+/-1 in which the filler:stdCell ratio follows the test parameter. Only the
FILLER IDENTITY of a site changes -- its implant geometry does not -- so the
violations above are identical at every ratio and only the size of the
editable universe moves.

Pre-existing unrelated violation, planted at row 0 site 190 the same way
(a retargeted std cell with no bridge). It is in the guard of a whole-design
check and must never be reported for another target.
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

// Flips a site between filler and std cell WITHOUT touching its implant
// geometry: same VT family, same shapes, same width. That is what lets the
// density matrix vary the editable universe while every case sees exactly the
// same violations.
void setFillerIdentity(SiteSpec& site, bool isFiller)
{
  const int family = familyOf(site.masterId);
  site.masterId = isFiller ? fillerMaster(family) : cellMaster(family);
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

// One 20-column, 3-row window per scenario, centred on its target row. The
// bridge filler must stay a filler at every ratio (it is the repair), and the
// target must stay a std cell (it is what opto retargets); everything else in
// the window is free to take either identity.
const std::array<LocalDensityWindow, SCENARIOS.size()>& localDensityWindows()
{
  static const std::array<LocalDensityWindow, SCENARIOS.size()> windows = [] {
    std::array<LocalDensityWindow, SCENARIOS.size()> built{};
    for (size_t index = 0; index < SCENARIOS.size(); ++index) {
      const Scenario& scn = SCENARIOS[index];
      built[index] = LocalDensityWindow{
          scn.name,
          static_cast<RowId>(scn.row - 1),
          static_cast<RowId>(scn.row + 1),
          scn.windowLo,
          static_cast<ColId>(scn.windowLo + 20),
          {{scn.row, scn.bridgeCol}},
          {{scn.row, scn.col}}};
    }
    return built;
  }();
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

  applyLocalDensities(sites, density);

  // The one planted defect: a std cell far from every scenario left carrying
  // the wrong VT, with no bridge beside it. Pre-existing and unrelated, so a
  // check on any other target must filter it out (see
  // expectOldUnrelatedFiltered).
  setCell(sites, OLD_UNRELATED_ROW, OLD_UNRELATED_COL, OLD_UNRELATED_MASTER);

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

// The check opto issues: same instance, same site, same orientation, the new
// master it wants to place there.
CheckRequest request(RowId rowId, ColId colId, MasterId masterId)
{
  return CheckRequest{instId(rowId, colId),
                      masterId,
                      rowId,
                      colId,
                      (rowId % 2) != 0 ? PhysOrientationE::MX
                                       : PhysOrientationE::R0};
}

CheckRequest retargeted(const Scenario& scn)
{
  return request(scn.row, scn.col, scn.newMaster);
}

LeafCellID leafCellId(RowId rowId, ColId colId)
{
  return LeafCellID(0, instId(rowId, colId));
}

LibCellID libCellId(MasterId masterId)
{
  return LibCellID(0, masterId);
}

// One candidate: recolour the scenario's bridge filler to `masterId`.
FillerChanges bridgeSwap(const Scenario& scn, MasterId masterId)
{
  return FillerChanges{CellChangeRecord{OpType::Replace,
                                        CellData{leafCellId(
                                            scn.row, scn.bridgeCol)},
                                        UvDist(0),
                                        UvDist(0),
                                        LibCellID(),
                                        libCellId(masterId),
                                        PhysOrientation(
                                            (scn.row % 2) != 0
                                                ? PhysOrientationE::MX
                                                : PhysOrientationE::R0)}};
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

// Any violation of `ruleId`/`relationship`, whoever the participants are.
bool hasRuleViolation(const CheckResult& result,
                      int ruleId,
                      Relationship relationship)
{
  return hasViolation(result, ruleId, relationship, {});
}

// The planted defect at OLD_UNRELATED sits inside a whole-design guard, so a
// check on any other target proves the checker suppresses pre-existing
// findings that do not touch it.
void expectOldUnrelatedFiltered(const CheckResult& result)
{
  for (const Violation& violation : result.violations) {
    EXPECT_EQ(std::find(violation.instances.begin(),
                        violation.instances.end(),
                        instId(OLD_UNRELATED_ROW, OLD_UNRELATED_COL)),
              violation.instances.end())
        << "the pre-existing unrelated violation was reported";
  }
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
  EXPECT_FALSE(checker.isFillerRepairEnabled());
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
class PortablePlacementView final : public fr::PlacementView
{
 public:
  explicit PortablePlacementView(
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

  CellChangeRecord cellChangeRecord(fr::InstanceId instanceId,
                                    fr::MasterId newMasterId) const override
  {
    const fr::PlacedInstance* placed = instance(instanceId);
    if (placed == nullptr) {
      return CellChangeRecord{OpType::Replace,
                              CellData{LeafCellID(0, 0)},
                              UvDist(static_cast<int64_t>(0)),
                              UvDist(static_cast<int64_t>(0)),
                              LibCellID(0, 0),
                              LibCellID(0, 0),
                              PhysOrientation(PhysOrientationE::R0)};
    }
    return CellChangeRecord{
        OpType::Replace,
        CellData{leafCellId(
            placed->rowId, static_cast<ColId>(placed->x / siteWidth()))},
        UvDist(placed->x),
        UvDist(placed->rowId * ROW_HEIGHT),
        libCellId(placed->masterId),
        libCellId(newMasterId),
        toCheckerOrient(placed->orientation)};
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

class PortableCheckerOracle final : public fr::RepairOracle
{
 public:
  PortableCheckerOracle(const PortablePlacementView& view,
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

  const PortablePlacementView& view_;
  const ImplantLayerChecker& checker_;
  int request_count_ = 0;
  int batch_count_ = 0;
};

// The planner-side form of `retargeted()`: same instance, same site, same
// orientation, the master opto wants there.
fr::TargetPlace plannerTarget(const Scenario& scn)
{
  // Odd rows are placed MX (band polarity alternates per row).
  return fr::TargetPlace{
      instId(scn.row, scn.col),
      scn.newMaster,
      scn.row,
      scn.col * SITE_WIDTH,
      (scn.row % 2) != 0 ? fr::Orient::MX : fr::Orient::R0};
}

fr::Region snapshotRegion(const Scenario& scn)
{
  return fr::Region{
      fr::XInterval{
          std::max<Dbu>(0, scn.col * SITE_WIDTH - 4 * MIN_RULE),
          std::min<Dbu>(SITE_COUNT * SITE_WIDTH,
                        (scn.col + 1) * SITE_WIDTH + 4 * MIN_RULE)},
      std::max<RowId>(0, scn.row - 1),
      std::min<RowId>(ROW_COUNT - 1, scn.row + 1)};
}

fr::InstanceId bridgeInstance(const Scenario& scn)
{
  return instId(scn.row, scn.bridgeCol);
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
    if (!fr::sameCellChangeRecord(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

// A realistic wide-implant scenario: one VT family whose minimum width spans
// several sites, which real technologies do have. The band around the target
// carries no cell of that family, so the retargeted cell has to grow its own
// run by recolouring the fillers beside it -- one swap can never be enough.
//
// The band is uniform F2 across every row and thirty columns wide, so the
// raised rule cannot reach any other F1 run from the target's window, and the
// sites flanking the target are fillers so a run of any width can be formed
// at all. The target site itself stays a placed F2 std cell: the F1 master
// arrives only in the CheckRequest, exactly as opto issues it.
constexpr ColId WIDE_RULE_COL = 100;
constexpr ColId WIDE_RULE_BAND_LO = 90;
constexpr ColId WIDE_RULE_BAND_HI = 120;
constexpr Scenario SCN_WIDE_RULE{"wide-rule",
                                 TARGET_ROW,
                                 WIDE_RULE_COL,
                                 C1_MASTER,
                                 static_cast<ColId>(WIDE_RULE_COL + 1),
                                 WIDE_RULE_BAND_LO};

ImplantInput multiSwapWidthInput(int requiredFillers)
{
  ImplantInput result = input();
  const Dbu required = (requiredFillers + 1) * SITE_WIDTH;
  for (Rule& candidate : result.rules) {
    if (candidate.getRuleId() == widthRule(familyOf(C1_MASTER))
        || candidate.getRuleId()
               == widthRule(familyOf(C1_MASTER)) + P_RULE_OFFSET) {
      candidate.setMinValue(required);
    }
  }
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    for (ColId colId = WIDE_RULE_BAND_LO; colId < WIDE_RULE_BAND_HI; ++colId) {
      PlacedInst& placed = result.placedInsts[siteIndex(rowId, colId)];
      const bool isFiller = colId % 2 != 0;
      placed.masterId = isFiller ? F2_FILL_MASTER : C2_MASTER;
      placed.isFiller = isFiller;
    }
  }
  for (ColId colId = WIDE_RULE_COL - 3; colId <= WIDE_RULE_COL + 3; ++colId) {
    if (colId == WIDE_RULE_COL) {
      continue;
    }
    PlacedInst& filler = result.placedInsts[siteIndex(SCN_WIDE_RULE.row, colId)];
    filler.masterId = F2_FILL_MASTER;
    filler.isFiller = true;
  }
  return result;
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
    view_ = std::make_unique<PortablePlacementView>(
        input_, std::move(configuredFillers));
    oracle_ = std::make_unique<PortableCheckerOracle>(*view_, *checker_);
  }

  const std::vector<Diagnostic>& checkerDiagnostics() const
  {
    return checker_->getDiags();
  }

  const PortablePlacementView& view() const { return *view_; }
  PortableCheckerOracle& oracle() { return *oracle_; }

  const ImplantLayerChecker& checker() const { return *checker_; }
  ImplantLayerChecker& mutableChecker() { return *checker_; }
  Grid* grid() { return helper_.getGrid(); }
  Network* network() { return helper_.getNetwork(); }

  fr::OracleResult baseline(const Scenario& scn)
  {
    return oracle_->checkPlaceWithOverlay(
        fr::OracleRequest{0, plannerTarget(scn), snapshotRegion(scn), {}});
  }

  fr::FillerRepairResult repair(const Scenario& scn,
                                const std::vector<fr::Violation>& violations,
                                fr::RepairConfig config = {})
  {
    fr::internal::RepairPlanner planner(*view_, *oracle_, config);
    return planner.repair(
        fr::FillerRepairRequest{plannerTarget(scn), violations});
  }

  fr::OracleResult verify(const Scenario& scn,
                          const dpl2::ipl::FillerChanges& changes)
  {
    return oracle_->checkPlaceWithOverlay(fr::OracleRequest{
        100, plannerTarget(scn), snapshotRegion(scn), changes});
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
  std::unique_ptr<PortablePlacementView> view_;
  std::unique_ptr<PortableCheckerOracle> oracle_;
};

TEST(ImplantCheckerNullSafetyTest,
     InvalidNodesAndMastersFailClosedWithoutDereference)
{
  PlannerCheckerFixture fixture;
  ASSERT_TRUE(fixture.checkerDiagnostics().empty());
  ImplantLayerChecker& checker = fixture.mutableChecker();
  Grid* grid = fixture.grid();
  Network* network = fixture.network();
  ASSERT_NE(grid, nullptr);
  ASSERT_NE(network, nullptr);
  ASSERT_FALSE(network->getNodes().empty());

  std::vector<CellChangeRecord> changes;
  EXPECT_FALSE(checker.check(nullptr,
                             GridX{0},
                             GridY{0},
                             eUTL::PhysOrientationE::R0,
                             changes));

  const Node* node = network->getNodes().front().get();
  ASSERT_NE(node, nullptr);
  ASSERT_NE(node->getMaster(), nullptr);
  CheckRequest request;
  request.instanceId = node->getId();
  request.masterId = node->getMaster()->getId();
  request.rowId = grid->gridSnapDownY(node).v;
  request.colId = grid->gridX(node).v;
  request.orientation = node->getOrient();

  CheckRequest unknownTarget = request;
  unknownTarget.instanceId = 1000000;
  const CheckResult targetResult = checker.checkDirect(unknownTarget);
  EXPECT_FALSE(targetResult.isLegal);
  EXPECT_TRUE(hasDiagnostic(targetResult.diagnostics,
                            "unknown_target_instance"));

  CheckRequest unknownMaster = request;
  unknownMaster.masterId = 1000000;
  const CheckResult masterResult = checker.checkDirect(unknownMaster);
  EXPECT_FALSE(masterResult.isLegal);
  EXPECT_TRUE(hasDiagnostic(masterResult.diagnostics,
                            "unknown_target_master"));

  CheckRequest outOfGrid = request;
  outOfGrid.rowId = -1;
  const std::vector<CheckResult> outOfGridResults =
      checker.checkPlaceWithOverlays(
          outOfGrid,
          makeRect(0,
                   0,
                   SITE_COUNT * SITE_WIDTH,
                   ROW_COUNT * ROW_HEIGHT),
          std::vector<FillerChanges>{FillerChanges{}});
  ASSERT_EQ(outOfGridResults.size(), 1u);
  EXPECT_FALSE(outOfGridResults.front().isLegal);
  EXPECT_TRUE(hasDiagnostic(outOfGridResults.front().diagnostics,
                            "placement_out_of_grid"));

  const std::vector<CheckResult> overlayResults =
      checker.checkPlaceWithOverlays(
          unknownTarget,
          makeRect(0,
                   0,
                   SITE_COUNT * SITE_WIDTH,
                   ROW_COUNT * ROW_HEIGHT),
          std::vector<FillerChanges>{FillerChanges{}});
  ASSERT_EQ(overlayResults.size(), 1u);
  EXPECT_FALSE(overlayResults.front().isLegal);
  EXPECT_TRUE(hasDiagnostic(overlayResults.front().diagnostics,
                            "unknown_target_instance"));

  Node* mutableNode = network->getNodes().front().get();
  Master* savedMaster = mutableNode->getMaster();
  mutableNode->setMaster(nullptr);
  const CheckResult missingMaster = checker.checkDirect(request);
  EXPECT_FALSE(missingMaster.isLegal);
  EXPECT_TRUE(hasDiagnostic(missingMaster.diagnostics,
                            "target_instance_missing_master"));
  mutableNode->setMaster(savedMaster);
}

TEST(InfrastructureNullSafetyTest, RejectsNullOwnedObjectsAndClearedGridAccess)
{
  Network emptyNetwork;
  EXPECT_FALSE(emptyNetwork.addNode(std::unique_ptr<Node>()));
  EXPECT_FALSE(emptyNetwork.addMaster(std::unique_ptr<Master>()));
  EXPECT_TRUE(emptyNetwork.getNodes().empty());
  EXPECT_TRUE(emptyNetwork.getMasters().empty());

  PlannerCheckerFixture fixture;
  Grid* grid = fixture.grid();
  ASSERT_NE(grid, nullptr);
  ASSERT_GT(grid->getRowCount().v, 0);
  grid->clear();
  EXPECT_EQ(grid->gridPixel(GridX{0}, GridY{0}), nullptr);
  EXPECT_EQ(grid->gridX(static_cast<const Node*>(nullptr)).v, 0);
  grid->paintPixel(nullptr);

  Grid uninitialized;
  uninitialized.examineRows(nullptr);
  EXPECT_EQ(uninitialized.getDesMgr(), nullptr);
  EXPECT_EQ(uninitialized.getRowCount().v, 0);
}

// --- window probe -----------------------------------------------------------
//
// The repair window IS the search space: which fillers may be edited, and --
// through its guard -- how much of the design the checker is asked about. Too
// narrow and the only repair is out of reach; too wide and the search explodes
// and the guard stops being the reach authority. So the cases below assert its
// size, not just that a repair came out the far end.
struct WindowProbe
{
  std::vector<fr::NormalizedViolation> normalized;
  fr::DbCoord ruleDistance = 0;
  fr::RepairWindow window;
};

WindowProbe probeWindow(PlannerCheckerFixture& fixture,
                        const Scenario& scn,
                        const std::vector<fr::Violation>& violations)
{
  WindowProbe probe;
  const fr::DebugLog log(false);
  const fr::FillerRepairRequest request{plannerTarget(scn), violations};
  probe.normalized = fr::normalizeViolations(request, log);
  probe.ruleDistance = fr::estimateRuleDistance(violations, SITE_WIDTH);
  probe.window = fr::buildWindow(
                                 request.targetPlace,
                                 probe.normalized,
                                 fixture.view(),
                                 probe.ruleDistance,
                                 log);
  return probe;
}

bool windowHasEditable(const fr::RepairWindow& window, fr::InstanceId id)
{
  return std::find(window.editableFillers.begin(),
                   window.editableFillers.end(),
                   id)
         != window.editableFillers.end();
}

// The whole flow on one scenario: retarget the std cell, confirm the checker
// rejects it, size the window, repair, and re-check with the real checker.
void expectPlannerRepairsWithFinalChecker(const Scenario& scn,
                                          const DensityCase& density)
{
  SCOPED_TRACE(densityTrace(density));
  SCOPED_TRACE(scn.name);
  PlannerCheckerFixture fixture(input(density));
  ASSERT_TRUE(fixture.checkerDiagnostics().empty());

  const fr::OracleResult baseline = fixture.baseline(scn);
  ASSERT_EQ(baseline.status, fr::OracleStatus::Checked);
  ASSERT_FALSE(baseline.isLegal);
  ASSERT_FALSE(baseline.violations.empty());

  // The window has to reach the bridge filler, or no repair exists at all.
  const WindowProbe probe = probeWindow(fixture, scn, baseline.violations);
  EXPECT_TRUE(windowHasEditable(probe.window, bridgeInstance(scn)))
      << "the L0 window does not reach the bridge filler";

  const fr::FillerRepairResult repaired
      = fixture.repair(scn, baseline.violations);
  ASSERT_TRUE(repaired.hasSolution) << plannerDiagnostics(repaired);
  ASSERT_FALSE(repaired.changes.empty());

  const fr::OracleResult verified = fixture.verify(scn, repaired.changes);
  EXPECT_EQ(verified.status, fr::OracleStatus::Checked);
  EXPECT_TRUE(verified.isLegal);
  EXPECT_TRUE(verified.violations.empty());
  EXPECT_TRUE(fixture.inputUnchanged());
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

// --- checker: what one VT change does, and what undoes it -------------------

// The layout is built legal, so every violation below is produced by the
// scenario edit and nothing else. Without this the cases could be passing on
// a defect that was planted rather than caused.
TEST_P(ImplantCheckerOverlayDensityTest, BuiltLayoutIsCleanBeforeAnyVtChange)
{
  for (const Scenario& scn : SCENARIOS) {
    SCOPED_TRACE(scn.name);
    const MasterId built = cellMaster((scn.col / 2) % 3);
    const std::vector<CheckResult> results
        = check(request(scn.row, scn.col, built), {{}}, GetParam());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results.front().isLegal);
    EXPECT_TRUE(results.front().violations.empty());
  }
}

// One realistic edit -- opto retargets a std cell to the VT of the pair on
// its right -- and the checker reports min width AND min spacing, on the new
// family's N band and on its P partner, intra-row and across a row boundary.
TEST_P(ImplantCheckerOverlayDensityTest, VtChangeViolatesWidthAndSpacing)
{
  for (const Scenario& scn : SCENARIOS) {
    SCOPED_TRACE(scn.name);
    const int family = familyOf(scn.newMaster);
    const std::vector<CheckResult> results
        = check(retargeted(scn), {{}}, GetParam());
    ASSERT_EQ(results.size(), 1u);
    const CheckResult& result = results.front();
    EXPECT_FALSE(result.isLegal);

    // The retargeted cell's own run is one site wide, on both bands.
    EXPECT_TRUE(hasRuleViolation(result, widthRule(family),
                                 Relationship::IntraRow));
    EXPECT_TRUE(hasRuleViolation(result, widthRule(family) + P_RULE_OFFSET,
                                 Relationship::IntraRow));
    // And it sits one site from the run it was meant to join.
    EXPECT_TRUE(hasRuleViolation(result, spacingRule(family),
                                 Relationship::IntraRow));
    EXPECT_TRUE(hasRuleViolation(result, spacingRule(family) + P_RULE_OFFSET,
                                 Relationship::IntraRow));
    // The same gap is seen across the row boundary, on whichever band faces
    // it (interRule picks by boundary parity; both boundaries are checked).
    EXPECT_TRUE(hasRuleViolation(result, spacingRule(family),
                                 Relationship::InterRow)
                || hasRuleViolation(result,
                                    spacingRule(family) + P_RULE_OFFSET,
                                    Relationship::InterRow));
    // Every reported violation involves the instance opto asked about.
    for (const Violation& violation : result.violations) {
      EXPECT_NE(std::find(violation.instances.begin(),
                          violation.instances.end(),
                          instId(scn.row, scn.col)),
                violation.instances.end());
    }
    expectOldUnrelatedFiltered(result);
  }
}

// The repair, expressed as the checker sees it: recolour the one filler
// between the two runs and they merge into a legal four-site run.
TEST_P(ImplantCheckerOverlayDensityTest, BridgeFillerSwapClearsEveryViolation)
{
  for (const Scenario& scn : SCENARIOS) {
    SCOPED_TRACE(scn.name);
    const std::vector<CheckResult> results
        = check(retargeted(scn),
                {bridgeSwap(scn, bridgeRepairMaster(scn))},
                GetParam());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results.front().isLegal);
    EXPECT_TRUE(results.front().violations.empty());
    expectOldUnrelatedFiltered(results.front());
  }
}

// A same-size swap on the right filler with the wrong VT: it looks like a
// repair and fixes nothing, which is why acceptance is the checker's call.
TEST_P(ImplantCheckerOverlayDensityTest, WrongVtOnTheBridgeLeavesTheViolation)
{
  for (const Scenario& scn : SCENARIOS) {
    SCOPED_TRACE(scn.name);
    const int family = familyOf(scn.newMaster);
    const std::vector<CheckResult> results
        = check(retargeted(scn),
                {bridgeSwap(scn, bridgeWrongMaster(scn))},
                GetParam());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results.front().isLegal);
    EXPECT_TRUE(hasRuleViolation(results.front(), widthRule(family),
                                 Relationship::IntraRow));
  }
}

// Results correlate by input order, so one batch must answer each candidate
// on its own merits -- a clean one and a useless one side by side.
TEST_P(ImplantCheckerOverlayDensityTest, BatchResultsCorrelateByInputOrder)
{
  for (const Scenario& scn : SCENARIOS) {
    SCOPED_TRACE(scn.name);
    const std::vector<CheckResult> results
        = check(retargeted(scn),
                {{},
                 bridgeSwap(scn, bridgeRepairMaster(scn)),
                 bridgeSwap(scn, bridgeWrongMaster(scn))},
                GetParam());
    ASSERT_EQ(results.size(), 3u);
    EXPECT_FALSE(results[0].isLegal);
    EXPECT_TRUE(results[1].isLegal);
    EXPECT_FALSE(results[2].isLegal);
  }
}

TEST(ImplantCheckerOverlayTest, ReplaceRejectsNamedCellData)
{
  const Scenario& scn = SCN_MID;
  const FillerChanges invalidChange{
      CellChangeRecord{OpType::Replace,
                       CellData{std::string("future_added_filler")},
                       UvDist(0),
                       UvDist(0),
                       LibCellID(),
                       libCellId(bridgeRepairMaster(scn)),
                       PhysOrientation(PhysOrientationE::R0)}};
  const std::vector<CheckResult> results
      = check(retargeted(scn), {invalidChange}, FILLER_50_STD_50);

  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results.front().isLegal);
  EXPECT_TRUE(std::any_of(
      results.front().diagnostics.begin(),
      results.front().diagnostics.end(),
      [](const Diagnostic& diagnostic) {
        return diagnostic.status == "changed_cell_data_not_leaf_id";
      }));
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

  const Scenario& scn = SCN_MID;
  // A guard that spans the target's own runs on its own row and nothing else.
  // The checker builds its snapshot from the guard, so the guard must cover
  // the pair under test; the changed filler is on a different row entirely --
  // well outside the guard -- and must neither mask nor invent the target's
  // violation.
  const Dbu targetY = scn.row * ROW_HEIGHT;
  const Rect scenarioGuard = makeRect((scn.col - 4) * SITE_WIDTH,
                                      targetY,
                                      (scn.col + 4) * SITE_WIDTH,
                                      targetY + ROW_HEIGHT);
  const FillerChanges outsideGuard{
      CellChangeRecord{OpType::Replace,
                       CellData{leafCellId(
                           OLD_UNRELATED_ROW, OLD_UNRELATED_COL + 1)},
                       UvDist(0),
                       UvDist(0),
                       LibCellID(),
                       libCellId(F3_FILL_MASTER),
                       PhysOrientation(PhysOrientationE::R0)}};
  const std::vector<CheckResult> results = checker.checkPlaceWithOverlays(
      retargeted(scn), scenarioGuard, {outsideGuard});

  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results.front().isLegal);
  EXPECT_TRUE(hasRuleViolation(results.front(),
                               widthRule(familyOf(scn.newMaster)),
                               Relationship::IntraRow));
}

// --- repair window: how much the planner opens up ---------------------------

// The L0 window is derived from the checker's own violation geometry, so it
// has to land on the two runs and the filler between them -- and stop there.
// A window that misses the bridge makes the repair unreachable; a window that
// swallows the row makes the search exponential for nothing.
TEST(FillerRepairWindowTest, L0WindowCoversTheViolationAndItsBridge)
{
  for (const Scenario& scn : SCENARIOS) {
    SCOPED_TRACE(scn.name);
    PlannerCheckerFixture fixture;
    const fr::OracleResult baseline = fixture.baseline(scn);
    ASSERT_FALSE(baseline.violations.empty());
    const WindowProbe probe = probeWindow(fixture, scn, baseline.violations);
    const fr::RepairWindow& window = probe.window;

    // Rows: the target's row plus the two it shares an inter-row violation
    // with -- no more, even though the design has three on each side.
    const std::vector<fr::RowId> expectedRows{
        static_cast<fr::RowId>(scn.row - 1),
        static_cast<fr::RowId>(scn.row),
        static_cast<fr::RowId>(scn.row + 1)};
    EXPECT_EQ(window.rows, expectedRows);

    // X: exactly five sites. The violation footprint is the retargeted cell
    // plus the run it has to join (four sites from the target), snapped out to
    // whole instances, which picks up the filler on the target's other side.
    // Wider would make the subset search exponential for nothing; narrower
    // would put the bridge out of reach.
    EXPECT_EQ(probe.ruleDistance, MIN_RULE);
    EXPECT_EQ(window.x.xl, (scn.col - 1) * SITE_WIDTH);
    EXPECT_EQ(window.x.xh, (scn.col + 4) * SITE_WIDTH);
    EXPECT_EQ(window.x.length(), 5 * SITE_WIDTH);

    // Three rows of five sites, of which the odd columns are fillers.
    EXPECT_EQ(window.editableFillers.size(), 9u);
    // The bridge filler is editable; the retargeted std cell is not.
    EXPECT_TRUE(windowHasEditable(window, bridgeInstance(scn)));
    EXPECT_FALSE(windowHasEditable(window, instId(scn.row, scn.col)));
    // Every editable is a filler inside the window's own rows and x range.
    for (fr::InstanceId id : window.editableFillers) {
      const fr::PlacedInstance* placed = fixture.view().instance(id);
      ASSERT_NE(placed, nullptr);
      EXPECT_TRUE(placed->isFiller);
      EXPECT_NE(std::find(window.rows.begin(), window.rows.end(), placed->rowId),
                window.rows.end());
      EXPECT_GE(placed->x, window.x.xl);
      EXPECT_LT(placed->x, window.x.xh);
    }
  }
}

// The guard is what the checker is asked about, and the checker's own reach
// (`getMaxRuleValue()` sites) is the one authority for how far a rule can
// see. A guard narrower than the window truncates the snapshot and fabricates
// min-width findings at its edge, so this pins containment plus reach.
TEST(FillerRepairWindowTest, GuardContainsTheWindowAndTheCheckersRuleReach)
{
  for (const Scenario& scn : SCENARIOS) {
    SCOPED_TRACE(scn.name);
    PlannerCheckerFixture fixture;
    const fr::OracleResult baseline = fixture.baseline(scn);
    ASSERT_FALSE(baseline.violations.empty());
    const WindowProbe probe = probeWindow(fixture, scn, baseline.violations);
    const fr::RepairWindow& window = probe.window;
    const fr::Region area = window.area();
    const fr::Region& guard = window.guardRegion;

    EXPECT_LE(guard.x.xl, area.x.xl);
    EXPECT_GE(guard.x.xh, area.x.xh);
    EXPECT_LE(guard.rowLo, area.rowLo);
    EXPECT_GE(guard.rowHi, area.rowHi);

    // Quantized: each side is snapped outward to a power-of-two number of
    // sites from the anchor (the target, which cannot move during a repair),
    // clamped at the core edge. That is what lets neighbouring adaptive
    // levels share one guard -- and one cached baseline.
    const Dbu anchor = scn.col * SITE_WIDTH;
    EXPECT_EQ(guard.x.xl, std::max<Dbu>(0, anchor - 4 * SITE_WIDTH));
    EXPECT_EQ(guard.x.xh, anchor + 8 * SITE_WIDTH);
    // Two rows of ring on each side of the window, clamped to the design.
    EXPECT_EQ(guard.rowLo, std::max<RowId>(0, scn.row - 3));
    EXPECT_EQ(guard.rowHi, std::min<RowId>(ROW_COUNT - 1, scn.row + 3));

    // And it clears the checker's own reach on both sides, so the snapshot it
    // builds is never truncated inside the window. `getMaxRuleValue()` is the
    // single authority for that distance; nothing here re-derives it from raw
    // width/spacing values.
    const Dbu reach = fixture.checker().getMaxRuleValue() * SITE_WIDTH;
    EXPECT_EQ(reach, MIN_RULE);
    EXPECT_LE(guard.x.xl, area.x.xl - reach);
    EXPECT_GE(guard.x.xh, area.x.xh + reach);
    EXPECT_GE(guard.x.xl, 0);
    EXPECT_LE(guard.x.xh, SITE_COUNT * SITE_WIDTH);
  }
}

// One adaptive step must actually buy something: strictly more editable
// fillers, never fewer, and never a window that walks off the design.
//
// A step walks outward from each window edge while it finds fillers and stops
// at the first std cell, so it can only grow where fillers actually sit next
// to each other. That is the wide-rule layout, not the alternating one.
TEST(FillerRepairWindowTest, AdaptiveStepGrowsTheEditableUniverse)
{
  const Scenario& scn = SCN_WIDE_RULE;
  PlannerCheckerFixture fixture(multiSwapWidthInput(2));
  const fr::OracleResult baseline = fixture.baseline(scn);
  ASSERT_FALSE(baseline.violations.empty());
  const WindowProbe probe = probeWindow(fixture, scn, baseline.violations);
  const fr::DebugLog log(false);
  const fr::RepairWindow grown = fr::expandWindowAdaptive(probe.window,
                                                          plannerTarget(scn),
                                                          baseline.violations,
                                                          fixture.view(),
                                                          2,
                                                          log);
  EXPECT_GT(grown.editableFillers.size(), probe.window.editableFillers.size());
  EXPECT_LE(grown.x.xl, probe.window.x.xl);
  EXPECT_GE(grown.x.xh, probe.window.x.xh);
  EXPECT_GE(grown.x.xl, 0);
  EXPECT_LE(grown.x.xh, SITE_COUNT * SITE_WIDTH);
  EXPECT_GE(grown.rows.front(), 0);
  EXPECT_LT(grown.rows.back(), ROW_COUNT);
  // The guard grows with it and still contains it.
  EXPECT_LE(grown.guardRegion.x.xl, grown.area().x.xl);
  EXPECT_GE(grown.guardRegion.x.xh, grown.area().x.xh);
}

// --- planner -> real checker, end to end ------------------------------------

TEST_P(FillerRepairCheckerDensityE2ETest, RepairsMidRowTarget)
{
  expectPlannerRepairsWithFinalChecker(SCN_MID, GetParam());
}

TEST_P(FillerRepairCheckerDensityE2ETest, RepairsEvenRowTarget)
{
  expectPlannerRepairsWithFinalChecker(SCN_EVEN_ROW, GetParam());
}

TEST_P(FillerRepairCheckerDensityE2ETest, RepairsTargetNearTheLeftEdge)
{
  expectPlannerRepairsWithFinalChecker(SCN_LEFT_EDGE, GetParam());
}

TEST_P(FillerRepairCheckerDensityE2ETest, RepairsFarColumnTarget)
{
  expectPlannerRepairsWithFinalChecker(SCN_FAR, GetParam());
}

// The bridge is the natural repair, so the planner should reach for it. It is
// not required to: acceptance is the checker's, and any checker-clean set is
// a correct answer. What is required is that the answer stays minimal.
TEST_P(FillerRepairCheckerDensityE2ETest, PrefersTheSingleBridgeSwap)
{
  PlannerCheckerFixture fixture(input(GetParam()));
  const Scenario& scn = SCN_MID;
  const fr::OracleResult baseline = fixture.baseline(scn);
  ASSERT_FALSE(baseline.violations.empty());
  const fr::FillerRepairResult result = fixture.repair(scn, baseline.violations);
  ASSERT_TRUE(result.hasSolution) << plannerDiagnostics(result);
  EXPECT_EQ(result.changes.size(), 1u);
  ASSERT_FALSE(result.changes.empty());
  EXPECT_EQ(fr::cellChangeRecordInstanceId(result.changes.front()),
            bridgeInstance(scn));
  EXPECT_EQ(fr::cellChangeRecordNewMasterId(result.changes.front()),
            bridgeRepairMaster(scn));
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
  // The same site, left on the VT it was built with: nothing to repair.
  const Scenario clean{"unchanged", TARGET_ROW, 20,
                       cellMaster((20 / 2) % 3), 21, 0};
  const fr::OracleResult result = fixture.baseline(clean);
  ASSERT_EQ(result.status, fr::OracleStatus::Checked);
  ASSERT_TRUE(result.isLegal);
  ASSERT_TRUE(result.violations.empty());
  const int beforeRequests = fixture.oracle().requestCount();
  const fr::FillerRepairResult repaired = fixture.repair(clean, {});
  EXPECT_TRUE(repaired.hasSolution);
  EXPECT_TRUE(repaired.changes.empty());
  EXPECT_EQ(fixture.oracle().requestCount(), beforeRequests);
  EXPECT_TRUE(fixture.inputUnchanged());
}

TEST(FillerRepairCheckerE2ETest, RepeatedRepairIsDeterministic)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline = fixture.baseline(SCN_MID);
  ASSERT_FALSE(baseline.violations.empty());
  const fr::FillerRepairResult first
      = fixture.repair(SCN_MID, baseline.violations);
  const fr::FillerRepairResult second
      = fixture.repair(SCN_MID, baseline.violations);
  ASSERT_TRUE(first.hasSolution);
  ASSERT_TRUE(second.hasSolution);
  EXPECT_TRUE(sameChanges(first.changes, second.changes));
  EXPECT_TRUE(fixture.inputUnchanged());
}

TEST(FillerRepairCheckerE2ETest, BatchSizeOneStillFindsSameRepair)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline = fixture.baseline(SCN_FAR);
  ASSERT_FALSE(baseline.violations.empty());
  fr::RepairConfig config;
  config.batchSize = 1;
  const fr::FillerRepairResult result
      = fixture.repair(SCN_FAR, baseline.violations, config);
  ASSERT_TRUE(result.hasSolution) << plannerDiagnostics(result);
  ASSERT_FALSE(result.changes.empty());
  EXPECT_TRUE(fixture.verify(SCN_FAR, result.changes).isLegal);
  EXPECT_GT(fixture.oracle().batchCount(), 1);
}

TEST(FillerRepairCheckerE2ETest, BatchSizeDoesNotChangeChosenOverlay)
{
  PlannerCheckerFixture smallBatch;
  PlannerCheckerFixture largeBatch;
  const fr::OracleResult smallBaseline = smallBatch.baseline(SCN_MID);
  const fr::OracleResult largeBaseline = largeBatch.baseline(SCN_MID);
  fr::RepairConfig smallConfig;
  smallConfig.batchSize = 1;
  fr::RepairConfig largeConfig;
  largeConfig.batchSize = 64;
  const fr::FillerRepairResult small
      = smallBatch.repair(SCN_MID, smallBaseline.violations, smallConfig);
  const fr::FillerRepairResult large
      = largeBatch.repair(SCN_MID, largeBaseline.violations, largeConfig);
  ASSERT_TRUE(small.hasSolution);
  ASSERT_TRUE(large.hasSolution);
  EXPECT_TRUE(sameChanges(small.changes, large.changes));
}

TEST(FillerRepairCheckerE2ETest, EmptyCandidateUniverseFailsWithoutPartial)
{
  PlannerCheckerFixture fixture(input(), std::vector<fr::MasterId>{});
  const fr::OracleResult baseline = fixture.baseline(SCN_MID);
  ASSERT_FALSE(baseline.violations.empty());
  fr::RepairConfig config;
  config.adaptiveStepFillers = 100;
  config.checkerCallBudgetPerWindow = 32;
  const fr::FillerRepairResult result
      = fixture.repair(SCN_MID, baseline.violations, config);
  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_TRUE(hasPlannerDiagnostic(result, "NoSwapGenerated")
              || hasPlannerDiagnostic(result, "NoEditableFiller"));
  EXPECT_TRUE(fixture.inputUnchanged());
}

// set_filler_option can leave exactly one VT family available. The repair for
// SCN_MID happens to need that family, so it must still be reachable when it
// is the only master offered.
TEST(FillerRepairCheckerE2ETest, SingleAllowedFillerMasterRemainsReachable)
{
  const MasterId only = bridgeRepairMaster(SCN_MID);
  PlannerCheckerFixture fixture(input(), std::vector<fr::MasterId>{only});
  const fr::OracleResult baseline = fixture.baseline(SCN_MID);
  ASSERT_FALSE(baseline.violations.empty());
  const fr::FillerRepairResult result
      = fixture.repair(SCN_MID, baseline.violations);
  ASSERT_TRUE(result.hasSolution) << plannerDiagnostics(result);
  ASSERT_FALSE(result.changes.empty());
  EXPECT_TRUE(std::all_of(result.changes.begin(),
                          result.changes.end(),
                          [](const dpl2::CellChangeRecord& change) {
                            return fr::cellChangeRecordNewMasterId(change) == only;
                          }));
  EXPECT_TRUE(fixture.verify(SCN_MID, result.changes).isLegal);
}

// Pins what Violation::xWindow MEANS, per rule kind. The planner seeds its
// repair window from it (united with the participants' spans) and measures
// relatedness of a new halo finding by its distance to a changed span, so a
// silent change of meaning here silently changes which fillers are editable
// and which new violations count as blocking.
//
// The checker sets it three ways:
//   width, no neighbour   the run itself
//   width, with neighbour the union / inter-row intersection of the two runs
//   SPACING               the GAP between the two runs -- an interval that
//                         lies BETWEEN the participants and contains neither
//
// That last one is the surprising one, and it is the one an earlier checker
// did not set at all: spacing violations used to arrive with a
// default-constructed [0,0), which put every one of them at the core's left
// edge. Anything that regresses to that shows up here rather than as
// mysteriously wide repair windows.
TEST(FillerRepairCheckerE2ETest, SpacingViolationXWindowIsTheGap)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline = fixture.baseline(SCN_MID);
  ASSERT_FALSE(baseline.violations.empty());

  int spacingSeen = 0;
  for (const fr::Violation& violation : baseline.violations) {
    if (violation.kind != fr::ViolationKind::MinSpacing) {
      continue;
    }
    ++spacingSeen;

    // Never the degenerate window at the core edge the old checker left.
    EXPECT_TRUE(violation.xWindow.xl > 0 || violation.xWindow.xh > 0)
        << "spacing xWindow is [0,0): the checker did not set it";

    // A gap is what separates the participants, so it overlaps none of them.
    for (const fr::ViolationParticipant& participant :
         violation.participants) {
      if (participant.xRange.empty()) {
        continue;
      }
      EXPECT_FALSE(violation.xWindow.overlaps(participant.xRange))
          << "spacing xWindow [" << violation.xWindow.xl << ","
          << violation.xWindow.xh << ") overlaps participant "
          << participant.instanceId << " [" << participant.xRange.xl << ","
          << participant.xRange.xh << ")";
    }

    // And it measures the distance the rule is about.
    EXPECT_EQ(violation.xWindow.length(), violation.measuredValue);
  }
  EXPECT_TRUE(spacingSeen > 0) << "fixture produced no spacing violation";
}

// Width windows are the runs themselves, so the retargeted cell's own
// one-site run is exactly what the checker hands back.
TEST(FillerRepairCheckerE2ETest, WidthViolationXWindowIsTheRun)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline = fixture.baseline(SCN_MID);
  ASSERT_FALSE(baseline.violations.empty());

  int widthSeen = 0;
  for (const fr::Violation& violation : baseline.violations) {
    if (violation.kind != fr::ViolationKind::MinWidth
        || violation.relation != fr::ViolationRelation::IntraRow) {
      continue;
    }
    ++widthSeen;
    EXPECT_EQ(violation.xWindow.xl, SCN_MID.col * SITE_WIDTH);
    EXPECT_EQ(violation.xWindow.xh, (SCN_MID.col + 1) * SITE_WIDTH);
    EXPECT_EQ(violation.xWindow.length(), violation.measuredValue);
    EXPECT_EQ(violation.requiredValue, MIN_RULE);
  }
  EXPECT_GT(widthSeen, 0) << "fixture produced no intra-row width violation";
}

TEST(FillerRepairCheckerE2ETest, OneCallBudgetReturnsNoPartialRepair)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline = fixture.baseline(SCN_MID);
  ASSERT_FALSE(baseline.violations.empty());
  fr::RepairConfig config;
  // The per-REPAIR ceiling is the one that starves the whole search: a
  // per-window ceiling alone is refilled at every adaptive level, and a guard
  // repeated across levels makes that level's baseline free (see
  // CachedBaselineFreesWindowBudget below). One call total means the L0
  // baseline consumes it and no candidate is ever evaluated.
  config.checkerCallBudgetPerRepair = 1;
  config.batchSize = 1;
  config.adaptiveStepFillers = 100;
  const fr::FillerRepairResult result
      = fixture.repair(SCN_MID, baseline.violations, config);
  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_TRUE(fixture.inputUnchanged());
}

// Adaptive levels reuse the guard -- it is quantized, so it changes O(log)
// times over the whole escalation rather than once per level -- and a
// repeated guard means that level's baseline is answered from cache without
// spending budget. The budget then buys candidate evaluations instead of
// re-buying an answer already held.
//
// One call per window makes that visible with nothing to hide behind: L0
// spends its single call on the baseline and evaluates no candidate at all,
// and the only way any candidate is ever evaluated is a later level whose
// baseline cost nothing. Starving the search this hard is the point, so no
// solution is expected -- `OneCallBudgetReturnsNoPartialRepair` above covers
// what a starved search must NOT return.
TEST(FillerRepairCheckerE2ETest, CachedBaselineFreesWindowBudget)
{
  // Needs a layout the window can actually escalate through, so that a later
  // level gets the chance to reuse an already-answered guard.
  const Scenario& scn = SCN_WIDE_RULE;
  PlannerCheckerFixture fixture(multiSwapWidthInput(2));
  const fr::OracleResult baseline = fixture.baseline(scn);
  ASSERT_FALSE(baseline.violations.empty());
  const int beforeRequests = fixture.oracle().requestCount();
  fr::RepairConfig config;
  config.checkerCallBudgetPerWindow = 1;  // one call per window, refilled
  config.batchSize = 1;
  config.adaptiveStepFillers = 100;
  const fr::FillerRepairResult result
      = fixture.repair(scn, baseline.violations, config);
  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  // A candidate was evaluated, which a level paying for its own baseline
  // could never afford at one call per window.
  EXPECT_TRUE(hasPlannerDiagnostic(result, "BestOverlay"))
      << "no candidate was evaluated: the baseline was re-bought every level"
      << plannerDiagnostics(result);
  EXPECT_GT(fixture.oracle().requestCount(), beforeRequests + 1);
  EXPECT_TRUE(fixture.inputUnchanged());
}

TEST(FillerRepairCheckerE2ETest, FabricatedOriginalFailsBaselineGate)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline = fixture.baseline(SCN_MID);
  ASSERT_FALSE(baseline.violations.empty());
  std::vector<fr::Violation> stale = baseline.violations;
  stale.front().ruleId += 10000;
  const fr::FillerRepairResult result = fixture.repair(SCN_MID, stale);
  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_TRUE(hasPlannerDiagnostic(result, "BaselineMismatch"));
}

TEST(FillerRepairCheckerE2ETest, DuplicateOriginalFailsOneToOneBaselineGate)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline = fixture.baseline(SCN_MID);
  ASSERT_FALSE(baseline.violations.empty());
  std::vector<fr::Violation> duplicated = baseline.violations;
  duplicated.push_back(duplicated.front());
  const fr::FillerRepairResult result = fixture.repair(SCN_MID, duplicated);
  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_TRUE(hasPlannerDiagnostic(result, "BaselineMismatch"));
}

TEST(FillerRepairCheckerE2ETest, ReturnedChangesTouchOnlySameSizeFillers)
{
  PlannerCheckerFixture fixture;
  const fr::OracleResult baseline = fixture.baseline(SCN_LEFT_EDGE);
  const fr::FillerRepairResult result
      = fixture.repair(SCN_LEFT_EDGE, baseline.violations);
  ASSERT_TRUE(result.hasSolution) << plannerDiagnostics(result);
  ASSERT_FALSE(result.changes.empty());
  for (const dpl2::CellChangeRecord& change : result.changes) {
    const fr::PlacedInstance* instance
        = fixture.view().instance(fr::cellChangeRecordInstanceId(change));
    ASSERT_NE(instance, nullptr);
    EXPECT_TRUE(instance->isFiller);
    const fr::MasterInfo* oldMaster
        = fixture.view().masterInfo(instance->masterId);
    const fr::MasterInfo* newMaster
        = fixture.view().masterInfo(fr::cellChangeRecordNewMasterId(change));
    ASSERT_NE(oldMaster, nullptr);
    ASSERT_NE(newMaster, nullptr);
    EXPECT_EQ(oldMaster->width, newMaster->width);
    EXPECT_EQ(oldMaster->height, newMaster->height);
  }
  EXPECT_TRUE(fixture.inputUnchanged());
}

// Everything the planner touches lies in its own window: it never reaches
// across the design to a filler that belongs to some other scenario.
TEST(FillerRepairCheckerE2ETest, ChangesStayInsideTheRepairWindow)
{
  for (const Scenario& scn : SCENARIOS) {
    SCOPED_TRACE(scn.name);
    PlannerCheckerFixture fixture;
    const fr::OracleResult baseline = fixture.baseline(scn);
    ASSERT_FALSE(baseline.violations.empty());
    const fr::FillerRepairResult result
        = fixture.repair(scn, baseline.violations);
    ASSERT_TRUE(result.hasSolution) << plannerDiagnostics(result);
    for (const dpl2::CellChangeRecord& change : result.changes) {
      const fr::PlacedInstance* placed
          = fixture.view().instance(fr::cellChangeRecordInstanceId(change));
      ASSERT_NE(placed, nullptr);
      EXPECT_LE(std::abs(placed->rowId - scn.row), 1);
      EXPECT_LE(std::abs(placed->x - scn.col * SITE_WIDTH),
                4 * MIN_RULE);
    }
    EXPECT_TRUE(fixture.verify(scn, result.changes).isLegal);
  }
}

// A minimum width of several sites cannot be met by one swap: the retargeted
// cell has to grow its run through two adjacent fillers, and both have to be
// in the answer or the answer is not legal.
TEST(FillerRepairCheckerE2ETest, MinimumWidthCanRequireTwoAtomicSwaps)
{
  PlannerCheckerFixture fixture(
      multiSwapWidthInput(2),
      std::vector<fr::MasterId>{bridgeRepairMaster(SCN_WIDE_RULE)});
  const fr::OracleResult baseline = fixture.baseline(SCN_WIDE_RULE);
  ASSERT_FALSE(baseline.violations.empty());
  fr::RepairConfig config;
  config.checkerCallBudgetPerWindow = 4096;
  config.memberCapSize3 = 100;
  config.batchSize = 64;
  const fr::FillerRepairResult result
      = fixture.repair(SCN_WIDE_RULE, baseline.violations, config);
  ASSERT_TRUE(result.hasSolution) << plannerDiagnostics(result);
  EXPECT_EQ(result.changes.size(), 2u);
  const fr::OracleResult verified
      = fixture.verify(SCN_WIDE_RULE, result.changes);
  EXPECT_TRUE(verified.isLegal);
  EXPECT_TRUE(verified.violations.empty());
}

TEST(FillerRepairCheckerE2ETest,
     ThreeSwapSolutionSurvivesAdaptiveDirectionFallback)
{
  PlannerCheckerFixture fixture(
      multiSwapWidthInput(3),
      std::vector<fr::MasterId>{bridgeRepairMaster(SCN_WIDE_RULE)});
  const fr::OracleResult baseline = fixture.baseline(SCN_WIDE_RULE);
  ASSERT_FALSE(baseline.violations.empty());
  fr::RepairConfig config;
  config.checkerCallBudgetPerWindow = 4096;
  config.memberCapSize3 = 200;
  config.batchSize = 64;
  const fr::FillerRepairResult result
      = fixture.repair(SCN_WIDE_RULE, baseline.violations, config);
  ASSERT_TRUE(result.hasSolution) << plannerDiagnostics(result);
  EXPECT_EQ(result.changes.size(), 3u);
  // Contiguous with the retargeted cell: a run is only as wide as its
  // uninterrupted sites, so a scattered triple could not have been accepted.
  std::vector<ColId> columns{SCN_WIDE_RULE.col};
  for (const dpl2::CellChangeRecord& change : result.changes) {
    const fr::PlacedInstance* placed
        = fixture.view().instance(fr::cellChangeRecordInstanceId(change));
    ASSERT_NE(placed, nullptr);
    EXPECT_EQ(placed->rowId, SCN_WIDE_RULE.row);
    columns.push_back(static_cast<ColId>(placed->x / SITE_WIDTH));
  }
  std::sort(columns.begin(), columns.end());
  EXPECT_EQ(columns.back() - columns.front(),
            static_cast<ColId>(columns.size() - 1));
  const fr::OracleResult verified
      = fixture.verify(SCN_WIDE_RULE, result.changes);
  EXPECT_TRUE(verified.isLegal);
  EXPECT_TRUE(verified.violations.empty());
  EXPECT_TRUE(fixture.inputUnchanged());
}

}  // namespace
}  // namespace ipl
}  // namespace dpl2
