// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Repository-local runtime E2E. Only FakeUdmE2ETestProvider constructs test
// data; every production source is compiled unchanged against the UDM names.

#include <drc/ImplantLayerChecker.h>
#include <fillerRepair/FillerRepairEngine.h>
#include <gtest/gtest.h>
#include <infrastructure/Grid.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "E2ETestProvider.h"

namespace frt = dpl2::fillerRepair::test;

namespace {

constexpr const char* kFillerMasters = "FL2 FH2 FS2 FH1 FL2D FH1D";

bool hasDiagnostic(const std::vector<dpl2::ipl::Diagnostic>& diagnostics,
                   const std::string& status)
{
  return std::any_of(
      diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.status == status;
      });
}

std::string diagnosticText(
    const std::vector<dpl2::ipl::Diagnostic>& diagnostics)
{
  std::string text;
  for (const auto& diagnostic : diagnostics) {
    text += diagnostic.status + ": " + diagnostic.message + '\n';
  }
  return text;
}

bool sameChanges(const dpl2::ipl::FillerChanges& left,
                 const dpl2::ipl::FillerChanges& right)
{
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t index = 0; index < left.size(); ++index) {
    if (left[index].op_ != right[index].op_
        || left[index].cell_data_ != right[index].cell_data_
        || left[index].orig_lib_cell_ != right[index].orig_lib_cell_
        || left[index].new_lib_cell_ != right[index].new_lib_cell_
        || left[index].orientation_ != right[index].orientation_) {
      return false;
    }
  }
  return true;
}

const dpl2::EdgeTypeTable& noEdgeTypes()
{
  static const dpl2::EdgeTypeTable table;
  return table;
}

class RuntimeFixture
{
 public:
  explicit RuntimeFixture(const frt::DesignSetup& setup = {})
      : provider_(frt::makeE2ETestProvider())
  {
    if (provider_ == nullptr) {
      return;
    }
    design_ = provider_->createDesign(setup);
    if (design_ == nullptr) {
      return;
    }
    infrastructure_ = provider_->createInfrastructure(*design_, setup);
    if (infrastructure_ == nullptr) {
      return;
    }
    setting_ = std::make_unique<dpl2::fillerSetting>(design_->design());
    setting_->addFillerCell(kFillerMasters);
    dpl2::Network* const network = infrastructure_->network();
    dpl2::Grid* const grid = infrastructure_->grid();
    network->setFillerSetting(setting_.get());
    for (const eLIB::PhysLibCell* filler : setting_->getFillerPhysCells()) {
      if (filler == nullptr
          || network->addMaster(
                 *filler, *setting_, grid, &noEdgeTypes()) == nullptr) {
        return;
      }
    }
    for (frt::MasterRole role : {frt::MasterRole::TargetNew,
                                 frt::MasterRole::WiderTarget,
                                 frt::MasterRole::TargetOldDoubleHeight,
                                 frt::MasterRole::TargetNewDoubleHeight,
                                 frt::MasterRole::Buffer,
                                 frt::MasterRole::BufferDoubleHeight,
                                 frt::MasterRole::NarrowBuffer}) {
      if (network->addMaster(design_->master(role),
                             *setting_,
                             grid,
                             &noEdgeTypes()) == nullptr) {
        return;
      }
    }
    checker_ = std::make_unique<dpl2::ipl::ImplantLayerChecker>(
        grid, design_->design(), network);
    ready_ = true;
  }

  bool ready() const { return ready_; }
  frt::E2ETestDesign& design() { return *design_; }
  dpl2::Grid& grid() { return *infrastructure_->grid(); }
  dpl2::Network& network() { return *infrastructure_->network(); }
  dpl2::ipl::ImplantLayerChecker& checker() { return *checker_; }

  bool request(frt::CellRole replacedRole,
               frt::MasterRole newMasterRole,
               dpl2::Node& temporary,
               dpl2::ipl::CheckRequestOverlay& request,
               std::optional<eUTL::PhysOrientation> orientation = std::nullopt)
  {
    dpl2::Node* const replaced = network().getNode(design().cell(replacedRole));
    const eLIB::PhysLibCell& physicalMaster = design().master(newMasterRole);
    dpl2::Master* const replacement =
        network().getMaster(physicalMaster.getLibCellId());
    if (replaced == nullptr || replaced->getMaster() == nullptr
        || replacement == nullptr) {
      return false;
    }
    const eUTL::PhysOrientation targetOrientation
        = orientation.value_or(replaced->getOrient());
    temporary.setId(replaced->getId());
    temporary.setDbInst(replaced->getDbInst());
    temporary.setMaster(replacement);
    temporary.setType(dpl2::Node::CELL);
    temporary.setWidth(
        dpl2::DbuX{physicalMaster.getWidth().getStorage()});
    temporary.setHeight(
        dpl2::DbuY{physicalMaster.getHeight().getStorage()});
    temporary.setLeft(replaced->getLeft());
    temporary.setBottom(replaced->getBottom());
    temporary.setOrient(targetOrientation);
    const eLIB::LibCellID oldMaster = replaced->getMaster()->getDbMaster();
    request = dpl2::ipl::CheckRequestOverlay{
        &temporary,
        grid().gridX(replaced),
        grid().gridSnapDownY(replaced),
        targetOrientation,
        {{dpl2::OpType::Delete,
          replaced->getDbInst(),
          eUTL::UvDist(replaced->getLeft().v),
          eUTL::UvDist(replaced->getBottom().v),
          oldMaster,
          oldMaster,
          replaced->getOrient()}}};
    return true;
  }

 private:
  std::unique_ptr<frt::E2ETestProvider> provider_;
  std::unique_ptr<frt::E2ETestDesign> design_;
  std::unique_ptr<frt::E2ETestInfrastructure> infrastructure_;
  std::unique_ptr<dpl2::fillerSetting> setting_;
  std::unique_ptr<dpl2::ipl::ImplantLayerChecker> checker_;
  bool ready_ = false;
};

struct LayoutCase
{
  const char* name;
  frt::DesignSetup setup;
};

class FillerRepairRuntimeE2E : public ::testing::TestWithParam<LayoutCase>
{
};

TEST_P(FillerRepairRuntimeE2E, StdCellReplacementReturnsOnlyFillerSwaps)
{
  RuntimeFixture fixture(GetParam().setup);
  ASSERT_TRUE(fixture.ready());
  dpl2::fillerRepair::FillerRepairEngine engine(fixture.checker());
  ASSERT_TRUE(engine.isReady()) << diagnosticText(engine.getInitDiagnostics());
  dpl2::Node temporary;
  dpl2::ipl::CheckRequestOverlay request;
  ASSERT_TRUE(fixture.request(frt::CellRole::Target,
                              frt::MasterRole::TargetNew,
                              temporary,
                              request));
  const frt::PhysicalSnapshot before = fixture.design().snapshot();

  const dpl2::fillerRepair::RepairOutcome outcome = engine.repair(request);

  ASSERT_TRUE(outcome.hasSolution) << diagnosticText(outcome.diagnostics);
  ASSERT_EQ(outcome.changes.size(), 1U);
  EXPECT_TRUE(std::all_of(
      outcome.changes.begin(), outcome.changes.end(), [](const auto& change) {
        return change.op_ == dpl2::OpType::Replace
               && std::holds_alternative<eUNL::LeafCellID>(change.cell_data_);
      }));
  EXPECT_EQ(outcome.changes.front().new_lib_cell_,
            fixture.design()
                .master(frt::MasterRole::RepairFiller)
                .getLibCellId());
  EXPECT_NE(std::get<eUNL::LeafCellID>(outcome.changes.front().cell_data_),
            fixture.design().cell(frt::CellRole::Target));
  EXPECT_EQ(fixture.design().snapshot(), before);
}

TEST_P(FillerRepairRuntimeE2E, CheckerEntryLazilyRunsTheSameRepair)
{
  RuntimeFixture fixture(GetParam().setup);
  ASSERT_TRUE(fixture.ready());
  dpl2::Node temporary;
  dpl2::ipl::CheckRequestOverlay request;
  ASSERT_TRUE(fixture.request(frt::CellRole::Target,
                              frt::MasterRole::TargetNew,
                              temporary,
                              request));
  const frt::PhysicalSnapshot before = fixture.design().snapshot();
  dpl2::ipl::FillerChanges changes;

  EXPECT_TRUE(fixture.checker().check(&temporary,
                                      request.x,
                                      request.y,
                                      request.orientation,
                                      changes,
                                      request.overlayChanges));
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes.front().op_, dpl2::OpType::Replace);
  EXPECT_EQ(fixture.design().snapshot(), before);
}

TEST_P(FillerRepairRuntimeE2E, RotationDoesNotMutateTheExistingCell)
{
  RuntimeFixture fixture(GetParam().setup);
  ASSERT_TRUE(fixture.ready());
  dpl2::fillerRepair::FillerRepairEngine engine(fixture.checker());
  ASSERT_TRUE(engine.isReady());
  dpl2::Node temporary;
  dpl2::ipl::CheckRequestOverlay request;
  ASSERT_TRUE(fixture.request(
      frt::CellRole::Target,
      frt::MasterRole::TargetOld,
      temporary,
      request,
      eUTL::PhysOrientation(eUTL::PhysOrientationE::R180)));
  const frt::PhysicalSnapshot before = fixture.design().snapshot();

  const auto outcome = engine.repair(request);

  EXPECT_TRUE(outcome.hasSolution) << diagnosticText(outcome.diagnostics);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_EQ(fixture.design().snapshot(), before);
}

TEST_P(FillerRepairRuntimeE2E, FindLegalReplacesExactlyOneFiller)
{
  RuntimeFixture fixture(GetParam().setup);
  ASSERT_TRUE(fixture.ready());
  dpl2::fillerRepair::FillerRepairEngine engine(fixture.checker());
  ASSERT_TRUE(engine.isReady());
  dpl2::Node temporary;
  dpl2::ipl::CheckRequestOverlay request;
  ASSERT_TRUE(fixture.request(frt::CellRole::TargetLeftFiller,
                              frt::MasterRole::Buffer,
                              temporary,
                              request));
  const frt::PhysicalSnapshot before = fixture.design().snapshot();

  const auto outcome = engine.repair(request);

  ASSERT_TRUE(outcome.hasSolution) << diagnosticText(outcome.diagnostics);
  EXPECT_TRUE(std::all_of(
      outcome.changes.begin(), outcome.changes.end(), [&](const auto& change) {
        const auto* id = std::get_if<eUNL::LeafCellID>(&change.cell_data_);
        return change.op_ == dpl2::OpType::Replace && id != nullptr
               && *id
                      != fixture.design().cell(
                          frt::CellRole::TargetLeftFiller);
      }));
  EXPECT_EQ(fixture.design().snapshot(), before);
}

TEST_P(FillerRepairRuntimeE2E, RejectsMoreThanOneReplacedNodeAtomically)
{
  RuntimeFixture fixture(GetParam().setup);
  ASSERT_TRUE(fixture.ready());
  dpl2::Node temporary;
  dpl2::ipl::CheckRequestOverlay request;
  ASSERT_TRUE(fixture.request(frt::CellRole::Target,
                              frt::MasterRole::TargetNew,
                              temporary,
                              request));
  request.overlayChanges.push_back(request.overlayChanges.front());
  dpl2::ipl::FillerChanges changes{
      {dpl2::OpType::Replace,
       fixture.design().cell(frt::CellRole::TargetRightFiller),
       eUTL::UvDist(0),
       eUTL::UvDist(0),
       eLIB::LibCellID(),
       eLIB::LibCellID(),
       eUTL::PhysOrientationE::R0}};
  const dpl2::ipl::FillerChanges beforeChanges = changes;

  EXPECT_FALSE(fixture.checker().check(&temporary,
                                       request.x,
                                       request.y,
                                       request.orientation,
                                       changes,
                                       request.overlayChanges));
  EXPECT_TRUE(sameChanges(changes, beforeChanges));
}

TEST_P(FillerRepairRuntimeE2E, RejectsFootprintChangeWithoutRetiling)
{
  RuntimeFixture fixture(GetParam().setup);
  ASSERT_TRUE(fixture.ready());
  dpl2::fillerRepair::FillerRepairEngine engine(fixture.checker());
  ASSERT_TRUE(engine.isReady());
  dpl2::Node temporary;
  dpl2::ipl::CheckRequestOverlay request;
  ASSERT_TRUE(fixture.request(frt::CellRole::Target,
                              frt::MasterRole::WiderTarget,
                              temporary,
                              request));

  const auto outcome = engine.repair(request);

  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_TRUE(hasDiagnostic(outcome.diagnostics, "TargetFootprintMismatch"));
}

TEST_P(FillerRepairRuntimeE2E, ConcurrentCheckerCallsAreDeterministic)
{
  RuntimeFixture fixture(GetParam().setup);
  ASSERT_TRUE(fixture.ready());
  dpl2::Node temporary;
  dpl2::ipl::CheckRequestOverlay request;
  ASSERT_TRUE(fixture.request(frt::CellRole::Target,
                              frt::MasterRole::TargetNew,
                              temporary,
                              request));
  constexpr size_t kWorkers = 8;
  std::array<bool, kWorkers> legal{};
  std::array<dpl2::ipl::FillerChanges, kWorkers> changes;
  std::vector<std::thread> workers;
  for (size_t index = 0; index < kWorkers; ++index) {
    workers.emplace_back([&, index]() {
      std::vector<dpl2::CellChangeRecord> overlay = request.overlayChanges;
      legal[index] = fixture.checker().check(&temporary,
                                             request.x,
                                             request.y,
                                             request.orientation,
                                             changes[index],
                                             overlay);
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  for (size_t index = 0; index < kWorkers; ++index) {
    EXPECT_TRUE(legal[index]);
    EXPECT_TRUE(sameChanges(changes.front(), changes[index]));
  }
}

TEST(FillerRepairRuntimeE2E, TwoRowTargetUsesTheSameOneToOneContract)
{
  frt::DesignSetup setup;
  setup.doubleHeightRepairLayout = true;
  RuntimeFixture fixture(setup);
  ASSERT_TRUE(fixture.ready());
  dpl2::fillerRepair::FillerRepairEngine engine(fixture.checker());
  ASSERT_TRUE(engine.isReady()) << diagnosticText(engine.getInitDiagnostics());
  dpl2::Node temporary;
  dpl2::ipl::CheckRequestOverlay request;
  ASSERT_TRUE(fixture.request(
      frt::CellRole::Target,
      frt::MasterRole::TargetOldDoubleHeight,
      temporary,
      request,
      eUTL::PhysOrientation(eUTL::PhysOrientationE::R180)));
  const frt::PhysicalSnapshot before = fixture.design().snapshot();

  const auto outcome = engine.repair(request);

  EXPECT_TRUE(outcome.hasSolution) << diagnosticText(outcome.diagnostics);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_EQ(fixture.design().snapshot(), before);
}

TEST(FillerRepairRuntimeE2E, MissingFillerConfigurationFailsClosed)
{
  auto provider = frt::makeE2ETestProvider();
  ASSERT_NE(provider, nullptr);
  auto design = provider->createDesign({});
  ASSERT_NE(design, nullptr);
  auto infrastructure = provider->createInfrastructure(*design, {});
  ASSERT_NE(infrastructure, nullptr);
  dpl2::fillerSetting emptySetting(design->design());
  infrastructure->network()->setFillerSetting(&emptySetting);
  dpl2::ipl::ImplantLayerChecker checker(infrastructure->grid(),
                                         design->design(),
                                         infrastructure->network());
  dpl2::fillerRepair::FillerRepairEngine engine(checker);

  EXPECT_FALSE(engine.isReady());
  EXPECT_TRUE(hasDiagnostic(engine.getInitDiagnostics(),
                            "empty_filler_allow_list"));
}

INSTANTIATE_TEST_SUITE_P(
    Layouts,
    FillerRepairRuntimeE2E,
    ::testing::Values(
        LayoutCase{"Canonical", frt::DesignSetup{}},
        [] {
          frt::DesignSetup setup;
          setup.rowOriginX.fill(4);
          return LayoutCase{"ShiftedOrigin", setup};
        }()),
    [](const ::testing::TestParamInfo<LayoutCase>& info) {
      return info.param.name;
    });

}  // namespace
