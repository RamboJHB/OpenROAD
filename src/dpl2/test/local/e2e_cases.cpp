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

#include "drc/ImplantLayerChecker.h"
#include "infrastructure/Grid.h"
#include "infrastructure/fillerSetting.h"
#include "infrastructure/network.h"

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
  explicit CheckerHarness(const frt::DesignSetup& setup)
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
    checker_ready_ = checker_->initFillerRepair(
        objects_.design().desMgr(), *filler_setting_);
  }

  bool checkerReady() const { return checker_ready_; }
  frt::E2ETestDesign& design() { return objects_.design(); }
  dpl2::ipl::ImplantLayerChecker& checker() { return *checker_; }

  // Refresh Network/engine snapshots after a placement mutation (the caller
  // contract before any subsequent check on a changed design).
  bool update()
  {
    return checker_->updateFillerRepair(design().desMgr(), *filler_setting_);
  }

  bool setTargetMaster(frt::MasterRole role)
  {
    dpl2::Grid* grid = objects_.infrastructure().grid();
    dpl2::Network* network = objects_.infrastructure().network();
    const eLIB::PhysLibCell& master = design().master(role);
    dpl2::Node* target = network->getNode(
        design().cell(frt::CellRole::Target));
    return target != nullptr && network->addMaster(master, grid) != nullptr
           && network->updateNode(target, design().desMgr(), master);
  }

  bool checkTarget()
  {
    dpl2::Grid* grid = objects_.infrastructure().grid();
    dpl2::Network* network = objects_.infrastructure().network();
    dpl2::Node* target = network->getNode(
        design().cell(frt::CellRole::Target));
    return target != nullptr
           && checker_->check(target,
                              grid->gridX(target),
                              grid->gridSnapDownY(target),
                              target->getOrient());
  }

 private:
  ProviderObjects objects_;
  std::unique_ptr<dpl2::fillerSetting> filler_setting_;
  std::unique_ptr<dpl2::ipl::ImplantLayerChecker> checker_;
  bool checker_ready_ = false;
};

class FillerRepairEngineE2E
    : public ::testing::TestWithParam<LayoutCase>
{
};

}  // namespace

TEST_P(FillerRepairEngineE2E, CleanPlacementPrecheck)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  ASSERT_GE(harness.design().standardRowCount(), frt::kStandardRows);
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const dpl2::ipl::CheckResult result = harness.engine().precheck();
  EXPECT_TRUE(result.isLegal);
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E, GapPlacementPrecheck)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  harness.design().moveCell(
      frt::CellRole::Row0TailFiller,
      harness.design().rowOriginX(0) + frt::kRowSites,
      0);
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto result = harness.engine().precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "Gap"));
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E, OverlapPlacementPrecheck)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  harness.design().moveCell(frt::CellRole::Row1TailFiller,
                            harness.design().rowOriginX(1) + 17,
                            frt::kRowHeight);
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto result = harness.engine().precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "Overlap"));
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E, GapInsideHardBlockageIsIgnored)
{
  frt::DesignSetup setup = GetParam().setup;
  setup.row0TailHardBlockage = true;
  EngineHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());
  harness.design().moveCell(
      frt::CellRole::Row0TailFiller,
      harness.design().rowOriginX(0) + frt::kRowSites,
      0);
  const auto before = harness.design().snapshot();
  const auto result = harness.engine().precheck();
  EXPECT_TRUE(result.isLegal);
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E, GapInsideSoftBlockageStillFails)
{
  frt::DesignSetup setup = GetParam().setup;
  setup.row0TailSoftBlockage = true;
  EngineHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());
  harness.design().moveCell(
      frt::CellRole::Row0TailFiller,
      harness.design().rowOriginX(0) + frt::kRowSites,
      0);
  const auto result = harness.engine().precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "Gap"));
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
  const auto result = harness.engine().precheck();
  EXPECT_TRUE(result.isLegal);
  EXPECT_TRUE(result.diagnostics.empty());

  const auto& replacement
      = harness.design().master(frt::MasterRole::TargetNew);
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
  const auto outcome = harness.engine().repair(macroId, replacement);
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "TargetNotStdCell"));
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
}

TEST_P(FillerRepairEngineE2E, GapInsideInstanceHaloIsIgnored)
{
  frt::DesignSetup setup = GetParam().setup;
  setup.row0TailHaloWidth = 2;
  EngineHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());
  harness.design().moveCell(
      frt::CellRole::Row0TailFiller,
      harness.design().rowOriginX(0) + frt::kRowSites,
      0);
  const auto before = harness.design().snapshot();
  const auto result = harness.engine().precheck();
  EXPECT_TRUE(result.isLegal);
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E, GapInsideLegalSegmentStillFails)
{
  frt::DesignSetup setup = GetParam().setup;
  setup.row0TailHardBlockage = true;
  EngineHarness harness(setup);
  ASSERT_TRUE(harness.engineReady());
  harness.design().moveCell(
      frt::CellRole::Row0ThirdCell,
      harness.design().rowOriginX(0) + frt::kRowSites,
      0);
  const auto before = harness.design().snapshot();
  const auto result = harness.engine().precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "Gap"));
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E, RepeatedExternalPrecheckIsStable)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const dpl2::ipl::CheckResult first = harness.engine().precheck();
  const dpl2::ipl::CheckResult second = harness.engine().precheck();
  EXPECT_TRUE(first.isLegal);
  EXPECT_TRUE(second.isLegal);
  EXPECT_EQ(first.diagnostics.size(), second.diagnostics.size());
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E, OptoStyleExternalGateBlocksMutation)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  harness.design().moveCell(frt::CellRole::Row0TailFiller,
                            harness.design().rowOriginX(0) + frt::kRowSites,
                            0);
  const frt::PhysicalSnapshot beforeGate = harness.design().snapshot();
  const dpl2::ipl::CheckResult gate = harness.engine().precheck();
  const bool optoMayMutate = gate.isLegal;
  EXPECT_FALSE(optoMayMutate);
  EXPECT_TRUE(hasDiagnostic(gate.diagnostics, "Gap"));
  EXPECT_EQ(harness.design().snapshot(), beforeGate);
}

TEST_P(FillerRepairEngineE2E, ExternalPrecheckReportsGapAndOverlapTogether)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  harness.design().moveCell(frt::CellRole::Row0TailFiller,
                            harness.design().rowOriginX(0) + frt::kRowSites,
                            0);
  harness.design().moveCell(frt::CellRole::Row1TailFiller,
                            harness.design().rowOriginX(1) + 17,
                            frt::kRowHeight);
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const dpl2::ipl::CheckResult result = harness.engine().precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "Gap"));
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "Overlap"));
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
  ASSERT_TRUE(harness.update());  // caller refreshes the snapshot after a move
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto outcome = harness.engine().repair(
      harness.design().cell(frt::CellRole::Target),
      harness.design().master(frt::MasterRole::TargetNew));
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "Gap"));
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "PrecheckFailed"));
  EXPECT_FALSE(hasDiagnostic(outcome.diagnostics, "Overlap"));
  EXPECT_EQ(harness.design().snapshot(), before);
}

TEST_P(FillerRepairEngineE2E, RepairIgnoresGapOutsideInfluenceRows)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  // A gap in row 0 is outside the target's influence rows (guard rows 1..3 for
  // a target in row 2). The narrowed internal precheck must not block the
  // repair, which still finds its local filler swap. The whole-design
  // precheck() remains available to opto for the global gate.
  harness.design().moveCell(frt::CellRole::Row0TailFiller,
                            harness.design().rowOriginX(0) + frt::kRowSites,
                            0);
  ASSERT_TRUE(harness.update());  // caller refreshes the snapshot after a move
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  ASSERT_FALSE(harness.engine().precheck().isLegal);  // global gate still sees it
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
  const auto& changes = harness.checker().getFillerChanges();
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
  EXPECT_TRUE(harness.checker().getFillerChanges().empty());
  EXPECT_EQ(harness.design().snapshot(), beforeFailedCheck);
}

TEST_P(FillerRepairEngineE2E, CheckerEntryCleanCandidateHasNoChanges)
{
  CheckerHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.checkerReady());
  const auto before = harness.design().snapshot();
  ASSERT_TRUE(harness.setTargetMaster(frt::MasterRole::TargetOld));

  EXPECT_TRUE(harness.checkTarget());
  EXPECT_TRUE(harness.checker().getFillerChanges().empty());
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
  ASSERT_TRUE(harness.update());  // caller refreshes the snapshot after a move
  const auto before = harness.design().snapshot();
  ASSERT_TRUE(harness.setTargetMaster(frt::MasterRole::TargetNew));

  EXPECT_FALSE(harness.checkTarget());
  EXPECT_TRUE(harness.checker().getFillerChanges().empty());
  EXPECT_TRUE(hasDiagnostic(
      harness.checker().getFillerRepairDiagnostics(), "Gap"));
  EXPECT_TRUE(hasDiagnostic(
      harness.checker().getFillerRepairDiagnostics(), "PrecheckFailed"));
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

TEST_P(FillerRepairEngineE2E, UpdateRefreshesExistingNodeMasterMapping)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  const auto targetId = harness.design().cell(frt::CellRole::Target);
  const auto& replacement
      = harness.design().master(frt::MasterRole::TargetNew);
  EXPECT_EQ(harness.network().getMasterId(replacement.getLibCellId()), -1);
  harness.design().replaceCellMaster(frt::CellRole::Target,
                                     frt::MasterRole::TargetNew);
  ASSERT_TRUE(harness.update());
  const dpl2::Node* target = harness.network().getNode(targetId);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(target->getMaster(), nullptr);
  EXPECT_EQ(target->getMaster()->getDbMaster(), replacement.getLibCellId());
  EXPECT_TRUE(harness.engine().precheck().isLegal);

  EXPECT_FALSE(harness.update(nullptr));
  const auto failedPrecheck = harness.engine().precheck();
  EXPECT_FALSE(failedPrecheck.isLegal);
  EXPECT_TRUE(
      hasDiagnostic(failedPrecheck.diagnostics, "precheck_not_initialized"));
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

TEST_P(FillerRepairEngineE2E,
       StructuralCheckerDiagnosticsBlockInitialization)
{
  frt::DesignSetup setup = GetParam().setup;
  setup.usedLayerMissingRule = true;
  ProviderObjects objects(setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  EXPECT_FALSE(engine.init(objects.design().desMgr(), setting));
  const auto precheck = engine.precheck();
  EXPECT_FALSE(precheck.isLegal);
  EXPECT_TRUE(hasDiagnostic(precheck.diagnostics, "missing_rule_parameter"));
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
  const auto result = engine.precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "empty_filler_allow_list"));
}

TEST_P(FillerRepairEngineE2E, MissingInfrastructureErrorsOut)
{
  ProviderObjects objects(GetParam().setup, false);
  ASSERT_TRUE(objects.hasDesign());
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  dpl2::fillerRepair::FillerRepairEngine engine(nullptr, nullptr);
  EXPECT_FALSE(engine.init(objects.design().desMgr(), setting));
  const auto result = engine.precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "missing_infrastructure"));
}

TEST_P(FillerRepairEngineE2E, ActiveDesignMismatchFailsInit)
{
  auto provider = frt::makeE2ETestProvider();
  ASSERT_NE(provider, nullptr);
  auto requested = provider->createDesign(GetParam().setup);
  ASSERT_NE(requested, nullptr);
  auto infrastructure
      = provider->createInfrastructure(*requested, GetParam().setup);
  ASSERT_NE(infrastructure, nullptr);
  auto active = provider->createDesign({});
  ASSERT_NE(active, nullptr);
  active->activate();
  dpl2::fillerSetting setting(requested->design());
  setting.addFillerCell(kDefaultFillers);
  dpl2::fillerRepair::FillerRepairEngine engine(infrastructure->grid(),
                                                 infrastructure->network());
  EXPECT_FALSE(engine.init(requested->desMgr(), setting));
  const auto result = engine.precheck();
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result.diagnostics, "active_design_mismatch"));
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
    const auto precheck = engine.precheck();
    EXPECT_FALSE(precheck.isLegal);
    EXPECT_TRUE(
        hasDiagnostic(precheck.diagnostics, "precheck_not_initialized"));
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
  EXPECT_TRUE(engine.precheck().isLegal);
}

INSTANTIATE_TEST_SUITE_P(
    FiveRowLayouts,
    FillerRepairEngineE2E,
    ::testing::Values(canonicalLayout(), shiftedLayout(), farShiftedLayout()),
    [](const ::testing::TestParamInfo<LayoutCase>& info) {
      return info.param.name;
    });

namespace {

struct RowOriginCase
{
  const char* name;
  frt::DesignSetup setup;
  bool expectInit;
};

RowOriginCase alignedShiftedRows()
{
  frt::DesignSetup setup;
  setup.rowOriginX.fill(3);
  return {"AlignedShiftedRows", setup, true};
}

RowOriginCase padBeforeStandardRows()
{
  frt::DesignSetup setup;
  setup.padRowFirst = true;
  setup.padRowOriginX = 5;
  return {"PadBeforeStandardRows", setup, false};
}

RowOriginCase padAfterStandardRows()
{
  frt::DesignSetup setup;
  setup.padRowLast = true;
  setup.padRowOriginX = 5;
  return {"PadAfterStandardRows", setup, true};
}

RowOriginCase misalignedStandardRowWithPad()
{
  frt::DesignSetup setup;
  setup.padRowLast = true;
  setup.padRowOriginX = 5;
  setup.rowOriginX = {0, 0, 0, 3, 0};
  return {"MisalignedStandardRowWithPad", setup, false};
}

class FillerRepairRowOriginE2E
    : public ::testing::TestWithParam<RowOriginCase>
{
};

}  // namespace

TEST_P(FillerRepairRowOriginE2E, FirstNonPadRowDefinesSharedXFrame)
{
  const RowOriginCase& testCase = GetParam();
  ProviderObjects objects(testCase.setup);
  ASSERT_TRUE(objects.hasDesign());
  ASSERT_TRUE(objects.hasInfrastructure());
  ASSERT_GE(objects.design().standardRowCount(), frt::kStandardRows);
  dpl2::fillerSetting setting(objects.design().design());
  setting.addFillerCell(kDefaultFillers);
  dpl2::fillerRepair::FillerRepairEngine engine(
      objects.infrastructure().grid(), objects.infrastructure().network());
  EXPECT_EQ(engine.init(objects.design().desMgr(), setting),
            testCase.expectInit);
}

INSTANTIATE_TEST_SUITE_P(
    FiveRowOriginCases,
    FillerRepairRowOriginE2E,
    ::testing::Values(alignedShiftedRows(), padBeforeStandardRows(),
                      padAfterStandardRows(),
                      misalignedStandardRowWithPad()),
    [](const ::testing::TestParamInfo<RowOriginCase>& info) {
      return info.param.name;
    });
