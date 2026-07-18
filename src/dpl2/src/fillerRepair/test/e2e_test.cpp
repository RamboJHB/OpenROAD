// Tier-1 end-to-end smoke over the REAL repair chain:
//   fake-UDM DesignDb -> Session -> FillerRepairEngine::init (private
//   Grid/Network/final-checker snapshot) -> {precheck,repair}.
//
// Every case owns at least five standard-cell rows. The canonical scenario
// (siteWidth=1, rowHeight=8, WIDTH rule=6, SPACING rule=2) is:
//   row2: SL[0,6) FL[6,8) T=TL[8,12) FL2[12,14) SL[14,20)   (all VTL)
//   rows 0/1/3/4: SL SL SL FL (all VTL, fully covered)
// repair(T -> TH(VTH)) breaks the VTL run: VTH island [8,12) width 4 < 6.
// Swapping ONE adjacent VTL filler to the VTH filler master heals it
// (either filler works; the engine picks deterministically).

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "fillerRepair/FillerRepairEngine.h"
#include "infrastructure/fillerSetting.h"

namespace {

constexpr int kSiteWidth = 1;
constexpr int kRowHeight = 8;
constexpr int kRowSites = 20;
constexpr int kStandardRows = 5;
constexpr int kTargetCellIndex = 122;

struct MasterSpec
{
  const char* name;
  int libIndex;
  int width;
  bool isFiller;
  int nLayerRel;  // bottom band layer (N)
  int pLayerRel;  // top band layer (P)
};

const MasterSpec kMasters[] = {
    {"SL6", 0, 6, false, 0, 1},  // VTL std, context cells
    {"TL4", 1, 4, false, 0, 1},  // VTL std, the target's old master
    {"TH4", 2, 4, false, 2, 3},  // VTH std, the target's new master
    {"FL2", 3, 2, true, 0, 1},   // VTL filler
    {"FH2", 4, 2, true, 2, 3},   // VTH filler
    {"FS2", 5, 2, true, 4, 5},   // VTS filler (third VT, demoted in domains)
};

struct Placement
{
  int cellIndex;  // LeafCellID index
  int libIndex;
  int row;
  int x;
};

const Placement kPlacements[] = {
    // row 0: SL6 SL6 SL6 FL2  -> 6+6+6+2 = 20
    {100, 0, 0, 0},
    {101, 0, 0, 6},
    {102, 0, 0, 12},
    {103, 3, 0, 18},
    // row 1: SL6 SL6 SL6 FL2
    {110, 0, 1, 0},
    {111, 0, 1, 6},
    {112, 0, 1, 12},
    {113, 3, 1, 18},
    // row 2: SL6 FL2 TL4 FL2 SL6 -> 6+2+4+2+6 = 20
    {120, 0, 2, 0},
    {121, 3, 2, 6},
    {122, 1, 2, 8},  // the repair target
    {123, 3, 2, 12},
    {124, 0, 2, 14},
    // rows 3/4: SL6 SL6 SL6 FL2
    {130, 0, 3, 0},
    {131, 0, 3, 6},
    {132, 0, 3, 12},
    {133, 3, 3, 18},
    {140, 0, 4, 0},
    {141, 0, 4, 6},
    {142, 0, 4, 12},
    {143, 3, 4, 18},
};

using PhysicalSnapshot = std::vector<
    std::tuple<int, int64_t, int64_t, int, int, int>>;

PhysicalSnapshot snapshotPhysicalData(fake_udm::DesignDb& db)
{
  PhysicalSnapshot snapshot;
  for (const Placement& placement : kPlacements) {
    const eUNL::PhysCell cell
        = db.desMgr().getPhysCell(eUNL::LeafCellID(0, placement.cellIndex));
    const eUTL::Point2D origin = cell.getOrigin();
    snapshot.emplace_back(
        placement.cellIndex,
        origin.getX().getStorage(),
        origin.getY().getStorage(),
        cell.getPhysMaster().getLibCellId().getIndexValue(),
        static_cast<int>(cell.getStatus()),
        static_cast<int>(cell.getOrient().getValue()));
  }
  return snapshot;
}

bool hasDiagnostic(const std::vector<dpl2::ipl::Diagnostic>& diagnostics,
                   const std::string& status)
{
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [&](const dpl2::ipl::Diagnostic& diagnostic) {
                       return diagnostic.status == status;
                     });
}

// Fixture variations. The defaults reproduce the canonical smoke design.
struct DesignSetup
{
  // Adds VTUL_{N,P} implant layers carrying a WIDTH value but no SPACING:
  // the checker records persistent missing_rule_parameter diagnostics at
  // init while no master ever touches these layers.
  bool unusedRuleLayers = false;
  // Prepends a pad row (below the core, first in iteration order) so the
  // first PhysRow is NOT a standard-cell row.
  bool padRowFirst = false;
  int64_t padRowOriginX = 0;
  // Per-standard-row origin X; placed cells shift with their row.
  std::array<int64_t, kStandardRows> rowOriginX{0, 0, 0, 0, 0};
};

void buildDesign(fake_udm::DesignDb& db, const DesignSetup& setup = {})
{
  db.coreSite.width_ = eUTL::UvDist(kSiteWidth);
  db.coreSite.height_ = eUTL::UvDist(kRowHeight);
  db.tech().addLayer("VTL_N", true, 0, /*width=*/6, /*minSpacing=*/2);
  db.tech().addLayer("VTL_P", true, 1, 6, 2);
  db.tech().addLayer("VTH_N", true, 2, 6, 2);
  db.tech().addLayer("VTH_P", true, 3, 6, 2);
  db.tech().addLayer("VTS_N", true, 4, 6, 2);
  db.tech().addLayer("VTS_P", true, 5, 6, 2);
  db.tech().addLayer("M1", false, 6);  // non-implant noise
  if (setup.unusedRuleLayers) {
    db.tech().addLayer("VTUL_N", true, 7, 6, /*minSpacing=*/0);
    db.tech().addLayer("VTUL_P", true, 8, 6, 0);
  }

  for (const MasterSpec& spec : kMasters) {
    eLIB::PhysLibCell& cell = db.addMaster(spec.name, spec.libIndex,
                                           spec.width, kRowHeight,
                                           spec.isFiller);
    fake_udm::DesignDb::addShape(cell, spec.nLayerRel, 0, kRowHeight / 2);
    fake_udm::DesignDb::addShape(cell, spec.pLayerRel, kRowHeight / 2,
                                 kRowHeight);
  }
  // Extra VTH filler master that exists in the library but is neither placed
  // nor part of the canonical allow list (candidate-universe mismatch tests).
  {
    eLIB::PhysLibCell& extra = db.addMaster("FX2", 6, 2, kRowHeight, true);
    fake_udm::DesignDb::addShape(extra, 2, 0, kRowHeight / 2);
    fake_udm::DesignDb::addShape(extra, 3, kRowHeight / 2, kRowHeight);
  }

  int rowIndexOffset = 0;
  if (setup.padRowFirst) {
    db.desMgr().addRow(setup.padRowOriginX, -kRowHeight, kSiteWidth,
                       kRowHeight, kRowSites, /*isPad=*/true);
    rowIndexOffset = 1;
  }
  for (int row = 0; row < kStandardRows; ++row) {
    db.desMgr().addRow(setup.rowOriginX[static_cast<size_t>(row)],
                       row * kRowHeight, kSiteWidth, kRowHeight, kRowSites);
  }
  // Row alternation convention: the track pattern expects P at the bottom
  // band on EVEN rows and N on odd rows (buildTrackPattern), indexed over ALL
  // rows including pads; R0 masters are N-bottom, so even-index cells are
  // placed MX-flipped.
  for (const Placement& p : kPlacements) {
    const int rowIndex = p.row + rowIndexOffset;
    const eUTL::PhysOrientation orient = rowIndex % 2 == 0
                                             ? eUTL::PhysOrientationE::MX
                                             : eUTL::PhysOrientationE::R0;
    db.desMgr().addCell(
        eUNL::LeafCellID(0, p.cellIndex),
        &db.design.lib_acc_.getPhysLibCell(p.libIndex),
        setup.rowOriginX[static_cast<size_t>(p.row)] + p.x,
        p.row * kRowHeight, orient);
  }
  db.activate();
}

std::vector<eUNL::LeafCellID> allLeafCells()
{
  std::vector<eUNL::LeafCellID> leafCells;
  leafCells.reserve(std::size(kPlacements));
  for (const Placement& p : kPlacements) {
    leafCells.emplace_back(0, p.cellIndex);
  }
  return leafCells;
}

struct LayoutCase
{
  const char* name;
  DesignSetup setup;
};

LayoutCase canonicalLayout()
{
  return {"Canonical", {}};
}

LayoutCase shiftedLayout()
{
  DesignSetup setup;
  setup.rowOriginX.fill(4);
  return {"ShiftedOrigin", setup};
}

LayoutCase farShiftedLayout()
{
  DesignSetup setup;
  setup.rowOriginX.fill(11);
  return {"FarShiftedOrigin", setup};
}

// Owns one complete production-chain snapshot. Only DesignDb is fake; the
// engine privately owns the production importer, Grid/Network, final checker
// and planner compiled from the same sources copied to the destination.
class ProductionHarness
{
 public:
  explicit ProductionHarness(const DesignSetup& setup)
      : filler_setting_(&db_.design)
  {
    buildDesign(db_, setup);
    filler_setting_.addFillerCell("FL2 FH2 FS2");
    engine_ = std::make_unique<dpl2::fillerRepair::FillerRepairEngine>();
    engine_ready_ = engine_->init(
        db_.design.getPhysDesMgr(), allLeafCells(), filler_setting_,
        db_.design.lib_acc_.getPhysLibCell(2));
  }

  bool engineReady() const { return engine_ready_; }

  fake_udm::DesignDb& db() { return db_; }
  dpl2::fillerRepair::FillerRepairEngine& engine() { return *engine_; }

 private:
  fake_udm::DesignDb db_;
  dpl2::fillerSetting filler_setting_;
  std::unique_ptr<dpl2::fillerRepair::FillerRepairEngine> engine_;
  bool engine_ready_ = false;
};

bool sameChanges(const dpl2::ipl::FillerChanges& lhs,
                 const dpl2::ipl::FillerChanges& rhs)
{
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].op_ != rhs[i].op_ || lhs[i].cell_id_ != rhs[i].cell_id_
        || lhs[i].new_lib_cell_ != rhs[i].new_lib_cell_) {
      return false;
    }
  }
  return true;
}

class FillerRepairProductionE2E
    : public ::testing::TestWithParam<LayoutCase>
{
};

}  // namespace

// Each public behavior below is exercised by three independently discovered
// GoogleTests: canonical and two distinct non-zero shared row origins. Every
// parameter owns exactly five standard rows.
TEST_P(FillerRepairProductionE2E, CleanPlacementPrecheck)
{
  ProductionHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  ASSERT_GE(harness.db().desMgr().getPhysRowIter().size(), kStandardRows);
  const PhysicalSnapshot before = snapshotPhysicalData(harness.db());
  const dpl2::ipl::CheckResult result = harness.engine().precheck();
  EXPECT_TRUE(result.isLegal);
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_EQ(snapshotPhysicalData(harness.db()), before);
}

TEST_P(FillerRepairProductionE2E, GapPlacementPrecheck)
{
  ProductionHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  ASSERT_GE(harness.db().desMgr().getPhysRowIter().size(), kStandardRows);
  eUNL::PhysCellData& moved
      = harness.db().desMgr().cells_[eUNL::LeafCellID(0, 103)];
  moved.origin = eUTL::Point2D(
      eUTL::UvDist(GetParam().setup.rowOriginX[0] + kRowSites),
      eUTL::UvDist(0));
  const PhysicalSnapshot before = snapshotPhysicalData(harness.db());
  const dpl2::ipl::CheckResult result = harness.engine().precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "Gap"));
  EXPECT_EQ(snapshotPhysicalData(harness.db()), before);
}

TEST_P(FillerRepairProductionE2E, OverlapPlacementPrecheck)
{
  ProductionHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  ASSERT_GE(harness.db().desMgr().getPhysRowIter().size(), kStandardRows);
  eUNL::PhysCellData& moved
      = harness.db().desMgr().cells_[eUNL::LeafCellID(0, 113)];
  moved.origin = eUTL::Point2D(
      eUTL::UvDist(GetParam().setup.rowOriginX[1] + 17),
      eUTL::UvDist(kRowHeight));
  const PhysicalSnapshot before = snapshotPhysicalData(harness.db());
  const dpl2::ipl::CheckResult result = harness.engine().precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "Overlap"));
  EXPECT_EQ(snapshotPhysicalData(harness.db()), before);
}

TEST_P(FillerRepairProductionE2E, CleanTargetOverlayNeedsNoFillerChange)
{
  ProductionHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  ASSERT_GE(harness.db().desMgr().getPhysRowIter().size(), kStandardRows);
  const PhysicalSnapshot before = snapshotPhysicalData(harness.db());
  const dpl2::fillerRepair::RepairOutcome outcome = harness.engine().repair(
      eUNL::LeafCellID(0, kTargetCellIndex),
      harness.db().design.lib_acc_.getPhysLibCell(1));
  EXPECT_TRUE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_EQ(snapshotPhysicalData(harness.db()), before);
}

TEST_P(FillerRepairProductionE2E, ViolatingTargetOverlayFindsFillerSwap)
{
  ProductionHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  ASSERT_GE(harness.db().desMgr().getPhysRowIter().size(), kStandardRows);
  const PhysicalSnapshot before = snapshotPhysicalData(harness.db());
  const dpl2::fillerRepair::RepairOutcome outcome = harness.engine().repair(
      eUNL::LeafCellID(0, kTargetCellIndex),
      harness.db().design.lib_acc_.getPhysLibCell(2));
  ASSERT_TRUE(outcome.hasSolution);
  ASSERT_EQ(outcome.changes.size(), 1U);
  EXPECT_EQ(outcome.changes.front().op_, dpl2::OpType::Replace);
  EXPECT_EQ(outcome.changes.front().new_lib_cell_.getIndexValue(), 4);
  EXPECT_TRUE(outcome.changes.front().cell_id_.getIndexValue() == 121
              || outcome.changes.front().cell_id_.getIndexValue() == 123);
  EXPECT_EQ(snapshotPhysicalData(harness.db()), before);
}

TEST_P(FillerRepairProductionE2E, RepeatedRepairIsDeterministicAndNonMutating)
{
  ProductionHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  ASSERT_GE(harness.db().desMgr().getPhysRowIter().size(), kStandardRows);
  const PhysicalSnapshot before = snapshotPhysicalData(harness.db());
  const auto& newMaster = harness.db().design.lib_acc_.getPhysLibCell(2);
  const auto first = harness.engine().repair(
      eUNL::LeafCellID(0, kTargetCellIndex), newMaster);
  const auto second = harness.engine().repair(
      eUNL::LeafCellID(0, kTargetCellIndex), newMaster);
  EXPECT_TRUE(first.hasSolution);
  EXPECT_EQ(second.hasSolution, first.hasSolution);
  EXPECT_TRUE(sameChanges(first.changes, second.changes));
  EXPECT_EQ(snapshotPhysicalData(harness.db()), before);
}

// The checker copies persistent init diagnostics into overlay results. The
// engine must strip those repetitions before classifying a candidate.
TEST_P(FillerRepairProductionE2E,
       PersistentCheckerDiagnosticsDoNotBlockRepair)
{
  DesignSetup setup = GetParam().setup;
  setup.unusedRuleLayers = true;
  ProductionHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());
  ASSERT_GE(harness.db().desMgr().getPhysRowIter().size(), kStandardRows);
  const auto outcome = harness.engine().repair(
      eUNL::LeafCellID(0, kTargetCellIndex),
      harness.db().design.lib_acc_.getPhysLibCell(2));
  ASSERT_TRUE(outcome.hasSolution);
  ASSERT_FALSE(outcome.changes.empty());
  EXPECT_EQ(outcome.changes.front().new_lib_cell_.getIndexValue(), 4);
}

// The single facade imports the complete configured filler universe itself,
// including an uninstantiated master. Callers cannot accidentally build a
// smaller Network than the engine configuration.
TEST_P(FillerRepairProductionE2E, ConfiguredMastersAreImportedByEngine)
{
  fake_udm::DesignDb db;
  buildDesign(db, GetParam().setup);
  ASSERT_GE(db.desMgr().getPhysRowIter().size(), kStandardRows);
  dpl2::fillerSetting fillerSetting(&db.design);
  fillerSetting.addFillerCell("FL2 FH2 FS2 FX2");
  dpl2::fillerRepair::FillerRepairEngine engine;
  EXPECT_TRUE(engine.init(db.design.getPhysDesMgr(), allLeafCells(),
                          fillerSetting,
                          db.design.lib_acc_.getPhysLibCell(2)));
}

// An empty allow list is rejected by the single production facade, and the
// reason remains available through the standard result diagnostics.
TEST_P(FillerRepairProductionE2E, EmptyFillerAllowListErrorsOut)
{
  fake_udm::DesignDb db;
  buildDesign(db, GetParam().setup);
  ASSERT_GE(db.desMgr().getPhysRowIter().size(), kStandardRows);
  dpl2::fillerSetting emptySetting(&db.design);
  dpl2::fillerRepair::FillerRepairEngine engine;
  EXPECT_FALSE(engine.init(db.design.getPhysDesMgr(), allLeafCells(),
                           emptySetting,
                           db.design.lib_acc_.getPhysLibCell(2)));
  const auto result = engine.precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "empty_filler_allow_list"));
}

// The final checker reads its design through UDM Session. The facade must
// reject an explicit PhysDesMgr from a different design before checker
// construction, rather than silently creating a mixed snapshot.
TEST_P(FillerRepairProductionE2E, ActiveDesignMismatchFailsInit)
{
  fake_udm::DesignDb requestedDb;
  buildDesign(requestedDb, GetParam().setup);
  dpl2::fillerSetting fillerSetting(&requestedDb.design);
  fillerSetting.addFillerCell("FL2 FH2 FS2");

  fake_udm::DesignDb activeDb;
  buildDesign(activeDb);  // makes a different design current in Session

  dpl2::fillerRepair::FillerRepairEngine engine;
  EXPECT_FALSE(engine.init(requestedDb.design.getPhysDesMgr(), allLeafCells(),
                           fillerSetting,
                           requestedDb.design.lib_acc_.getPhysLibCell(2)));
  const auto result = engine.precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "active_design_mismatch"));
}

// Before or after failed init, every public entry point must fail closed.
TEST_P(FillerRepairProductionE2E, FailedInitFailsClosed)
{
  fake_udm::DesignDb db;
  buildDesign(db, GetParam().setup);
  ASSERT_GE(db.desMgr().getPhysRowIter().size(), kStandardRows);
  dpl2::fillerSetting fillerSetting(&db.design);
  fillerSetting.addFillerCell("FL2 FH2 FS2");
  dpl2::fillerRepair::FillerRepairEngine engine;
  const auto expectClosed = [&](const char* phase) {
    SCOPED_TRACE(phase);
    const auto precheck = engine.precheck();
    EXPECT_FALSE(precheck.isLegal);
    EXPECT_TRUE(hasDiagnostic(precheck.diagnostics,
                              "precheck_not_initialized"));
    const auto repair = engine.repair(
        eUNL::LeafCellID(0, kTargetCellIndex),
        db.design.lib_acc_.getPhysLibCell(2));
    EXPECT_FALSE(repair.hasSolution);
    EXPECT_TRUE(repair.changes.empty());
    EXPECT_TRUE(hasDiagnostic(repair.diagnostics, "engine_not_initialized"));
  };
  expectClosed("before init");
  EXPECT_FALSE(engine.init(nullptr, allLeafCells(), fillerSetting,
                           db.design.lib_acc_.getPhysLibCell(2)));
  expectClosed("after failed init");
}

// One engine owns one coherent design snapshot. A second init is rejected
// without damaging the already-ready first snapshot.
TEST_P(FillerRepairProductionE2E, EngineUsesOneInitialization)
{
  fake_udm::DesignDb db;
  buildDesign(db, GetParam().setup);
  dpl2::fillerSetting fillerSetting(&db.design);
  fillerSetting.addFillerCell("FL2 FH2 FS2");
  dpl2::fillerRepair::FillerRepairEngine engine;
  ASSERT_TRUE(engine.init(db.design.getPhysDesMgr(), allLeafCells(),
                          fillerSetting,
                          db.design.lib_acc_.getPhysLibCell(2)));
  EXPECT_FALSE(engine.init(db.design.getPhysDesMgr(), allLeafCells(),
                           fillerSetting,
                           db.design.lib_acc_.getPhysLibCell(2)));
  EXPECT_TRUE(engine.precheck().isLegal);
}

INSTANTIATE_TEST_SUITE_P(
    FiveRowLayouts,
    FillerRepairProductionE2E,
    ::testing::Values(canonicalLayout(), shiftedLayout(), farShiftedLayout()),
    [](const ::testing::TestParamInfo<LayoutCase>& info) {
      return info.param.name;
    });

namespace {

struct RowOriginCase
{
  const char* name;
  DesignSetup setup;
  bool expectInit;
};

RowOriginCase alignedShiftedRows()
{
  DesignSetup setup;
  setup.rowOriginX.fill(3);
  return {"AlignedShiftedRows", setup, true};
}

RowOriginCase padBeforeStandardRows()
{
  DesignSetup setup;
  setup.padRowFirst = true;
  setup.padRowOriginX = 5;
  return {"PadBeforeStandardRows", setup, true};
}

RowOriginCase misalignedStandardRowAfterPad()
{
  DesignSetup setup;
  setup.padRowFirst = true;
  setup.padRowOriginX = 5;
  setup.rowOriginX = {0, 0, 0, 3, 0};
  return {"MisalignedStandardRowAfterPad", setup, false};
}

class FillerRepairRowOriginE2E
    : public ::testing::TestWithParam<RowOriginCase>
{
};

}  // namespace

// Three five-row cases prove the x-frame baseline uses the first non-pad row.
TEST_P(FillerRepairRowOriginE2E, FirstNonPadRowDefinesSharedXFrame)
{
  const RowOriginCase& testCase = GetParam();
  fake_udm::DesignDb db;
  buildDesign(db, testCase.setup);
  ASSERT_GE(db.desMgr().getPhysRowIter().size(), kStandardRows);
  dpl2::fillerSetting fillerSetting(&db.design);
  fillerSetting.addFillerCell("FL2 FH2 FS2");
  dpl2::fillerRepair::FillerRepairEngine engine;
  EXPECT_EQ(engine.init(db.design.getPhysDesMgr(), allLeafCells(),
                        fillerSetting,
                        db.design.lib_acc_.getPhysLibCell(2)),
            testCase.expectInit);
}

INSTANTIATE_TEST_SUITE_P(
    FiveRowOriginCases,
    FillerRepairRowOriginE2E,
    ::testing::Values(alignedShiftedRows(), padBeforeStandardRows(),
                      misalignedStandardRowAfterPad()),
    [](const ::testing::TestParamInfo<RowOriginCase>& info) {
      return info.param.name;
    });
