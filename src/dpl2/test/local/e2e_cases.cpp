// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Historical repository-local engine regression. Fixture
// construction uses the adjacent fake-UDM provider; this suite is deliberately
// outside the migration payload.

#include "E2ETestProvider.h"

#include <algorithm>
#include <memory>
#include <string>

#include <gtest/gtest.h>

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

 private:
  ProviderObjects objects_;
  std::unique_ptr<dpl2::fillerSetting> filler_setting_;
  std::unique_ptr<dpl2::fillerRepair::FillerRepairEngine> engine_;
  bool engine_ready_ = false;
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

TEST_P(FillerRepairEngineE2E, RepairDoesNotImplicitlyCallExternalPrecheck)
{
  EngineHarness harness(GetParam().setup);
  ASSERT_TRUE(harness.engineReady());
  harness.design().moveCell(frt::CellRole::Row0TailFiller,
                            harness.design().rowOriginX(0) + frt::kRowSites,
                            0);
  const frt::PhysicalSnapshot before = harness.design().snapshot();
  const auto outcome = harness.engine().repair(
      harness.design().cell(frt::CellRole::Target),
      harness.design().master(frt::MasterRole::TargetNew));
  EXPECT_FALSE(hasDiagnostic(outcome.diagnostics, "Gap"));
  EXPECT_FALSE(hasDiagnostic(outcome.diagnostics, "Overlap"));
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
