// Tier-1 end-to-end smoke over the REAL repair chain:
//   fake-UDM DesignDb -> Session -> ipl::ImplantLayerChecker (real
//   init(desMgr) UDM extraction) -> FillerRepairEngine::{precheck,repair}.
//
// Scenario (siteWidth=1, rowHeight=8, WIDTH rule=6, SPACING rule=2):
//   row1: SL[0,6) FL[6,8) T=TL[8,12) FL2[12,14) SL[14,20)   (all VTL)
//   rows 0/2: SL SL SL FL (all VTL, fully covered)
// repair(T -> TH(VTH)) breaks the VTL run: VTH island [8,12) width 4 < 6.
// Swapping ONE adjacent VTL filler to the VTH filler master heals it
// (either filler works; the engine picks deterministically).

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "drc/ImplantLayerChecker.h"
#include "fillerRepair/FillerRepairEngine.h"
#include "fillerRepair/RepairInfrastructure.h"
#include "infrastructure/fillerSetting.h"

namespace {

constexpr int kSiteWidth = 1;
constexpr int kRowHeight = 8;
constexpr int kRowSites = 20;

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
    // row 1: SL6 FL2 TL4 FL2 SL6 -> 6+2+4+2+6 = 20
    {110, 0, 1, 0},
    {111, 3, 1, 6},
    {112, 1, 1, 8},  // the repair target
    {113, 3, 1, 12},
    {114, 0, 1, 14},
    // row 2: SL6 SL6 SL6 FL2
    {120, 0, 2, 0},
    {121, 0, 2, 6},
    {122, 0, 2, 12},
    {123, 3, 2, 18},
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
  std::array<int64_t, 3> rowOriginX{0, 0, 0};
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
  for (int row = 0; row < 3; ++row) {
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

void expect(bool ok, const std::string& what)
{
  SCOPED_TRACE(what);
  EXPECT_TRUE(ok);
  if (ok) {
    std::printf("[smoke][ok] %s\n", what.c_str());
  }
}

}  // namespace

TEST(FillerRepairProduction, FakeUdmEndToEnd)
{
  using dpl2::fillerRepair::FillerRepairEngine;
  using dpl2::fillerRepair::RepairOutcome;

  // --- fake UDM design ------------------------------------------------------
  fake_udm::DesignDb db;
  buildDesign(db);

  // --- ECO filler allow list (the candidate universe) -----------------------
  dpl2::fillerSetting fillerSetting(&db.design);
  fillerSetting.addFillerCell("FL2 FH2 FS2");

  // --- production infrastructure import ------------------------------------
  // The test provides only UDM data + cell handles.  Network masters/nodes,
  // Grid geometry and occupancy are all built by production infrastructure.
  dpl2::RepairInfrastructure infrastructure;
  const bool infraReady = infrastructure.build(
      db.design.getPhysDesMgr(), allLeafCells(), fillerSetting,
      db.design.lib_acc_.getPhysLibCell(2));  // target new master TH4
  for (const std::string& diag : infrastructure.diagnostics()) {
    std::printf("[smoke][infra] %s\n", diag.c_str());
  }
  expect(infraReady, "production infrastructure snapshot is ready");
  expect(infrastructure.network()->getNodes().size() == std::size(kPlacements),
         "Network nodes were imported from PhysDesMgr");
  expect(infrastructure.network()->getMasters().size() == std::size(kMasters),
         "placed, target and getFillerMasters masters were registered");

  // --- the real checker: constructor pulls the Session design and runs the
  // full init(desMgr) UDM extraction over the fake data ----------------------
  dpl2::ipl::ImplantLayerChecker checker(infrastructure.grid(),
                                         infrastructure.network());

  // --- single production boundary ------------------------------------------
  FillerRepairEngine engine(infrastructure.grid(), infrastructure.network());
  expect(engine.init(db.design.getPhysDesMgr(), &checker, &fillerSetting),
         "FillerRepairEngine init succeeds");

  const PhysicalSnapshot cleanBefore = snapshotPhysicalData(db);
  const dpl2::ipl::CheckResult cleanPrecheck = engine.precheck();
  expect(cleanPrecheck.isLegal && cleanPrecheck.diagnostics.empty(),
         "clean placement passes gap/overlap precheck");
  expect(snapshotPhysicalData(db) == cleanBefore,
         "clean precheck does not mutate UDM");

  eUNL::PhysCellData& moved
      = db.desMgr().cells_[eUNL::LeafCellID(0, 111)];
  const eUTL::Point2D originalOrigin = moved.origin;
  moved.origin = eUTL::Point2D(eUTL::UvDist(20), eUTL::UvDist(kRowHeight));
  const PhysicalSnapshot gapBefore = snapshotPhysicalData(db);
  const dpl2::ipl::CheckResult gapPrecheck = engine.precheck();
  expect(!gapPrecheck.isLegal && hasDiagnostic(gapPrecheck.diagnostics, "Gap"),
         "gap precheck blocks opto with Gap warning");
  expect(snapshotPhysicalData(db) == gapBefore,
         "gap precheck does not mutate UDM");

  moved.origin = eUTL::Point2D(eUTL::UvDist(7), eUTL::UvDist(kRowHeight));
  const PhysicalSnapshot overlapBefore = snapshotPhysicalData(db);
  const dpl2::ipl::CheckResult overlapPrecheck = engine.precheck();
  expect(!overlapPrecheck.isLegal
             && hasDiagnostic(overlapPrecheck.diagnostics, "Overlap"),
         "overlap precheck blocks opto with Overlap warning");
  expect(snapshotPhysicalData(db) == overlapBefore,
         "overlap precheck does not mutate UDM");
  moved.origin = originalOrigin;

  const PhysicalSnapshot noRepairBefore = snapshotPhysicalData(db);
  const RepairOutcome noRepair = engine.repair(
      eUNL::LeafCellID(0, 112),
      db.design.lib_acc_.getPhysLibCell(1));  // committed TL4 overlay
  expect(noRepair.hasSolution && noRepair.changes.empty(),
         "clean target overlay succeeds with empty filler changes");
  expect(snapshotPhysicalData(db) == noRepairBefore,
         "empty-change repair does not mutate UDM");

  const auto runRepair = [&](const char* label) -> RepairOutcome {
    RepairOutcome outcome =
        engine.repair(eUNL::LeafCellID(0, 112),
                      db.design.lib_acc_.getPhysLibCell(2));  // TL4 -> TH4
    std::printf("[smoke] %s: hasSolution=%d changes=%zu\n", label,
                outcome.hasSolution ? 1 : 0, outcome.changes.size());
    for (const auto& record : outcome.changes) {
      std::printf("[smoke]   swap cell %d -> libCell %d\n",
                  record.cell_id_.getIndexValue(),
                  record.new_lib_cell_.getIndexValue());
    }
    for (const auto& diag : outcome.diagnostics) {
      std::printf("[smoke]   diag %s: %s\n", diag.status.c_str(),
                  diag.message.c_str());
    }
    return outcome;
  };

  const PhysicalSnapshot repairBefore = snapshotPhysicalData(db);
  const RepairOutcome first = runRepair("repair#1");
  expect(first.hasSolution, "repair finds a filler swap solution");
  expect(!first.changes.empty(), "solution carries FillerCellRecord changes");
  if (!first.changes.empty()) {
    const auto& record = first.changes.front();
    expect(record.op_ == dpl2::OpType::Replace, "record op is Replace");
    expect(record.new_lib_cell_.getIndexValue() == 4,
           "replacement master is the VTH filler (FH2)");
    expect(record.cell_id_.getIndexValue() == 111
               || record.cell_id_.getIndexValue() == 113,
           "swapped cell is one of the fillers next to the target");
  }
  expect(snapshotPhysicalData(db) == repairBefore,
         "repair does not mutate UDM");

  // Determinism: identical outcome on a second run over the same state.
  const RepairOutcome second = runRepair("repair#2");
  expect(second.hasSolution == first.hasSolution
             && second.changes.size() == first.changes.size(),
         "second run matches the first");
  for (size_t i = 0; i < first.changes.size() && i < second.changes.size();
       ++i) {
    expect(first.changes[i].cell_id_ == second.changes[i].cell_id_
               && first.changes[i].new_lib_cell_
                      == second.changes[i].new_lib_cell_,
           "change " + std::to_string(i) + " identical across runs");
  }
  expect(snapshotPhysicalData(db) == repairBefore,
         "repeated repair remains non-mutating");

}

// Regression: the checker copies its persistent init diagnostics into every
// overlay result twice (checkPlaceWithOverlay + the embedded region result).
// The boundary must strip every repetition, or benign init diagnostics make
// all candidates illegal and repair can never succeed.
TEST(FillerRepairProduction, PersistentCheckerDiagnosticsDoNotBlockRepair)
{
  using dpl2::fillerRepair::FillerRepairEngine;
  using dpl2::fillerRepair::RepairOutcome;

  fake_udm::DesignDb db;
  DesignSetup setup;
  setup.unusedRuleLayers = true;
  buildDesign(db, setup);

  dpl2::fillerSetting fillerSetting(&db.design);
  fillerSetting.addFillerCell("FL2 FH2 FS2");
  dpl2::RepairInfrastructure infrastructure;
  ASSERT_TRUE(infrastructure.build(db.design.getPhysDesMgr(), allLeafCells(),
                                   fillerSetting,
                                   db.design.lib_acc_.getPhysLibCell(2)));
  dpl2::ipl::ImplantLayerChecker checker(infrastructure.grid(),
                                         infrastructure.network());
  // Precondition: the unused VTUL layers left persistent init diagnostics.
  ASSERT_FALSE(checker.getDiags().empty());

  FillerRepairEngine engine(infrastructure.grid(), infrastructure.network());
  ASSERT_TRUE(engine.init(db.design.getPhysDesMgr(), &checker,
                          &fillerSetting));
  const RepairOutcome outcome = engine.repair(
      eUNL::LeafCellID(0, 112), db.design.lib_acc_.getPhysLibCell(2));
  EXPECT_TRUE(outcome.hasSolution);
  ASSERT_FALSE(outcome.changes.empty());
  EXPECT_EQ(outcome.changes.front().new_lib_cell_.getIndexValue(), 4);
}

// Regression: a configured filler master the Network never imported means the
// snapshot was built against different inputs -> init must fail, not shrink
// the candidate universe silently.
TEST(FillerRepairProduction, ConfiguredMasterMissingFromNetworkFailsInit)
{
  using dpl2::fillerRepair::FillerRepairEngine;

  fake_udm::DesignDb db;
  buildDesign(db);

  dpl2::fillerSetting infraSetting(&db.design);
  infraSetting.addFillerCell("FL2 FH2 FS2");
  dpl2::RepairInfrastructure infrastructure;
  ASSERT_TRUE(infrastructure.build(db.design.getPhysDesMgr(), allLeafCells(),
                                   infraSetting,
                                   db.design.lib_acc_.getPhysLibCell(2)));
  dpl2::ipl::ImplantLayerChecker checker(infrastructure.grid(),
                                         infrastructure.network());

  // FX2 exists in the library but was never registered into the Network.
  dpl2::fillerSetting engineSetting(&db.design);
  engineSetting.addFillerCell("FL2 FH2 FS2 FX2");
  FillerRepairEngine engine(infrastructure.grid(), infrastructure.network());
  EXPECT_FALSE(engine.init(db.design.getPhysDesMgr(), &checker,
                           &engineSetting));
}

// Regression: an empty filler allow list can never produce a swap, so both
// the snapshot builder and the engine must error out instead of building a
// state that only fails later.
TEST(FillerRepairProduction, EmptyFillerAllowListErrorsOut)
{
  using dpl2::fillerRepair::FillerRepairEngine;

  fake_udm::DesignDb db;
  buildDesign(db);

  // Infrastructure level: build() refuses an empty allow list.
  dpl2::fillerSetting emptySetting(&db.design);
  dpl2::RepairInfrastructure rejected;
  EXPECT_FALSE(rejected.build(db.design.getPhysDesMgr(), allLeafCells(),
                              emptySetting,
                              db.design.lib_acc_.getPhysLibCell(2)));
  EXPECT_FALSE(rejected.diagnostics().empty());

  // Engine level: a good snapshot + an empty allow list still fails init.
  dpl2::fillerSetting goodSetting(&db.design);
  goodSetting.addFillerCell("FL2 FH2 FS2");
  dpl2::RepairInfrastructure infrastructure;
  ASSERT_TRUE(infrastructure.build(db.design.getPhysDesMgr(), allLeafCells(),
                                   goodSetting,
                                   db.design.lib_acc_.getPhysLibCell(2)));
  dpl2::ipl::ImplantLayerChecker checker(infrastructure.grid(),
                                         infrastructure.network());
  FillerRepairEngine engine(infrastructure.grid(), infrastructure.network());
  EXPECT_FALSE(engine.init(db.design.getPhysDesMgr(), &checker,
                           &emptySetting));
}

// Regression: after a missing or failed init() every public API fails closed.
TEST(FillerRepairProduction, FailedInitFailsClosed)
{
  using dpl2::fillerRepair::FillerRepairEngine;
  using dpl2::fillerRepair::RepairOutcome;

  fake_udm::DesignDb db;
  buildDesign(db);

  dpl2::fillerSetting fillerSetting(&db.design);
  fillerSetting.addFillerCell("FL2 FH2 FS2");
  dpl2::RepairInfrastructure infrastructure;
  ASSERT_TRUE(infrastructure.build(db.design.getPhysDesMgr(), allLeafCells(),
                                   fillerSetting,
                                   db.design.lib_acc_.getPhysLibCell(2)));

  const auto expectClosed = [&](FillerRepairEngine& engine,
                                const char* when) {
    SCOPED_TRACE(when);
    const dpl2::ipl::CheckResult precheck = engine.precheck();
    EXPECT_FALSE(precheck.isLegal);
    EXPECT_TRUE(hasDiagnostic(precheck.diagnostics,
                              "precheck_not_initialized"));
    const RepairOutcome outcome = engine.repair(
        eUNL::LeafCellID(0, 112), db.design.lib_acc_.getPhysLibCell(2));
    EXPECT_FALSE(outcome.hasSolution);
    EXPECT_TRUE(outcome.changes.empty());
    EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "engine_not_initialized"));
  };

  FillerRepairEngine engine(infrastructure.grid(), infrastructure.network());
  expectClosed(engine, "before any init()");
  EXPECT_FALSE(engine.init(db.design.getPhysDesMgr(), nullptr,
                           &fillerSetting));
  expectClosed(engine, "after failed init()");
}

// Regression: the shared-x-frame validation must baseline on the first
// NON-PAD row. A pad row anywhere (any origin) is fine; a misaligned
// standard row must still be refused even when a pad row comes first.
TEST(FillerRepairProduction, RowOriginCheckUsesFirstNonPadRow)
{
  using dpl2::fillerRepair::FillerRepairEngine;

  const auto initWith = [](const DesignSetup& setup) {
    fake_udm::DesignDb db;
    buildDesign(db, setup);
    dpl2::fillerSetting fillerSetting(&db.design);
    fillerSetting.addFillerCell("FL2 FH2 FS2");
    dpl2::RepairInfrastructure infrastructure;
    if (!infrastructure.build(db.design.getPhysDesMgr(), allLeafCells(),
                              fillerSetting,
                              db.design.lib_acc_.getPhysLibCell(2))) {
      return false;
    }
    dpl2::ipl::ImplantLayerChecker checker(infrastructure.grid(),
                                           infrastructure.network());
    FillerRepairEngine engine(infrastructure.grid(),
                              infrastructure.network());
    return engine.init(db.design.getPhysDesMgr(), &checker, &fillerSetting);
  };

  DesignSetup padOnly;
  padOnly.padRowFirst = true;
  padOnly.padRowOriginX = 5;  // pad rows may sit anywhere
  EXPECT_TRUE(initWith(padOnly));

  DesignSetup misaligned = padOnly;
  misaligned.rowOriginX = {0, 0, 3};  // one standard row off the shared frame
  EXPECT_FALSE(initWith(misaligned));
}
