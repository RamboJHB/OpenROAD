// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <PlacementDRC.h>
#include <dpl2/DePlace.h>
#include <drc/ImplantLayerChecker.h>
#include <fillerInsertion/FillerInsertionEngine.h>
#include <gtest/gtest.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Padding.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <algorithm>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "fake_udm.h"

namespace fi = dpl2::fillerInsertion;

namespace {

bool sameChanges(const std::vector<dpl2::CellChangeRecord>& left,
                 const std::vector<dpl2::CellChangeRecord>& right)
{
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto& a = left[index];
    const auto& b = right[index];
    if (a.op_ != b.op_ || a.cell_data_ != b.cell_data_ || a.x_ != b.x_
        || a.y_ != b.y_ || a.orig_lib_cell_ != b.orig_lib_cell_
        || a.new_lib_cell_ != b.new_lib_cell_
        || a.orientation_.getValue() != b.orientation_.getValue()) {
      return false;
    }
  }
  return true;
}

class InsertionFixture
{
 public:
  InsertionFixture()
  {
    db_.coreSite.width_ = eUTL::UvDist(1);
    db_.coreSite.height_ = eUTL::UvDist(8);
    one_ = &db_.addMaster("F1", 1, 1, 8, true);
    wide_ = &db_.addMaster("F2", 2, 2, 8, true);
    tall_ = &db_.addMaster("F2D", 3, 2, 16, true);
    ordinary_ = &db_.addMaster("NOT_FILLER", 4, 1, 8, false);
    db_.desMgr().addRow(0, 0, 1, 8, 4).orient_ = eUTL::PhysOrientationE::MX;
    db_.desMgr().addRow(0, 8, 1, 8, 4).orient_ = eUTL::PhysOrientationE::R0;
    db_.activate();

    padding_->setDesginManager(&db_.desMgr());
    grid_.setCore(eUTL::Rect(
        eUTL::UvDist(0), eUTL::UvDist(0), eUTL::UvDist(4), eUTL::UvDist(16)));
    grid_.examineRows(&db_.desMgr());
    grid_.initGrid(&db_.desMgr(), padding_, 100, 100);
    network_.setCore(grid_.getCore());
    setting_ = std::make_unique<dpl2::fillerSetting>(&db_.design);
    network_.setFillerSetting(setting_.get());
    for (const eLIB::PhysLibCell* master : {one_, wide_, tall_, ordinary_}) {
      network_.addMaster(*master, *setting_, &grid_, &edgeTypes_);
    }
  }

  void configure(const std::string& names,
                 bool checkDrc = false,
                 bool publish = true)
  {
    setting_->addFillerCell(names);
    setting_->setCheckDRC(checkDrc);
    if (publish) {
      network_.updateFillerClassification(*setting_);
    }
  }

  void buildChecker()
  {
    drc_ = std::make_unique<dpl2::PlacementDRC>(&grid_);
    drc_->addChecker(dpl2::DRCCheckerType::ImplantLayer,
                     std::make_unique<dpl2::ipl::ImplantLayerChecker>(
                         &grid_, &db_.design, &network_));
  }

  fi::InsertionOutcome plan() const
  {
    return fi::FillerInsertionEngine(grid_, network_, *setting_, drc_.get())
        .plan();
  }

  dpl2::Grid& grid() { return grid_; }
  dpl2::Network& network() { return network_; }
  dpl2::fillerSetting& setting() { return *setting_; }
  dpl2::PlacementDRC* drc() { return drc_.get(); }
  const eLIB::PhysLibCell& one() const { return *one_; }
  const eLIB::PhysLibCell& tall() const { return *tall_; }

  void placeExistingWideFiller()
  {
    const eUNL::LeafCellID id(0, 100);
    db_.desMgr().addCell(id,
                         wide_,
                         0,
                         0,
                         eUTL::PhysOrientationE::MX,
                         eUNL::PhysObjStatus::PLACED,
                         "EXISTING_FILLER");
    network_.addNode(id, &db_.desMgr());
    grid_.paintPixel(network_.getNode(id));
  }

 private:
  fake_udm::DesignDb db_;
  eLIB::PhysLibCell* one_ = nullptr;
  eLIB::PhysLibCell* wide_ = nullptr;
  eLIB::PhysLibCell* tall_ = nullptr;
  eLIB::PhysLibCell* ordinary_ = nullptr;
  std::shared_ptr<dpl2::Padding> padding_ = std::make_shared<dpl2::Padding>();
  dpl2::Grid grid_;
  dpl2::Network network_;
  dpl2::EdgeTypeTable edgeTypes_;
  std::unique_ptr<dpl2::fillerSetting> setting_;
  std::unique_ptr<dpl2::PlacementDRC> drc_;
};

TEST(FillerInsertionRuntime, SetFillerOptionCanSelectDoubleHeightMasters)
{
  InsertionFixture fixture;
  fixture.configure("F2D F1", true);
  fixture.buildChecker();
  const uint64_t revision = fixture.setting().getRevision();

  const fi::InsertionOutcome outcome = fixture.plan();

  ASSERT_TRUE(outcome.hasSolution);
  ASSERT_TRUE(outcome.complete);
  ASSERT_EQ(outcome.changes.size(), 2U);
  EXPECT_EQ(outcome.coveredSites, 8U);
  for (const dpl2::CellChangeRecord& change : outcome.changes) {
    EXPECT_EQ(change.op_, dpl2::OpType::Add);
    EXPECT_TRUE(std::holds_alternative<std::string>(change.cell_data_));
    EXPECT_FALSE(change.orig_lib_cell_.isValid());
    EXPECT_EQ(change.new_lib_cell_, fixture.tall().getLibCellId());
    EXPECT_EQ(change.y_, eUTL::UvDist(0));
    EXPECT_EQ(change.orientation_.getValue(), eUTL::PhysOrientationE::MX);
  }
  EXPECT_EQ(fixture.setting().getRevision(), revision);
  for (int row = 0; row < 2; ++row) {
    for (int column = 0; column < 4; ++column) {
      EXPECT_EQ(
          fixture.grid().gridPixel(dpl2::GridX{column}, dpl2::GridY{row})->cell,
          nullptr);
    }
  }
}

TEST(FillerInsertionRuntime, FollowOrderAndGeometricOrderAreDistinct)
{
  InsertionFixture fixture;
  fixture.configure("F1 F2D");
  fixture.setting().setFollowOrder(true);
  const fi::InsertionOutcome configuredOrder = fixture.plan();
  ASSERT_TRUE(configuredOrder.hasSolution);
  ASSERT_EQ(configuredOrder.changes.size(), 8U);
  EXPECT_TRUE(std::all_of(configuredOrder.changes.begin(),
                          configuredOrder.changes.end(),
                          [&fixture](const auto& change) {
                            return change.new_lib_cell_
                                   == fixture.one().getLibCellId();
                          }));

  fixture.setting().setFollowOrder(false);
  const fi::InsertionOutcome geometricOrder = fixture.plan();
  ASSERT_TRUE(geometricOrder.hasSolution);
  ASSERT_EQ(geometricOrder.changes.size(), 2U);
  EXPECT_TRUE(std::all_of(geometricOrder.changes.begin(),
                          geometricOrder.changes.end(),
                          [&fixture](const auto& change) {
                            return change.new_lib_cell_
                                   == fixture.tall().getLibCellId();
                          }));
}

TEST(FillerInsertionRuntime, AvoidPatternCanMakeExactFillImpossible)
{
  InsertionFixture fixture;
  fixture.configure("F2D");
  fixture.setting().addAvoidPattern("2:2");
  const fi::InsertionOutcome outcome = fixture.plan();
  EXPECT_FALSE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  ASSERT_FALSE(outcome.diagnostics.empty());
  EXPECT_EQ(outcome.diagnostics.front().code, "space_not_tileable");
}

TEST(FillerInsertionRuntime, StaleClassificationFailsClosed)
{
  InsertionFixture fixture;
  fixture.configure("F2D", false, false);
  const fi::InsertionOutcome outcome = fixture.plan();
  EXPECT_FALSE(outcome.hasSolution);
  ASSERT_FALSE(outcome.diagnostics.empty());
  EXPECT_EQ(outcome.diagnostics.front().code, "stale_filler_catalog");
}

TEST(FillerInsertionRuntime, CheckDrcRequiresPublishedChecker)
{
  InsertionFixture fixture;
  fixture.configure("F2D", true);
  const fi::InsertionOutcome outcome = fixture.plan();
  EXPECT_FALSE(outcome.hasSolution);
  ASSERT_FALSE(outcome.diagnostics.empty());
  EXPECT_EQ(outcome.diagnostics.front().code, "missing_insertion_checker");
}

TEST(FillerInsertionRuntime, AvoidPatternChecksCommittedFillerBoundary)
{
  InsertionFixture fixture;
  fixture.configure("F1 F2");
  fixture.placeExistingWideFiller();
  fixture.setting().addAvoidPattern("2:1 2:2");

  const fi::InsertionOutcome outcome = fixture.plan();

  EXPECT_FALSE(outcome.hasSolution);
  ASSERT_FALSE(outcome.diagnostics.empty());
  EXPECT_EQ(outcome.diagnostics.front().code, "avoid_pattern_rejected");
}

TEST(FillerInsertionRuntime, PrefixAndFitSpaceControlGeneratedTransaction)
{
  InsertionFixture fixture;
  fixture.configure("F2");
  fixture.setting().setPrefix("MH_");
  fixture.setting().setFitSpace(false);
  fixture.grid().pixel(dpl2::GridY{0}, dpl2::GridX{3}).is_valid = false;

  const fi::InsertionOutcome outcome = fixture.plan();

  ASSERT_TRUE(outcome.hasSolution);
  EXPECT_FALSE(outcome.complete);
  EXPECT_EQ(outcome.fillableSites, 7U);
  EXPECT_EQ(outcome.coveredSites, 6U);
  ASSERT_FALSE(outcome.changes.empty());
  for (const auto& change : outcome.changes) {
    const auto* name = std::get_if<std::string>(&change.cell_data_);
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->find("MH_INSERT_"), 0U);
  }
}

TEST(FillerInsertionRuntime, ImplantCheckerRejectsMalformedAddBatch)
{
  InsertionFixture fixture;
  fixture.configure("F2D", false);
  fixture.buildChecker();
  fi::InsertionOutcome outcome = fixture.plan();
  ASSERT_TRUE(outcome.hasSolution);
  outcome.changes.front().orientation_ = eUTL::PhysOrientationE::R0;
  const auto* checker = dynamic_cast<const dpl2::ipl::ImplantLayerChecker*>(
      fixture.drc()->getChecker(dpl2::DRCCheckerType::ImplantLayer));
  ASSERT_NE(checker, nullptr);
  const dpl2::ipl::CheckResult checked
      = checker->checkFillerInsertion(outcome.changes);
  EXPECT_FALSE(checked.isLegal);
  EXPECT_TRUE(std::any_of(checked.diagnostics.begin(),
                          checked.diagnostics.end(),
                          [](const auto& d) {
                            return d.status
                                   == "added_filler_orientation_mismatch";
                          }));

  outcome = fixture.plan();
  ASSERT_TRUE(outcome.hasSolution);
  outcome.changes.front().orig_lib_cell_ = fixture.one().getLibCellId();
  const dpl2::ipl::CheckResult originalMaster
      = checker->checkFillerInsertion(outcome.changes);
  EXPECT_FALSE(originalMaster.isLegal);
  EXPECT_TRUE(std::any_of(originalMaster.diagnostics.begin(),
                          originalMaster.diagnostics.end(),
                          [](const auto& d) {
                            return d.status
                                   == "added_filler_has_original_master";
                          }));
}

TEST(FillerInsertionRuntime, ConcurrentPlanningIsReadOnlyAndDeterministic)
{
  InsertionFixture fixture;
  fixture.configure("F2D F1", false);
  const fi::FillerInsertionEngine engine(
      fixture.grid(), fixture.network(), fixture.setting());
  std::vector<std::future<fi::InsertionOutcome>> futures;
  for (int index = 0; index < 8; ++index) {
    futures.push_back(
        std::async(std::launch::async, [&engine]() { return engine.plan(); }));
  }
  const fi::InsertionOutcome first = futures.front().get();
  ASSERT_TRUE(first.hasSolution);
  for (std::size_t index = 1; index < futures.size(); ++index) {
    const fi::InsertionOutcome current = futures[index].get();
    ASSERT_TRUE(current.hasSolution);
    EXPECT_TRUE(sameChanges(first.changes, current.changes));
  }
}

TEST(FillerInsertionRuntime, SettingRevisionChangesOnlyForRealChanges)
{
  InsertionFixture fixture;
  const uint64_t initial = fixture.setting().getRevision();
  fixture.setting().setPrefix("ECOFILLER");
  EXPECT_EQ(fixture.setting().getRevision(), initial);
  fixture.setting().setPrefix("MH_FILL");
  EXPECT_EQ(fixture.setting().getRevision(), initial + 1);
  fixture.setting().addFillerCell("F1");
  const uint64_t configured = fixture.setting().getRevision();
  fixture.setting().addFillerCell("F1");
  EXPECT_EQ(fixture.setting().getRevision(), configured);

  EXPECT_THROW(fixture.setting().addAvoidPattern("1:2 invalid"),
               std::invalid_argument);
  EXPECT_FALSE(fixture.setting().needAvoidAbut({1, 2}));
  EXPECT_EQ(fixture.setting().getRevision(), configured);
  EXPECT_THROW(fixture.setting().addAvoidPattern("1:2:3"),
               std::invalid_argument);
  EXPECT_EQ(fixture.setting().getRevision(), configured);
}

TEST(FillerInsertionRuntime, DePlacePublishesLateSetFillerOptionBeforePlanning)
{
  InsertionFixture fixture;
  dpl2::DePlace deplace;
  dpl2::fillerSetting* setting = deplace.getFillerSetting();
  ASSERT_NE(setting, nullptr);
  const uint64_t initialPublication = deplace.getPublishedFillerRevision();
  setting->addFillerCell("F2D");
  setting->setCheckDRC(false);
  ASSERT_NE(setting->getRevision(), initialPublication);
  std::vector<dpl2::CellChangeRecord> changes;

  ASSERT_TRUE(deplace.planFillerInsertion(changes));

  EXPECT_EQ(deplace.getPublishedFillerRevision(), setting->getRevision());
  ASSERT_EQ(changes.size(), 2U);
  EXPECT_TRUE(std::all_of(
      changes.begin(), changes.end(), [&fixture](const auto& change) {
        return change.op_ == dpl2::OpType::Add
               && change.new_lib_cell_ == fixture.tall().getLibCellId();
      }));
}

}  // namespace
