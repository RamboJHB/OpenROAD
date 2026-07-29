// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Repository-local checker/engine regression. Fixture
// construction uses the adjacent fake-UDM provider; this suite is deliberately
// outside the migration payload.

#include "E2ETestProvider.h"

#include <algorithm>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <drc/ImplantLayerChecker.h>
#include <infrastructure/Grid.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

namespace frt = dpl2::fillerRepair::test;

namespace {

constexpr const char* kDefaultFillers = "FL2 FH2 FS2";
constexpr const char* kFillersWithExtra = "FL2 FH2 FS2 FX2";

bool hasDiagnostic(const std::vector<dpl2::ipl::Diagnostic>& diagnostics,
                   const std::string& status)
{
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [&](const dpl2::ipl::Diagnostic& diagnostic) {
                       return diagnostic.status == status;
                     });
}

std::string diagnosticText(
    const std::vector<dpl2::ipl::Diagnostic>& diagnostics)
{
  std::string text;
  for (const dpl2::ipl::Diagnostic& diagnostic : diagnostics) {
    text += diagnostic.status + ": " + diagnostic.message + "\n";
  }
  return text;
}


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

// addMaster dereferences the edge-type table unconditionally; these cases
// exercise implant DRC, which reads master geometry only.
const dpl2::EdgeTypeTable& noEdgeTypes()
{
  static const dpl2::EdgeTypeTable kTable;
  return kTable;
}

bool syncInfrastructureNode(frt::E2ETestDesign& design,
                            dpl2::Grid* grid,
                            dpl2::Network* network,
                            frt::CellRole role)
{
  if (grid == nullptr || network == nullptr || design.desMgr() == nullptr) {
    return false;
  }
  const eUNL::LeafCellID cellId = design.cell(role);
  dpl2::Node* node = network->getNode(cellId);
  const eUNL::PhysCell cell = design.desMgr()->getPhysCell(cellId);
  if (node == nullptr || !cell.isValid()) {
    return false;
  }
  const eLIB::PhysLibCell& master = cell.getPhysMaster();
  return network->addMaster(master, grid, &noEdgeTypes()) != nullptr
         && network->updateNode(node, design.desMgr(), master);
}

struct LayoutCase
{
  const char* name;
  frt::DesignSetup setup;
};

LayoutCase canonicalLayout()
{
  return {"Canonical", {}};
}

LayoutCase shiftedLayout()
{
  frt::DesignSetup setup;
  setup.rowOriginX.fill(4);
  return {"ShiftedOrigin", setup};
}

LayoutCase farShiftedLayout()
{
  frt::DesignSetup setup;
  setup.rowOriginX.fill(11);
  return {"FarShiftedOrigin", setup};
}

class ProviderObjects
{
 public:
  explicit ProviderObjects(const frt::DesignSetup& setup,
                           bool buildInfrastructure = true)
      : provider_(frt::makeE2ETestProvider()), setup_(setup)
  {
    if (provider_ == nullptr) {
      return;
    }
    design_ = provider_->createDesign(setup_);
    if (design_ == nullptr || !buildInfrastructure) {
      return;
    }
    infrastructure_ = provider_->createInfrastructure(*design_, setup_);
  }

  bool hasDesign() const { return design_ != nullptr; }
  bool hasInfrastructure() const { return infrastructure_ != nullptr; }
  frt::E2ETestProvider& provider() { return *provider_; }
  frt::E2ETestDesign& design() { return *design_; }
  frt::E2ETestInfrastructure& infrastructure() { return *infrastructure_; }

 private:
  std::unique_ptr<frt::E2ETestProvider> provider_;
  frt::DesignSetup setup_;
  std::unique_ptr<frt::E2ETestDesign> design_;
  std::unique_ptr<frt::E2ETestInfrastructure> infrastructure_;
};

class EngineHarness
{
 public:
  explicit EngineHarness(const frt::DesignSetup& setup)
      : objects_(setup)
  {
    if (!objects_.hasDesign() || !objects_.hasInfrastructure()) {
      return;
    }
    filler_setting_ = std::make_unique<dpl2::fillerSetting>(
        objects_.design().design());
    filler_setting_->addFillerCell(kDefaultFillers);
    engine_ = std::make_unique<dpl2::fillerRepair::FillerRepairEngine>(
        objects_.infrastructure().grid(), objects_.infrastructure().network());
    engine_ready_ = engine_->init(objects_.design().desMgr(), *filler_setting_);
  }

  bool engineReady() const { return engine_ready_; }
  frt::E2ETestDesign& design() { return objects_.design(); }
  dpl2::fillerRepair::FillerRepairEngine& engine() { return *engine_; }
  dpl2::Network& network() { return *objects_.infrastructure().network(); }
  bool syncInfrastructureCell(frt::CellRole role)
  {
    return syncInfrastructureNode(design(),
                                  objects_.infrastructure().grid(),
                                  objects_.infrastructure().network(),
                                  role);
  }
  bool update()
  {
    return update(design().desMgr());
  }
  bool update(eUNL::PhysDesMgr* desMgr)
  {
    engine_ready_ = engine_->update(desMgr, *filler_setting_);
    return engine_ready_;
  }

 private:
  ProviderObjects objects_;
  std::unique_ptr<dpl2::fillerSetting> filler_setting_;
  std::unique_ptr<dpl2::fillerRepair::FillerRepairEngine> engine_;
  bool engine_ready_ = false;
};

class CheckerHarness
{
 public:
  // presetContext=false leaves the repair context unset so a test can drive
  // the production path instead: the registered setting provider.
  explicit CheckerHarness(const frt::DesignSetup& setup,
                          bool presetContext = true)
      : objects_(setup)
  {
    if (!objects_.hasDesign() || !objects_.hasInfrastructure()) {
      return;
    }
    filler_setting_ = std::make_unique<dpl2::fillerSetting>(
        objects_.design().design());
    filler_setting_->addFillerCell(kDefaultFillers);
    checker_ = std::make_unique<dpl2::ipl::ImplantLayerChecker>(
        objects_.infrastructure().grid(),
        objects_.infrastructure().network());
    // The checker self-initializes from the activated Session design; the
    // repair engine is created lazily on the first failing check. The seam
    // presets what production obtains from the DePlace-registered provider.
    if (presetContext) {
      checker_->setFillerRepairContext(objects_.design().desMgr(),
                                       filler_setting_.get());
    }
    checker_ready_ = true;
  }

  const dpl2::fillerSetting* fillerSetting() const
  {
    return filler_setting_.get();
  }

  bool checkerReady() const { return checker_ready_; }
  frt::E2ETestDesign& design() { return objects_.design(); }
  dpl2::ipl::ImplantLayerChecker& checker() { return *checker_; }

  bool syncInfrastructureCell(frt::CellRole role)
  {
    return syncInfrastructureNode(design(),
                                  objects_.infrastructure().grid(),
                                  objects_.infrastructure().network(),
                                  role);
  }

  // Drops the lazily-built repair engine so the next failing check rebuilds
  // its snapshot against the changed design.
  bool update()
  {
    checker_->setFillerRepairContext(design().desMgr(), filler_setting_.get());
    return true;
  }

  bool setTargetMaster(frt::MasterRole role)
  {
    dpl2::Grid* grid = objects_.infrastructure().grid();
    dpl2::Network* network = objects_.infrastructure().network();
    const eLIB::PhysLibCell& master = design().master(role);
    dpl2::Node* target = network->getNode(
        design().cell(frt::CellRole::Target));
    return target != nullptr
           && network->addMaster(master, grid, &noEdgeTypes()) != nullptr
           && network->updateNode(target, design().desMgr(), master);
  }

  // Opto-style entry: the caller owns the record vector and the checker
  // appends repair data into it (it keeps no member copy).
  bool checkTarget()
  {
    fc_record_.clear();
    dpl2::Grid* grid = objects_.infrastructure().grid();
    dpl2::Network* network = objects_.infrastructure().network();
    dpl2::Node* target = network->getNode(
        design().cell(frt::CellRole::Target));
    return target != nullptr
           && checker_->check(target,
                              grid->gridX(target),
                              grid->gridSnapDownY(target),
                              target->getOrient(),
                              fc_record_);
  }

  const std::vector<dpl2::FillerCellRecord>& fillerChanges() const
  {
    return fc_record_;
  }

 private:
  ProviderObjects objects_;
  std::unique_ptr<dpl2::fillerSetting> filler_setting_;
  std::unique_ptr<dpl2::ipl::ImplantLayerChecker> checker_;
  std::vector<dpl2::FillerCellRecord> fc_record_;
  bool checker_ready_ = false;
};

class FillerRepairEngineE2E
    : public ::testing::TestWithParam<LayoutCase>
{
};

// Stand-in for the provider DePlace registers. The pointer it returns is
// swapped mid-test to model set_filler_option running after the first
// failing check.
const dpl2::fillerSetting* g_providedSetting = nullptr;
const dpl2::fillerSetting* provideTestSetting()
{
  return g_providedSetting;
}

}  // namespace



TEST(FillerRepairInitializationDiagnostics,
     MixedHeightPhysRowsUseSmallestBaseHeight)
{
  frt::DesignSetup setup;
  setup.overlappingDoubleHeightRow = true;
  EngineHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto outcome = harness.engine().repair(
      harness.design().cell(frt::CellRole::Target),
      harness.design().master(frt::MasterRole::TargetNew));
  ASSERT_TRUE(outcome.hasSolution) << diagnosticText(outcome.diagnostics);
  ASSERT_EQ(outcome.changes.size(), 1U);
  EXPECT_EQ(outcome.changes.front().new_lib_cell_,
            harness.design().master(frt::MasterRole::RepairFiller)
                .getLibCellId());
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST(FillerRepairInitializationDiagnostics,
     DebugTranscriptReportsHaloSourceAndSnapshotFrames)
{
  frt::DesignSetup setup;
  setup.implantRuleWidth = 2;
  ProviderObjects objects(setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  engine.setDebugLogging(true);

  testing::internal::CaptureStdout();
  const bool initialized = engine.init(objects.design().desMgr(), setting);
  dpl2::fillerRepair::RepairOutcome outcome;
  if (initialized) {
    outcome = engine.repair(
        objects.design().cell(frt::CellRole::Target),
        objects.design().master(frt::MasterRole::TargetNew));
  }
  const std::string transcript = testing::internal::GetCapturedStdout();

  ASSERT_TRUE(initialized) << transcript;
  EXPECT_TRUE(outcome.hasSolution) << diagnosticText(outcome.diagnostics);
  EXPECT_NE(transcript.find("[fr][engine] default halo source:"),
            std::string::npos);
  // Rule reach comes from the checker alone; the widest placed master (6)
  // beats it here, so the halo is 2 * 6.
  EXPECT_NE(transcript.find("kind=PLACED_MASTER_WIDTH"), std::string::npos);
  EXPECT_NE(transcript.find("widestPlaced{"), std::string::npos);
  EXPECT_NE(transcript.find("dbu=6}"), std::string::npos);
  EXPECT_NE(transcript.find("checkerReach{sites="), std::string::npos);
  EXPECT_NE(transcript.find("defaultHaloX=12"), std::string::npos);
  EXPECT_NE(transcript.find("guard=[-4,24) rows[1,3]"), std::string::npos);
  EXPECT_NE(transcript.find("[fr][engine] snapshot frame: request{"),
            std::string::npos);
  EXPECT_NE(transcript.find("engineSnapshot{"), std::string::npos);
  EXPECT_NE(transcript.find("network{"), std::string::npos);
  EXPECT_NE(transcript.find("physical{"), std::string::npos);
  EXPECT_NE(transcript.find("matchingPhysRows=["), std::string::npos);
}






TEST_P(FillerRepairEngineE2E, HardMacroIsImportedAndCoversLegalSites)
{
  frt::DesignSetup setup = GetParam().setup;
  setup.row0ThirdHardMacro = true;
  EngineHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());
  const auto macroId = harness.design().cell(frt::CellRole::Row0ThirdCell);
  const dpl2::Node* macro = harness.network().getNode(macroId);
  ASSERT_NE(macro, nullptr);
  EXPECT_TRUE(macro->isBlock());

  const auto& replacement
      = harness.design().master(frt::MasterRole::TargetNew);
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
  const auto outcome = harness.engine().repair(macroId, replacement);
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "TargetNotStdCell"));
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
}

TEST_P(FillerRepairEngineE2E,
       RepairPrecheckCountsUpperRowOfMultiRowHardMacro)
{
  frt::DesignSetup setup = GetParam().setup;
  setup.row0ThirdHardMacro = true;
  EngineHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());

  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto outcome = harness.engine().repair(
      harness.design().cell(frt::CellRole::Target),
      harness.design().master(frt::MasterRole::TargetOld));
  EXPECT_TRUE(outcome.hasSolution);
  EXPECT_FALSE(hasDiagnostic(outcome.diagnostics, "Gap"));
  EXPECT_FALSE(hasDiagnostic(outcome.diagnostics, "PrecheckFailed"));
  EXPECT_EQ(harness.design().snapshot(), before);
}






TEST_P(FillerRepairEngineE2E, RepairPrecheckFailureWarnsAndBlocks)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  // The gap must fall inside the target's influence rows (target is in row 2;
  // repair narrows its internal precheck to the guard rows around it). Row 1
  // is a coupled guard row, so a gap there still blocks the repair.
  harness.design().moveCell(frt::CellRole::Row1TailFiller,
                            harness.design().rowOriginX(1) + frt::kRowSites,
                            frt::kRowHeight);
  ASSERT_TRUE(
      harness.syncInfrastructureCell(frt::CellRole::Row1TailFiller));
  ASSERT_TRUE(harness.update());
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto& replacement
      = harness.design().master(frt::MasterRole::TargetNew);
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
  const auto outcome = harness.engine().repair(
      harness.design().cell(frt::CellRole::Target), replacement);
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "Gap"));
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "PrecheckFailed"));
  EXPECT_FALSE(hasDiagnostic(outcome.diagnostics, "Overlap"));
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E, RepairIgnoresGapOutsideInfluenceRows)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  // A gap in row 0 is outside the target's influence rows (guard rows 1..3
  // for a target in row 2). The regional precheck must not block the repair,
  // which still finds its local filler swap; whole-design placement legality
  // is infrastructure's own gate, not this engine's.
  harness.design().moveCell(frt::CellRole::Row0TailFiller,
                            harness.design().rowOriginX(0) + frt::kRowSites,
                            0);
  ASSERT_TRUE(
      harness.syncInfrastructureCell(frt::CellRole::Row0TailFiller));
  ASSERT_TRUE(harness.update());
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto outcome = harness.engine().repair(
      harness.design().cell(frt::CellRole::Target),
      harness.design().master(frt::MasterRole::TargetNew));
  EXPECT_TRUE(outcome.hasSolution);
  EXPECT_FALSE(hasDiagnostic(outcome.diagnostics, "PrecheckFailed"));
  ASSERT_EQ(outcome.changes.size(), 1U);
  EXPECT_EQ(outcome.changes.front().new_lib_cell_,
            harness.design().master(frt::MasterRole::RepairFiller)
                .getLibCellId());
  EXPECT_EQ(harness.design().snapshot(), before);
}

// --- Grid::getBoundingBox ---------------------------------------------------
// Fixture rows (core-relative, siteWidth 1, rowHeight 8, 20 sites, 5 rows):
//   row 2 (target row): [0,6) [6,8) [8,12)=target [12,14) [14,20)
//   rows 0/1/3/4:       [0,6) [6,12) [12,18) [18,20)
// This needs only a built Grid -- no engine, no checker, no init.
namespace {

::Rect rect(int64_t xl, int64_t yl, int64_t xh, int64_t yh)
{
  return ::Rect(eUTL::UvDist(xl), eUTL::UvDist(yl),
                eUTL::UvDist(xh), eUTL::UvDist(yh));
}

std::string showRect(const ::Rect& r)
{
  return "[" + std::to_string(r.getXL().getStorage()) + ","
       + std::to_string(r.getYL().getStorage()) + ","
       + std::to_string(r.getXH().getStorage()) + ","
       + std::to_string(r.getYH().getStorage()) + ")";
}

}  // namespace

TEST_P(FillerRepairEngineE2E, BoundingBoxCountsCellsAcrossStdCells)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasInfrastructure());
  const dpl2::Grid* grid = objects.infrastructure().grid();

  // One ring around the target's own site band.
  const ::Rect out = grid->getBoundingBox(rect(8, 16, 12, 24), 1);

  // Rows 1..3 -> y [8,32). On the target row one cell each side gives
  // [6,14); on rows 1 and 3 the single ring step lands on cells at [0,6) and
  // [12,18) -- both STD cells, counted as ring members rather than treated as
  // stoppers -- so x reaches [0,18).
  EXPECT_EQ(showRect(out), showRect(rect(0, 8, 18, 32)));
}

TEST_P(FillerRepairEngineE2E, BoundingBoxDefaultsToThreeRingsAndClamps)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasInfrastructure());
  const dpl2::Grid* grid = objects.infrastructure().grid();

  // Three rings reach past every cell in this 5-row fixture; the result
  // clamps to the core instead of running off the placeable area.
  const ::Rect out = grid->getBoundingBox(rect(8, 16, 12, 24));
  EXPECT_EQ(showRect(out),
            showRect(rect(0, 0, frt::kRowSites * frt::kSiteWidth,
                          frt::kStandardRows * frt::kRowHeight)));
}

TEST_P(FillerRepairEngineE2E, BoundingBoxIsMonotonicInRings)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasInfrastructure());
  const dpl2::Grid* grid = objects.infrastructure().grid();

  const ::Rect in = rect(8, 16, 12, 24);
  const ::Rect zero = grid->getBoundingBox(in, 0);
  const ::Rect one = grid->getBoundingBox(in, 1);
  // Zero rings still snaps to whole cells/rows, and growth never shrinks.
  EXPECT_LE(zero.getXL().getStorage(), in.getXL().getStorage());
  EXPECT_GE(zero.getXH().getStorage(), in.getXH().getStorage());
  EXPECT_LE(one.getXL().getStorage(), zero.getXL().getStorage());
  EXPECT_GE(one.getXH().getStorage(), zero.getXH().getStorage());
  EXPECT_LE(one.getYL().getStorage(), zero.getYL().getStorage());
  EXPECT_GE(one.getYH().getStorage(), zero.getYH().getStorage());
}

TEST_P(FillerRepairEngineE2E, CleanTargetOverlayReturnsNoChanges)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  const auto before = harness.design().snapshot();
  const auto outcome = harness.engine().repair(
      harness.design().cell(frt::CellRole::Target),
      harness.design().master(frt::MasterRole::TargetOld));
  EXPECT_TRUE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E, ViolatingTargetOverlayFindsFillerSwap)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  const auto& targetMaster
      = harness.design().master(frt::MasterRole::TargetNew);
  EXPECT_EQ(harness.network().getMasterId(targetMaster.getLibCellId()), -1);
  const auto before = harness.design().snapshot();
  const auto outcome = harness.engine().repair(
      harness.design().cell(frt::CellRole::Target), targetMaster);
  ASSERT_TRUE(outcome.hasSolution);
  ASSERT_EQ(outcome.changes.size(), 1U);
  const auto& change = outcome.changes.front();
  EXPECT_EQ(change.op_, dpl2::OpType::Replace);
  EXPECT_EQ(change.new_lib_cell_,
            harness.design().master(frt::MasterRole::RepairFiller)
                .getLibCellId());
  EXPECT_TRUE(
      change.cell_id_ == harness.design().cell(frt::CellRole::TargetLeftFiller)
      || change.cell_id_
             == harness.design().cell(frt::CellRole::TargetRightFiller));
  EXPECT_GE(harness.network().getMasterId(targetMaster.getLibCellId()), 0);
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E,
       CheckerEntryReturnsRepairChangesWithoutMutation)
{
  CheckerHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.checkerReady());
  const auto before = harness.design().snapshot();
  ASSERT_TRUE(harness.setTargetMaster(frt::MasterRole::TargetNew));

  ASSERT_TRUE(harness.checkTarget());
  const auto& changes = harness.fillerChanges();
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes.front().op_, dpl2::OpType::Replace);
  EXPECT_EQ(changes.front().new_lib_cell_,
            harness.design().master(frt::MasterRole::RepairFiller)
                .getLibCellId());
  EXPECT_EQ(harness.design().snapshot(), before);

  // A later failed check must clear the previously accepted overlay before
  // returning; callers can never observe stale changes from the first check.
  // The defect must fall in the target's influence rows (row 1 couples to the
  // row-2 target) for the narrowed internal precheck to block.
  harness.design().moveCell(
      frt::CellRole::Row1TailFiller,
      harness.design().rowOriginX(1) + frt::kRowSites,
      frt::kRowHeight);
  const auto beforeFailedCheck = harness.design().snapshot();
  EXPECT_FALSE(harness.checkTarget());
  EXPECT_TRUE(harness.fillerChanges().empty());
  EXPECT_EQ(harness.design().snapshot(), beforeFailedCheck);
}

TEST_P(FillerRepairEngineE2E, CheckerEntryCleanCandidateHasNoChanges)
{
  CheckerHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.checkerReady());
  const auto before = harness.design().snapshot();
  ASSERT_TRUE(harness.setTargetMaster(frt::MasterRole::TargetOld));

  EXPECT_TRUE(harness.checkTarget());
  EXPECT_TRUE(harness.fillerChanges().empty());
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E,
       CheckerEntryPrecheckFailureReturnsNoPartialChanges)
{
  CheckerHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.checkerReady());
  // Gap inside the target's influence rows (row 1 couples to the row-2 target)
  // so the narrowed internal precheck blocks the checker-entry repair.
  harness.design().moveCell(
      frt::CellRole::Row1TailFiller,
      harness.design().rowOriginX(1) + frt::kRowSites,
      frt::kRowHeight);
  ASSERT_TRUE(
      harness.syncInfrastructureCell(frt::CellRole::Row1TailFiller));
  ASSERT_TRUE(harness.update());
  const auto before = harness.design().snapshot();
  ASSERT_TRUE(harness.setTargetMaster(frt::MasterRole::TargetNew));

  EXPECT_FALSE(harness.checkTarget());
  EXPECT_TRUE(harness.fillerChanges().empty());
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E,
       UnknownTargetDoesNotRegisterReplacementMaster)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  const auto& replacement
      = harness.design().master(frt::MasterRole::TargetNew);
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
  const auto outcome
      = harness.engine().repair(eUNL::LeafCellID(0, 9999), replacement);
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "UnknownTarget"));
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
}

TEST_P(FillerRepairEngineE2E,
       SizeMismatchDoesNotRegisterReplacementMaster)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  const auto& replacement
      = harness.design().master(frt::MasterRole::MismatchedTarget);
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
  const auto outcome = harness.engine().repair(
      harness.design().cell(frt::CellRole::Target), replacement);
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "TargetSizeMismatch"));
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
}

TEST_P(FillerRepairEngineE2E,
       UpdateFollowsInfrastructureNodeSynchronization)
{
  // Infrastructure owns Network<->UDM coherence: the engine consumes the
  // synchronized state as-is (no cross-validation of its own). After the
  // owner replaces a master and syncs the Node, update() adopts it; a null
  // PhysDesMgr still fails closed.
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  const auto targetId = harness.design().cell(frt::CellRole::Target);
  const auto& replacement
      = harness.design().master(frt::MasterRole::TargetNew);
  const dpl2::Node* target = harness.network().getNode(targetId);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(target->getMaster(), nullptr);

  ASSERT_TRUE(harness.update());
  harness.design().replaceCellMaster(frt::CellRole::Target,
                                     frt::MasterRole::TargetNew);
  ASSERT_TRUE(harness.syncInfrastructureCell(frt::CellRole::Target));
  ASSERT_TRUE(harness.update());
  target = harness.network().getNode(targetId);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(target->getMaster(), nullptr);
  EXPECT_EQ(target->getMaster()->getDbMaster(), replacement.getLibCellId());

  EXPECT_FALSE(harness.update(nullptr));
  const auto failedRepair = harness.engine().repair(targetId, replacement);
  EXPECT_FALSE(failedRepair.hasSolution);
  EXPECT_TRUE(failedRepair.changes.empty());
  EXPECT_TRUE(
      hasDiagnostic(failedRepair.diagnostics, "engine_not_initialized"));
}

TEST_P(FillerRepairEngineE2E,
       RepeatedRepairIsDeterministicAndNonMutating)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  const auto before = harness.design().snapshot();
  const auto& newMaster = harness.design().master(frt::MasterRole::TargetNew);
  const auto target = harness.design().cell(frt::CellRole::Target);
  const auto first = harness.engine().repair(target, newMaster);
  const auto second = harness.engine().repair(target, newMaster);
  EXPECT_TRUE(first.hasSolution);
  EXPECT_EQ(second.hasSolution, first.hasSolution);
  EXPECT_TRUE(sameChanges(first.changes, second.changes));
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E,
       PersistentCheckerDiagnosticsDoNotBlockRepair)
{
  frt::DesignSetup setup = GetParam().setup;
  setup.unusedRuleLayers = true;
  EngineHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());
  const auto outcome = harness.engine().repair(
      harness.design().cell(frt::CellRole::Target),
      harness.design().master(frt::MasterRole::TargetNew));
  ASSERT_TRUE(outcome.hasSolution);
  ASSERT_FALSE(outcome.changes.empty());
  EXPECT_EQ(outcome.changes.front().new_lib_cell_,
            harness.design().master(frt::MasterRole::RepairFiller)
                .getLibCellId());
}


TEST_P(FillerRepairEngineE2E, ConfiguredMastersAreRegisteredByEngine)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  const auto extraId
      = objects.design().master(frt::MasterRole::ExtraUninstantiatedFiller)
            .getLibCellId();
  EXPECT_EQ(objects.infrastructure().network()->getMasterId(extraId), -1);
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kFillersWithExtra);
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  EXPECT_TRUE(engine.init(objects.design().desMgr(), setting));
  EXPECT_GE(objects.infrastructure().network()->getMasterId(extraId), 0);
}

TEST_P(FillerRepairEngineE2E, EmptyFillerAllowListErrorsOut)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::fillerSetting emptySetting(objects.design().design());
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  EXPECT_FALSE(engine.init(objects.design().desMgr(), emptySetting));
  const auto repair = engine.repair(
      objects.design().cell(frt::CellRole::Target),
      objects.design().master(frt::MasterRole::TargetNew));
  EXPECT_FALSE(repair.hasSolution);
  EXPECT_TRUE(hasDiagnostic(repair.diagnostics, "empty_filler_allow_list"));
}

TEST_P(FillerRepairEngineE2E, MissingInfrastructureErrorsOut)
{
  ProviderObjects objects(GetParam().setup, false);
  ASSERT_TRUE(objects.hasDesign());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  dpl2::fillerRepair::FillerRepairEngine engine(nullptr, nullptr);
  EXPECT_FALSE(engine.init(objects.design().desMgr(), setting));
  const auto repair = engine.repair(
      objects.design().cell(frt::CellRole::Target),
      objects.design().master(frt::MasterRole::TargetNew));
  EXPECT_FALSE(repair.hasSolution);
  EXPECT_TRUE(hasDiagnostic(repair.diagnostics, "missing_infrastructure"));
}

// The engine binds its private oracle checker to the PhysDesMgr it is given,
// so a design that is not Session's current one is repaired normally. This
// used to be a fatal init diagnostic ("active_design_mismatch") purely
// because the oracle took its design from the global Session.
TEST_P(FillerRepairEngineE2E, InitBindsRequestedDesignNotSessionCurrent)
{
  auto provider = frt::makeE2ETestProvider();
  ASSERT_NE(provider, nullptr);
  auto requested = provider->createDesign(GetParam().setup);
  ASSERT_NE(requested, nullptr);
  auto infrastructure
      = provider->createInfrastructure(*requested, GetParam().setup);
  ASSERT_NE(infrastructure, nullptr);
  // A different design becomes Session's current one AFTER infrastructure was
  // built for `requested`.
  auto active = provider->createDesign({});
  ASSERT_NE(active, nullptr);
  active->activate();
  dpl2::fillerSetting setting(requested->design());
  setting.addFillerCell(kDefaultFillers);
  dpl2::fillerRepair::FillerRepairEngine engine(infrastructure->grid(),
                                                 infrastructure->network());
  ASSERT_TRUE(engine.init(requested->desMgr(), setting));
  const auto repair = engine.repair(
      requested->cell(frt::CellRole::Target),
      requested->master(frt::MasterRole::TargetNew));
  EXPECT_TRUE(repair.hasSolution);
  EXPECT_FALSE(hasDiagnostic(repair.diagnostics, "active_design_mismatch"));
}

// Lazy init means the first failing check can arrive before
// set_filler_option has run. That is "not configured yet", not a failure:
// latching it would silently disable repair for the rest of the run even
// after the configuration shows up.
TEST_P(FillerRepairEngineE2E, UnconfiguredRepairRetriesOnceConfigured)
{
  CheckerHarness harness(GetParam().setup, /*presetContext=*/false);
  ASSERT_TRUE(harness.checkerReady());
  g_providedSetting = nullptr;
  dpl2::ipl::ImplantLayerChecker::setFillerRepairSettingProvider(
      provideTestSetting);
  ASSERT_TRUE(harness.setTargetMaster(frt::MasterRole::TargetNew));

  // No setting yet: the check fails and no repair is attempted.
  EXPECT_FALSE(harness.checkTarget());
  EXPECT_TRUE(harness.fillerChanges().empty());

  // set_filler_option lands; the very next failing check must build the
  // engine and repair.
  g_providedSetting = harness.fillerSetting();
  EXPECT_TRUE(harness.checkTarget());
  EXPECT_EQ(harness.fillerChanges().size(), 1U);

  dpl2::ipl::ImplantLayerChecker::setFillerRepairSettingProvider(nullptr);
  g_providedSetting = nullptr;
}

TEST_P(FillerRepairEngineE2E, FailedInitFailsClosed)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  const auto expectClosed = [&](const char* phase) {
    SCOPED_TRACE(phase);
    const auto repair = engine.repair(
        objects.design().cell(frt::CellRole::Target),
        objects.design().master(frt::MasterRole::TargetNew));
    EXPECT_FALSE(repair.hasSolution);
    EXPECT_TRUE(repair.changes.empty());
    EXPECT_TRUE(hasDiagnostic(repair.diagnostics, "engine_not_initialized"));
  };
  expectClosed("before init");
  EXPECT_FALSE(engine.init(nullptr, setting));
  expectClosed("after failed init");
}

TEST_P(FillerRepairEngineE2E, EngineUsesOneInitialization)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  ASSERT_TRUE(engine.init(objects.design().desMgr(), setting));
  EXPECT_FALSE(engine.init(objects.design().desMgr(), setting));
  const auto outcome = engine.repair(
      objects.design().cell(frt::CellRole::Target),
      objects.design().master(frt::MasterRole::TargetOld));
  EXPECT_TRUE(outcome.hasSolution);
}

// --- Grid::isFullUtil -------------------------------------------------------
// Reported as returning false on a design that is in fact full. The cause was
// on the painting side (DePlace was skipping Node::FILLER, so filler sites
// stayed empty in the grid); these pin the detection side, which is what makes
// that diagnosis sound. DePlace.cpp is destination code and is not compiled
// here, so the painting filter itself has no coverage in this repository --
// this fixture paints every node unconditionally, which is what a correct
// DePlace does.

TEST_P(FillerRepairEngineE2E, FullyOccupiedGridIsFullyUtilized)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasInfrastructure());
  EXPECT_TRUE(objects.infrastructure().grid()->isFullUtil());
}

// The exact shape of the report: a filler's sites left unpainted. If a filler
// does not count as an occupant, a full design looks unfull.
TEST_P(FillerRepairEngineE2E, UnpaintedFillerSitesMakeTheGridNotFull)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::Grid* grid = objects.infrastructure().grid();
  ASSERT_TRUE(grid->isFullUtil());

  dpl2::Node* filler = nullptr;
  for (const auto& node : objects.infrastructure().network()->getNodes()) {
    if (node->isFiller()) {
      filler = node.get();
      break;
    }
  }
  ASSERT_NE(filler, nullptr) << "fixture has no filler node";

  // Its sites are occupied by it, and only by it.
  const dpl2::Pixel* pixel
      = grid->gridPixel(grid->gridX(filler), grid->gridSnapDownY(filler));
  ASSERT_NE(pixel, nullptr);
  EXPECT_EQ(pixel->cell, filler);

  grid->erasePixel(filler);
  EXPECT_FALSE(grid->isFullUtil());
  grid->paintPixel(filler);
  EXPECT_TRUE(grid->isFullUtil());
}

// A grid with no pixels knows nothing about utilization, so it must not claim
// the design is full. It used to warn and return true.
TEST(GridIsFullUtil, EmptyGridIsNotFull)
{
  dpl2::Grid grid;
  EXPECT_FALSE(grid.isFullUtil());
}

INSTANTIATE_TEST_SUITE_P(
    FiveRowLayouts,
    FillerRepairEngineE2E,
    ::testing::Values(canonicalLayout(), shiftedLayout(), farShiftedLayout()),
    [](const ::testing::TestParamInfo<LayoutCase>& info) {
      return info.param.name;
    });


