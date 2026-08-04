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
constexpr const char* kFillersWithExtra = "FL2 FH2 FS2 FX4";

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
    if (lhs[i].op_ != rhs[i].op_
        || lhs[i].cell_data_ != rhs[i].cell_data_
        || lhs[i].new_lib_cell_ != rhs[i].new_lib_cell_
        || lhs[i].orientation_.getValue()
               != rhs[i].orientation_.getValue()) {
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
                            const dpl2::fillerSetting& fillerSetting,
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
  return network->addMaster(
             master, fillerSetting, grid, &noEdgeTypes())
             != nullptr
         && network->updateNode(node, design.desMgr(), master);
}

dpl2::ipl::CheckRequest repairRequest(dpl2::Grid* grid,
                                      dpl2::Network* network,
                                      eUNL::LeafCellID targetCell,
                                      const eLIB::PhysLibCell& newMaster)
{
  dpl2::ipl::CheckRequest request;
  request.instanceId = network != nullptr ? network->getNodeId(targetCell) : -1;
  request.masterId
      = network != nullptr ? network->getMasterId(newMaster.getLibCellId()) : -1;
  const dpl2::Node* node
      = network != nullptr ? network->getNode(targetCell) : nullptr;
  if (grid != nullptr && node != nullptr) {
    request.rowId = grid->gridSnapDownY(node).v;
    request.colId = grid->gridX(node).v;
    request.orientation = node->getOrient();
  }
  return request;
}

dpl2::fillerRepair::RepairOutcome repairWithRequest(
    dpl2::fillerRepair::FillerRepairEngine& engine,
    dpl2::Grid* grid,
    dpl2::Network* network,
    eUNL::LeafCellID targetCell,
    const eLIB::PhysLibCell& newMaster)
{
  return engine.repair(
      repairRequest(grid, network, targetCell, newMaster));
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

dpl2::fillerRepair::RepairOutcome repairWithRequest(
    dpl2::fillerRepair::FillerRepairEngine& engine,
    ProviderObjects& objects,
    eUNL::LeafCellID targetCell,
    const eLIB::PhysLibCell& newMaster)
{
  dpl2::Grid* grid = objects.hasInfrastructure()
                         ? objects.infrastructure().grid()
                         : nullptr;
  dpl2::Network* network = objects.hasInfrastructure()
                              ? objects.infrastructure().network()
                              : nullptr;
  return repairWithRequest(engine, grid, network, targetCell, newMaster);
}

// The test infrastructure owns Network master construction, just like the
// destination. The engine may only refresh setFiller(true) on these objects.
bool registerRepairMasters(frt::E2ETestDesign& design,
                           dpl2::Grid* grid,
                           dpl2::Network* network,
                           const dpl2::fillerSetting& setting,
                           bool includeTarget = true)
{
  if (grid == nullptr || network == nullptr) {
    return false;
  }
  for (const eLIB::PhysLibCell* master : setting.getFillerPhysCells()) {
    if (master == nullptr
        || network->addMaster(*master, setting, grid, &noEdgeTypes())
               == nullptr) {
      return false;
    }
  }
  return !includeTarget
         || network->addMaster(
                design.master(frt::MasterRole::TargetNew),
                setting,
                grid,
                &noEdgeTypes()) != nullptr;
}

bool registerRepairMasters(ProviderObjects& objects,
                           const dpl2::fillerSetting& setting,
                           bool includeTarget = true)
{
  return registerRepairMasters(objects.design(),
                               objects.infrastructure().grid(),
                               objects.infrastructure().network(),
                               setting,
                               includeTarget);
}

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
    if (!registerRepairMasters(objects_, *filler_setting_)) {
      return;
    }
    engine_ = std::make_unique<dpl2::fillerRepair::FillerRepairEngine>(
        objects_.infrastructure().grid(), objects_.infrastructure().network());
    engine_ready_ = engine_->init(objects_.design().desMgr(), *filler_setting_);
  }

  bool engineReady() const { return engine_ready_; }
  frt::E2ETestDesign& design() { return objects_.design(); }
  dpl2::fillerRepair::FillerRepairEngine& engine() { return *engine_; }
  dpl2::Network& network() { return *objects_.infrastructure().network(); }
  dpl2::fillerRepair::RepairOutcome repair(
      eUNL::LeafCellID targetCell,
      const eLIB::PhysLibCell& newMaster)
  {
    return repairWithRequest(*engine_,
                             objects_.infrastructure().grid(),
                             objects_.infrastructure().network(),
                             targetCell,
                             newMaster);
  }
  bool syncInfrastructureCell(frt::CellRole role)
  {
    return syncInfrastructureNode(design(),
                                  objects_.infrastructure().grid(),
                                  objects_.infrastructure().network(),
                                  *filler_setting_,
                                  role);
  }
  bool reinitialize()
  {
    return reinitialize(design().desMgr());
  }
  bool reinitialize(eUNL::PhysDesMgr* desMgr)
  {
    engine_ = std::make_unique<dpl2::fillerRepair::FillerRepairEngine>(
        objects_.infrastructure().grid(), objects_.infrastructure().network());
    engine_ready_ = engine_->init(desMgr, *filler_setting_);
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
  // the runtime path instead: the registered setting provider.
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
    if (!registerRepairMasters(objects_, *filler_setting_, false)) {
      return;
    }
    checker_ = std::make_unique<dpl2::ipl::ImplantLayerChecker>(
        objects_.infrastructure().grid(),
        objects_.infrastructure().network());
    // The checker self-initializes from Grid's retained manager; the repair
    // engine is created lazily on the first failing check. This presets what
    // the normal flow obtains from the DePlace-registered provider.
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
                                  *filler_setting_,
                                  role);
  }

  bool resetContext()
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
           && network->addMaster(
                  master, *filler_setting_, grid, &noEdgeTypes())
                  != nullptr
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

  const std::vector<dpl2::CellChangeRecord>& fillerChanges() const
  {
    return fc_record_;
  }

 private:
  ProviderObjects objects_;
  std::unique_ptr<dpl2::fillerSetting> filler_setting_;
  std::unique_ptr<dpl2::ipl::ImplantLayerChecker> checker_;
  std::vector<dpl2::CellChangeRecord> fc_record_;
  bool checker_ready_ = false;
};

class FillerRepairEngineE2E
    : public ::testing::TestWithParam<LayoutCase>
{
};

// Stand-in for the provider DePlace registers. The fail-closed test changes
// this after an invalid first check to prove that a contract violation cannot
// silently change behaviour later.
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
  const auto outcome = harness.repair(
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
  setup.row0ThirdHardMacro = true;
  ProviderObjects objects(setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  ASSERT_TRUE(registerRepairMasters(objects, setting));
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  engine.setDebugLogging(true);

  testing::internal::CaptureStdout();
  const bool initialized = engine.init(objects.design().desMgr(), setting);
  dpl2::fillerRepair::RepairOutcome outcome;
  if (initialized) {
    outcome = repairWithRequest(engine, objects,
        objects.design().cell(frt::CellRole::Target),
        objects.design().master(frt::MasterRole::TargetNew));
  }
  const std::string transcript = testing::internal::GetCapturedStdout();

  ASSERT_TRUE(initialized) << transcript;
  EXPECT_TRUE(outcome.hasSolution) << diagnosticText(outcome.diagnostics);
  EXPECT_NE(transcript.find("[fr][engine] default halo source:"),
            std::string::npos);
  // The placed hard macro is 6 sites wide, but only configured filler
  // masters may contribute a width floor. Both that floor and checker reach
  // are 2 here, so the macro does not inflate the initial snapshot halo.
  EXPECT_NE(transcript.find("kind=CHECKER_RULE_REACH"), std::string::npos);
  EXPECT_NE(transcript.find("widestConfiguredFiller{"),
            std::string::npos);
  EXPECT_NE(transcript.find("dbu=2}"), std::string::npos);
  EXPECT_NE(transcript.find("checkerReach{sites="), std::string::npos);
  EXPECT_NE(transcript.find("defaultHaloX=2"), std::string::npos);
  EXPECT_NE(transcript.find("guard=[6,14) rows[1,3]"), std::string::npos);
  EXPECT_NE(transcript.find("[fr][engine] snapshot frame: request{"),
            std::string::npos);
  EXPECT_NE(transcript.find("engineSnapshot{"), std::string::npos);
  EXPECT_NE(transcript.find("network{"), std::string::npos);
  EXPECT_NE(transcript.find("physical{"), std::string::npos);
  EXPECT_NE(transcript.find("matchingPhysRows=["), std::string::npos);
}

TEST(FillerRepairInitializationDiagnostics,
     WidestConfiguredFillerSetsInitialHalo)
{
  frt::DesignSetup setup;
  setup.implantRuleWidth = 2;
  setup.row0ThirdHardMacro = true;
  ProviderObjects objects(setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kFillersWithExtra);
  ASSERT_TRUE(registerRepairMasters(objects, setting));
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  engine.setDebugLogging(true);

  testing::internal::CaptureStdout();
  const bool initialized = engine.init(objects.design().desMgr(), setting);
  const std::string transcript = testing::internal::GetCapturedStdout();

  ASSERT_TRUE(initialized) << transcript;
  EXPECT_NE(transcript.find("kind=FILLER_MASTER_WIDTH"), std::string::npos);
  EXPECT_NE(transcript.find("checkerReach{sites=2 dbu=2}"),
            std::string::npos);
  EXPECT_NE(transcript.find("widestConfiguredFiller{"),
            std::string::npos);
  EXPECT_NE(transcript.find("dbu=4}"), std::string::npos);
  EXPECT_NE(transcript.find("defaultHaloX=4"), std::string::npos);
}

TEST(FillerRepairInitializationDiagnostics,
     CheckerRuleReachRemainsInitialHaloFloor)
{
  frt::DesignSetup setup;
  setup.implantRuleWidth = 6;
  setup.row0ThirdHardMacro = true;
  ProviderObjects objects(setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  ASSERT_TRUE(registerRepairMasters(objects, setting));
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  engine.setDebugLogging(true);

  testing::internal::CaptureStdout();
  const bool initialized = engine.init(objects.design().desMgr(), setting);
  const std::string transcript = testing::internal::GetCapturedStdout();

  ASSERT_TRUE(initialized) << transcript;
  EXPECT_NE(transcript.find("kind=CHECKER_RULE_REACH"), std::string::npos);
  EXPECT_NE(transcript.find("checkerReach{sites=6 dbu=6}"),
            std::string::npos);
  EXPECT_NE(transcript.find("widestConfiguredFiller{"),
            std::string::npos);
  EXPECT_NE(transcript.find("dbu=2}"), std::string::npos);
  EXPECT_NE(transcript.find("defaultHaloX=6"), std::string::npos);
}

TEST(FillerRepairInitializationDiagnostics,
     CandidateCatalogFastRejectsWidthMismatch)
{
  frt::DesignSetup setup;
  setup.implantRuleWidth = 6;
  ProviderObjects objects(setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell("FX4");
  ASSERT_TRUE(registerRepairMasters(objects, setting));
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  engine.setDebugLogging(true);

  testing::internal::CaptureStdout();
  const bool initialized = engine.init(objects.design().desMgr(), setting);
  dpl2::fillerRepair::RepairOutcome outcome;
  if (initialized) {
    outcome = repairWithRequest(engine, objects,
        objects.design().cell(frt::CellRole::Target),
        objects.design().master(frt::MasterRole::TargetNew));
  }
  const std::string transcript = testing::internal::GetCapturedStdout();

  ASSERT_TRUE(initialized) << transcript;
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_NE(transcript.find(
                "[fr][candidate] provider source: fillerSetting "
                "configuredCount=1"),
            std::string::npos);
  EXPECT_NE(transcript.find("[fr][candidate] configured[0]"),
            std::string::npos);
  EXPECT_NE(transcript.find("filler=1 vt="), std::string::npos);
  EXPECT_NE(transcript.find("width=4 heightRows=1 bottom=N"),
            std::string::npos);
  EXPECT_NE(transcript.find("[fr][candidate] catalog ready:"),
            std::string::npos);
  EXPECT_NE(transcript.find("compatiblePairs=0 placedCandidate=0"),
            std::string::npos);
  EXPECT_NE(transcript.find("widthMismatch=1"), std::string::npos);
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics,
                            "NoCompatibleFillerCandidate"));
  EXPECT_EQ(transcript.find("[fr][swapgen]"), std::string::npos);
}

TEST(FillerRepairInitializationDiagnostics,
     ExistingCandidateMasterRefreshesFillerClassificationForChecker)
{
  ProviderObjects objects(frt::DesignSetup{});
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::Grid* grid = objects.infrastructure().grid();
  dpl2::Network* network = objects.infrastructure().network();
  ASSERT_NE(grid, nullptr);
  ASSERT_NE(network, nullptr);

  // Pre-register FH2 under a stale setting that does not classify it as a
  // filler. This reproduces the early-return bug in ensureMasterRegistered.
  dpl2::fillerSetting staleSetting(objects.design().design());
  staleSetting.addFillerCell("FL2");
  const eLIB::PhysLibCell& repairMaster
      = objects.design().master(frt::MasterRole::RepairFiller);
  dpl2::Master* stale = network->addMaster(
      repairMaster, staleSetting, grid, &noEdgeTypes());
  ASSERT_NE(stale, nullptr);
  ASSERT_FALSE(stale->isFiller());

  dpl2::fillerSetting repairSetting(objects.design().design());
  repairSetting.addFillerCell(kDefaultFillers);
  // Register the remaining masters through infrastructure while FH2 still
  // carries the stale non-filler flag. init() must only refresh that flag.
  ASSERT_TRUE(registerRepairMasters(objects, staleSetting));
  for (const eLIB::PhysLibCell* configured
       : repairSetting.getFillerPhysCells()) {
    ASSERT_NE(network->addMaster(
                  *configured, staleSetting, grid, &noEdgeTypes()),
              nullptr);
  }
  dpl2::fillerRepair::FillerRepairEngine engine(grid, network);
  ASSERT_TRUE(engine.init(objects.design().desMgr(), repairSetting));
  EXPECT_EQ(network->getMaster(repairMaster.getLibCellId()), stale);
  EXPECT_TRUE(stale->isFiller());

  const auto outcome = repairWithRequest(engine, objects,
      objects.design().cell(frt::CellRole::Target),
      objects.design().master(frt::MasterRole::TargetNew));
  ASSERT_TRUE(outcome.hasSolution) << diagnosticText(outcome.diagnostics);
  ASSERT_EQ(outcome.changes.size(), 1U);
  EXPECT_EQ(outcome.changes.front().new_lib_cell_,
            repairMaster.getLibCellId());
  EXPECT_FALSE(hasDiagnostic(outcome.diagnostics,
                             "replacement_master_not_filler"));
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
  const int replacementId
      = harness.network().getMasterId(replacement.getLibCellId());
  ASSERT_GE(replacementId, 0);
  const auto outcome = harness.repair(macroId, replacement);
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "TargetNotStdCell"));
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()),
            replacementId);
}

TEST_P(FillerRepairEngineE2E,
       RepairPrecheckCountsUpperRowOfMultiRowHardMacro)
{
  frt::DesignSetup setup = GetParam().setup;
  setup.row0ThirdHardMacro = true;
  EngineHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());

  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto outcome = harness.repair(
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
  ASSERT_TRUE(harness.reinitialize());
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto& replacement
      = harness.design().master(frt::MasterRole::TargetNew);
  const int replacementId
      = harness.network().getMasterId(replacement.getLibCellId());
  ASSERT_GE(replacementId, 0);
  const auto outcome = harness.repair(
      harness.design().cell(frt::CellRole::Target), replacement);
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "Gap"));
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "PrecheckFailed"));
  EXPECT_FALSE(hasDiagnostic(outcome.diagnostics, "Overlap"));
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()),
            replacementId);
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
  ASSERT_TRUE(harness.reinitialize());
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto outcome = harness.repair(
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
  const auto outcome = harness.repair(
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
  ASSERT_GE(harness.network().getMasterId(targetMaster.getLibCellId()), 0);
  const auto before = harness.design().snapshot();
  const auto outcome = harness.repair(
      harness.design().cell(frt::CellRole::Target), targetMaster);
  ASSERT_TRUE(outcome.hasSolution);
  ASSERT_EQ(outcome.changes.size(), 1U);
  const auto& change = outcome.changes.front();
  EXPECT_EQ(change.op_, dpl2::OpType::Replace);
  EXPECT_EQ(change.new_lib_cell_,
            harness.design().master(frt::MasterRole::RepairFiller)
                .getLibCellId());
  const auto* cellId
      = std::get_if<eUNL::LeafCellID>(&change.cell_data_);
  ASSERT_NE(cellId, nullptr);
  EXPECT_TRUE(
      *cellId == harness.design().cell(frt::CellRole::TargetLeftFiller)
      || *cellId
             == harness.design().cell(frt::CellRole::TargetRightFiller));
  const dpl2::Node* changedNode = harness.network().getNode(*cellId);
  ASSERT_NE(changedNode, nullptr);
  EXPECT_EQ(change.orientation_.getValue(),
            changedNode->getOrient().getValue());
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
  ASSERT_TRUE(harness.resetContext());
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
  const int replacementId
      = harness.network().getMasterId(replacement.getLibCellId());
  ASSERT_GE(replacementId, 0);
  const auto outcome
      = harness.repair(eUNL::LeafCellID(0, 9999), replacement);
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "UnknownTarget"));
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()),
            replacementId);
}

TEST_P(FillerRepairEngineE2E,
       UnregisteredTargetMasterFailsWithoutRegistration)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  const auto& replacement
      = harness.design().master(frt::MasterRole::MismatchedTarget);
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
  const auto outcome = harness.repair(
      harness.design().cell(frt::CellRole::Target), replacement);
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "TargetMasterUnknown"));
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
}

TEST_P(FillerRepairEngineE2E, RegisteredSizeMismatchFailsWithoutMutation)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  ASSERT_TRUE(registerRepairMasters(objects, setting));
  dpl2::Grid* grid = objects.infrastructure().grid();
  dpl2::Network* network = objects.infrastructure().network();
  const eLIB::PhysLibCell& replacement
      = objects.design().master(frt::MasterRole::MismatchedTarget);
  ASSERT_NE(network->addMaster(replacement,
                               setting,
                               grid,
                               &noEdgeTypes()),
            nullptr);
  dpl2::fillerRepair::FillerRepairEngine engine(grid, network);
  ASSERT_TRUE(engine.init(objects.design().desMgr(), setting));
  const frt::PhysicalSnapshot before = objects.design().snapshot();

  const auto outcome = repairWithRequest(
      engine,
      objects,
      objects.design().cell(frt::CellRole::Target),
      replacement);
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "TargetSizeMismatch"));
  EXPECT_EQ(objects.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E,
       UpdateFollowsInfrastructureNodeSynchronization)
{
  // Infrastructure owns Network<->UDM coherence: the engine consumes the
  // synchronized state as-is (no cross-validation of its own). After the
  // owner replaces a master and syncs the Node, reinitialization adopts it; a null
  // PhysDesMgr still fails closed.
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  const auto targetId = harness.design().cell(frt::CellRole::Target);
  const auto& replacement
      = harness.design().master(frt::MasterRole::TargetNew);
  const dpl2::Node* target = harness.network().getNode(targetId);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(target->getMaster(), nullptr);

  ASSERT_TRUE(harness.reinitialize());
  harness.design().replaceCellMaster(frt::CellRole::Target,
                                     frt::MasterRole::TargetNew);
  ASSERT_TRUE(harness.syncInfrastructureCell(frt::CellRole::Target));
  ASSERT_TRUE(harness.reinitialize());
  target = harness.network().getNode(targetId);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(target->getMaster(), nullptr);
  EXPECT_EQ(target->getMaster()->getDbMaster(), replacement.getLibCellId());

  EXPECT_FALSE(harness.reinitialize(nullptr));
  const auto failedRepair = harness.repair(targetId, replacement);
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
  const auto first = harness.repair(target, newMaster);
  const auto second = harness.repair(target, newMaster);
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
  const auto outcome = harness.repair(
      harness.design().cell(frt::CellRole::Target),
      harness.design().master(frt::MasterRole::TargetNew));
  ASSERT_TRUE(outcome.hasSolution);
  ASSERT_FALSE(outcome.changes.empty());
  EXPECT_EQ(outcome.changes.front().new_lib_cell_,
            harness.design().master(frt::MasterRole::RepairFiller)
                .getLibCellId());
}


TEST_P(FillerRepairEngineE2E,
       MissingConfiguredMasterIsSafelySkippedWithoutRegistration)
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
  engine.setDebugLogging(true);

  testing::internal::CaptureStdout();
  const bool initialized = engine.init(objects.design().desMgr(), setting);
  const std::string transcript = testing::internal::GetCapturedStdout();

  EXPECT_TRUE(initialized) << transcript;
  EXPECT_EQ(objects.infrastructure().network()->getMasterId(extraId), -1);
  EXPECT_NE(transcript.find("ConfiguredMasterNotInNetwork"),
            std::string::npos);
  EXPECT_NE(transcript.find("decision=skip"), std::string::npos);

  // The safe omission is not permission to initialize with no candidate
  // universe at all.
  dpl2::fillerSetting onlyMissing(objects.design().design());
  onlyMissing.addFillerCell("FX4");
  dpl2::fillerRepair::FillerRepairEngine emptyEngine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  EXPECT_FALSE(emptyEngine.init(objects.design().desMgr(), onlyMissing));
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
  const auto repair = repairWithRequest(engine, objects,
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
  const auto repair = repairWithRequest(engine, objects,
      objects.design().cell(frt::CellRole::Target),
      objects.design().master(frt::MasterRole::TargetNew));
  EXPECT_FALSE(repair.hasSolution);
  EXPECT_TRUE(hasDiagnostic(repair.diagnostics, "missing_infrastructure"));
}

// Grid's retained manager is the checker authority. Changing the global
// current design after infrastructure creation must not redirect the checker
// or the engine's private checker.
TEST_P(FillerRepairEngineE2E, GridManagerAvoidsGlobalSession)
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
  frt::DesignSetup unrelatedSetup;
  unrelatedSetup.usedLayerMissingRule = true;
  auto active = provider->createDesign(unrelatedSetup);
  ASSERT_NE(active, nullptr);
  active->activate();
  ASSERT_EQ(infrastructure->grid()->getDesMgr(), requested->desMgr());
  dpl2::ipl::ImplantLayerChecker checker(infrastructure->grid(),
                                         infrastructure->network());
  EXPECT_TRUE(checker.getDiags().empty()) << diagnosticText(checker.getDiags());
  EXPECT_EQ(checker.siteWidth(), infrastructure->grid()->getSiteWidth().v);

  dpl2::fillerSetting setting(requested->design());
  setting.addFillerCell(kDefaultFillers);
  ASSERT_TRUE(registerRepairMasters(*requested,
                                    infrastructure->grid(),
                                    infrastructure->network(),
                                    setting));
  dpl2::fillerRepair::FillerRepairEngine engine(infrastructure->grid(),
                                                 infrastructure->network());
  ASSERT_TRUE(engine.init(requested->desMgr(), setting));
  const auto repair = repairWithRequest(
      engine,
      infrastructure->grid(),
      infrastructure->network(),
      requested->cell(frt::CellRole::Target),
      requested->master(frt::MasterRole::TargetNew));
  EXPECT_TRUE(repair.hasSolution);
  EXPECT_FALSE(hasDiagnostic(repair.diagnostics, "design_mismatch"));
}

TEST_P(FillerRepairEngineE2E, GridManagerMismatchFailsClosed)
{
  auto provider = frt::makeE2ETestProvider();
  ASSERT_NE(provider, nullptr);
  auto requested = provider->createDesign(GetParam().setup);
  ASSERT_NE(requested, nullptr);
  auto infrastructure
      = provider->createInfrastructure(*requested, GetParam().setup);
  ASSERT_NE(infrastructure, nullptr);
  auto other = provider->createDesign({});
  ASSERT_NE(other, nullptr);

  dpl2::fillerSetting setting(requested->design());
  setting.addFillerCell(kDefaultFillers);
  dpl2::fillerRepair::FillerRepairEngine engine(infrastructure->grid(),
                                                 infrastructure->network());
  EXPECT_FALSE(engine.init(other->desMgr(), setting));
  const auto repair = repairWithRequest(
      engine,
      infrastructure->grid(),
      infrastructure->network(),
      requested->cell(frt::CellRole::Target),
      requested->master(frt::MasterRole::TargetNew));
  EXPECT_FALSE(repair.hasSolution);
  EXPECT_TRUE(hasDiagnostic(repair.diagnostics,
                            "grid_phys_des_mgr_mismatch"))
      << diagnosticText(repair.diagnostics);
}

// set_filler_option must precede checker and repair initialization. Missing
// configuration is an integration error and disables repair for this checker
// until an explicit setFillerRepairContext() reset.
TEST_P(FillerRepairEngineE2E, MissingConfigurationFailsClosed)
{
  CheckerHarness harness(GetParam().setup, /*presetContext=*/false);
  ASSERT_TRUE(harness.checkerReady());
  g_providedSetting = nullptr;
  dpl2::ipl::ImplantLayerChecker::setFillerRepairSettingProvider(
      provideTestSetting);
  ASSERT_TRUE(harness.setTargetMaster(frt::MasterRole::TargetNew));

  // No setting: the check fails closed and no repair is attempted.
  EXPECT_FALSE(harness.checkTarget());
  EXPECT_TRUE(harness.fillerChanges().empty());

  // A setting appearing later cannot implicitly revive this checker.
  g_providedSetting = harness.fillerSetting();
  EXPECT_FALSE(harness.checkTarget());
  EXPECT_TRUE(harness.fillerChanges().empty());

  // A command/harness-created checker owns this explicit reset. DePlace-owned
  // checkers obtain the same setting through the provider.
  ASSERT_TRUE(harness.resetContext());
  EXPECT_TRUE(harness.checkTarget());
  EXPECT_FALSE(harness.fillerChanges().empty());

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
    const auto repair = repairWithRequest(engine, objects,
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
  ASSERT_TRUE(registerRepairMasters(objects, setting));
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  ASSERT_TRUE(engine.init(objects.design().desMgr(), setting));
  EXPECT_FALSE(engine.init(objects.design().desMgr(), setting));
  const auto outcome = repairWithRequest(engine, objects,
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

// --- filler classification --------------------------------------------------
// fillerSetting::core_ is the only filler authority. Network stores that
// answer on Master and Node; every downstream consumer reads those types.

TEST(FillerClassification, CoreListOverridesUdmMacroFlags)
{
  frt::DesignSetup setup;
  setup.misclassifiedFillerMasters = true;
  ProviderObjects objects(setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());

  dpl2::Node* filler = objects.infrastructure().network()->getNode(
      objects.design().cell(frt::CellRole::TargetLeftFiller));
  ASSERT_NE(filler, nullptr);
  ASSERT_NE(filler->getMaster(), nullptr);
  ASSERT_NE(filler->getMaster()->getPhysLibCell(), nullptr);

  const eLIB::PhysMacroType& udmType
      = filler->getMaster()->getPhysLibCell()->getType();
  EXPECT_FALSE(udmType.isCoreFiller());
  EXPECT_FALSE(udmType.isPadFiller());
  EXPECT_TRUE(filler->getMaster()->isFiller());
  EXPECT_TRUE(filler->isFiller());
  EXPECT_FALSE(filler->isStdCell());
}

TEST(FillerClassification, RepairUsesCoreListWhenUdmFlagsSayNonFiller)
{
  frt::DesignSetup setup;
  setup.misclassifiedFillerMasters = true;
  EngineHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());

  const auto outcome = harness.repair(
      harness.design().cell(frt::CellRole::Target),
      harness.design().master(frt::MasterRole::TargetNew));
  ASSERT_TRUE(outcome.hasSolution) << diagnosticText(outcome.diagnostics);
  ASSERT_EQ(outcome.changes.size(), 1U);
  EXPECT_EQ(outcome.changes.front().new_lib_cell_,
            harness.design()
                .master(frt::MasterRole::RepairFiller)
                .getLibCellId());
}

// PhysMacroType::isCore() is true for CORE_FILLER, so isStdCell() used to say
// yes for every filler. "Is a standard cell" is not "stands on a site".
TEST_P(FillerRepairEngineE2E, FillerIsNotAStandardCell)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasInfrastructure());
  int fillers = 0;
  int stdCells = 0;
  for (const auto& node : objects.infrastructure().network()->getNodes()) {
    if (node->isFiller()) {
      ++fillers;
      EXPECT_FALSE(node->isStdCell()) << "filler reported as a standard cell";
    } else if (!node->isTerminal()) {
      ++stdCells;
    }
  }
  EXPECT_GT(fillers, 0);
  EXPECT_GT(stdCells, 0) << "isStdCell must not have swallowed everything";
}

TEST_P(FillerRepairEngineE2E, NodeAndMasterAgreeOnFillerness)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasInfrastructure());
  for (const auto& node : objects.infrastructure().network()->getNodes()) {
    ASSERT_NE(node->getMaster(), nullptr);
    EXPECT_EQ(node->isFiller(), node->getMaster()->isFiller())
        << "node " << node->getId() << " disagrees with its master";
  }
}

// updateNode refreshed master, size, orientation and status but never the
// type, so a swap that changes filler-ness left isFiller() answering about the
// previous master.
TEST_P(FillerRepairEngineE2E, UpdateNodeRefreshesFillerness)
{
  ProviderObjects objects(GetParam().setup, /*buildInfrastructure=*/true);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::Network* network = objects.infrastructure().network();

  dpl2::Node* target
      = network->getNode(objects.design().cell(frt::CellRole::Target));
  ASSERT_NE(target, nullptr);
  ASSERT_FALSE(target->isFiller());
  ASSERT_NE(target->getMaster(), nullptr);
  const eLIB::PhysLibCell& oldMaster = *target->getMaster()->getPhysLibCell();

  // A master already registered in this Network, so the swap is the only
  // thing under test.
  const eLIB::PhysLibCell* fillerMaster = nullptr;
  for (const auto& node : network->getNodes()) {
    if (node->isFiller() && node->getMaster() != nullptr) {
      fillerMaster = node->getMaster()->getPhysLibCell();
      break;
    }
  }
  ASSERT_NE(fillerMaster, nullptr);
  ASSERT_TRUE(network->getMaster(fillerMaster->getLibCellId())->isFiller());

  ASSERT_TRUE(
      network->updateNode(target, objects.design().desMgr(), *fillerMaster));
  EXPECT_TRUE(target->isFiller());
  EXPECT_FALSE(target->isStdCell());

  ASSERT_TRUE(
      network->updateNode(target, objects.design().desMgr(), oldMaster));
  EXPECT_FALSE(target->isFiller());
  EXPECT_TRUE(target->isStdCell());
}

// An unregistered master used to be stored and then dereferenced: the node was
// left half-updated and the process died. updateNode returns bool; this is
// what it is for.
TEST_P(FillerRepairEngineE2E, UpdateNodeRefusesAnUnregisteredMaster)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::Network* network = objects.infrastructure().network();

  dpl2::Node* target
      = network->getNode(objects.design().cell(frt::CellRole::Target));
  ASSERT_NE(target, nullptr);
  const dpl2::Master* before = target->getMaster();
  const bool wasFiller = target->isFiller();

  // Never instantiated, so no placed cell caused it to be registered.
  const eLIB::PhysLibCell& unregistered
      = objects.design().master(frt::MasterRole::ExtraUninstantiatedFiller);
  ASSERT_EQ(network->getMaster(unregistered.getLibCellId()), nullptr);

  EXPECT_FALSE(
      network->updateNode(target, objects.design().desMgr(), unregistered));
  EXPECT_EQ(target->getMaster(), before);
  EXPECT_EQ(target->isFiller(), wasFiller);
}

TEST_P(FillerRepairEngineE2E, NetworkRejectsNullImportDependencies)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::Network* network = objects.infrastructure().network();
  dpl2::Grid* grid = objects.infrastructure().grid();
  ASSERT_NE(network, nullptr);
  ASSERT_NE(grid, nullptr);

  dpl2::Node* target
      = network->getNode(objects.design().cell(frt::CellRole::Target));
  ASSERT_NE(target, nullptr);
  ASSERT_NE(target->getMaster(), nullptr);
  ASSERT_NE(target->getMaster()->getPhysLibCell(), nullptr);
  const eLIB::PhysLibCell& master = *target->getMaster()->getPhysLibCell();
  const dpl2::Master* before = target->getMaster();
  const size_t nodeCount = network->getNodes().size();

  EXPECT_FALSE(network->addNode(
      objects.design().cell(frt::CellRole::Target), nullptr));
  EXPECT_EQ(network->getNodes().size(), nodeCount);
  EXPECT_FALSE(network->updateNode(nullptr, objects.design().desMgr(), master));
  EXPECT_FALSE(network->updateNode(target, nullptr, master));
  EXPECT_EQ(target->getMaster(), before);

  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  static const dpl2::EdgeTypeTable kNoEdgeTypes;
  EXPECT_EQ(network->addMaster(master, setting, nullptr, &kNoEdgeTypes),
            nullptr);
  EXPECT_EQ(network->addMaster(master, setting, grid, nullptr), nullptr);
}

TEST_P(FillerRepairEngineE2E, EngineRejectsNullNetworkNode)
{
  ProviderObjects objects(GetParam().setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::Network* network = objects.infrastructure().network();
  ASSERT_NE(network, nullptr);
  network->getNodes().emplace_back(nullptr);

  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  ASSERT_TRUE(registerRepairMasters(objects, setting));
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), network);
  EXPECT_FALSE(engine.init(objects.design().desMgr(), setting));
  const auto repair = repairWithRequest(engine, objects,
      objects.design().cell(frt::CellRole::Target),
      objects.design().master(frt::MasterRole::TargetNew));
  EXPECT_FALSE(repair.hasSolution);
  EXPECT_TRUE(hasDiagnostic(repair.diagnostics, "NullNetworkNode"));
}

INSTANTIATE_TEST_SUITE_P(
    FiveRowLayouts,
    FillerRepairEngineE2E,
    ::testing::Values(canonicalLayout(), shiftedLayout(), farShiftedLayout()),
    [](const ::testing::TestParamInfo<LayoutCase>& info) {
      return info.param.name;
    });
