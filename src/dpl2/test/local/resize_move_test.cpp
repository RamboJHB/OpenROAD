// Read-only resize/move probes followed by real DePlace commit, using fake UDM data.
#include <PlacementDRC.h>
#include <dpl2/DePlace.h>
#include <drc/ImplantLayerChecker.h>
#include <fake_udm.h>
#include <gtest/gtest.h>
#include <infrastructure/Grid.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <algorithm>
#include <array>
#include <latch>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "E2ETestProvider.h"

namespace {
using namespace dpl2;
namespace frt = dpl2::fillerRepair::test;

class ResizeMoveFixture
{
 public:
  enum class Layout { SingleHeight, DoubleHeight, GrowHeight, MoveHeight, OddWidth };

  explicit ResizeMoveFixture(Layout layout = Layout::SingleHeight,
                             bool shifted = true,
                             bool crossRowDestination = false,
                             const char* fillers = "FL2 FH2 FS2 FH1 FL2D FH1D")
      : offsetX_(shifted ? 17 : 0), offsetY_(shifted ? 20 : 0),
        provider_(frt::makeE2ETestProvider())
  {
    frt::DesignSetup setup;
    setup.rowOriginX.fill(offsetX_);
    setup.implantRuleWidth = 1;
    setup.doubleHeightRepairLayout = layout == Layout::DoubleHeight;
    design_ = provider_->createDesign(setup);
    auto& library = design_->design()->getLibAcc();
    for (int id = 0; id <= 19; ++id) {
      const_cast<eLIB::PhysLibCell&>(library.getPhysLibCell(id)).symmetry_ = 3;
    }
    if (crossRowDestination) {
      design_->desMgr()->cells_.erase(eUNL::LeafCellID(0, 111));
      for (int x : {6, 8, 10}) {
        addPhysical(300 + x, 3, x, 1);
      }
    }
    if (layout == Layout::GrowHeight) {
      design_->desMgr()->cells_.erase(eUNL::LeafCellID(0, 131));
      design_->desMgr()->cells_.erase(eUNL::LeafCellID(0, 132));
      for (int x : {6, 8, 10, 12}) {
        addPhysical(400 + x, 3, x, 3);
      }
      addPhysical(414, 1, 14, 3);
    }
    if (layout == Layout::MoveHeight) {
      design_->desMgr()->cells_.erase(eUNL::LeafCellID(0, 131));
      addPhysical(406, 3, 6, 3);
      addPhysical(408, 1, 8, 3);
    }
    if (layout == Layout::OddWidth) {
      design_->desMgr()->cells_.at(eUNL::LeafCellID(0, 122)).master
          = &library.getPhysLibCell(9);
      design_->desMgr()->cells_.erase(eUNL::LeafCellID(0, 123));
      addPhysical(123, 10, 13, 2);
    }
    for (auto& [id, physical] : design_->desMgr()->cells_) {
      physical.name = "RESIZE_" + std::to_string(id.getIndexValue());
      physical.origin = eUTL::Point2D(physical.origin.getX(),
                                      physical.origin.getY() + eUTL::UvDist(offsetY_));
    }
    for (auto& row : design_->desMgr()->rows_) {
      row.origin_ = eUTL::Point2D(row.origin_.getX(),
                                  row.origin_.getY() + eUTL::UvDist(offsetY_));
      row.bbox_.move(eUTL::UvDist(0), eUTL::UvDist(offsetY_));
    }
    dp_ = std::make_unique<DePlace>();
    dp_->getFillerSetting()->addFillerCell(fillers);
    dp_->getFillerSetting()->setPrefix("RESIZE_FILL_");
    dp_->getNetwork()->updateFillerClassification(*dp_->getFillerSetting());
    dp_->getPlacementDRC()->addChecker(
        DRCCheckerType::ImplantLayer,
        std::make_unique<ipl::ImplantLayerChecker>(
            dp_->getGrid(), design_->design(), dp_->getNetwork()));
  }

  DePlace& dp() { return *dp_; }
  Node* target() { return dp_->getNetwork()->getNode(eUNL::LeafCellID(0, 122)); }
  ipl::ImplantLayerChecker* checker()
  {
    return dynamic_cast<ipl::ImplantLayerChecker*>(
        dp_->getPlacementDRC()->getChecker(DRCCheckerType::ImplantLayer));
  }
  eUNL::VoltageArea area(int x, int row, int width, int height = 1) const
  {
    eUNL::VoltageArea result;
    result.getRects().emplace_back(
        eUTL::UvDist(offsetX_ + x),
        eUTL::UvDist(offsetY_ + row * frt::kRowHeight),
        eUTL::UvDist(offsetX_ + x + width),
        eUTL::UvDist(offsetY_ + (row + height) * frt::kRowHeight));
    return result;
  }
  std::string snapshot() const
  {
    std::ostringstream out;
    for (const auto& [id, physical] : design_->desMgr()->cells_) {
      out << id.getIndexValue() << ',' << physical.valid << ',' << physical.name
          << ',' << physical.master->getLibCellId().getIndexValue() << ','
          << physical.origin.getX().getStorage() << ','
          << physical.origin.getY().getStorage() << ','
          << static_cast<int>(physical.orient.getValue()) << ';';
    }
    for (const auto& [id, node] : dp_->getNetwork()->getNodes()) {
      out << id << ',' << node->getDbInst().getIndexValue() << ','
          << node->getMaster()->getDbMaster().getIndexValue() << ','
          << node->getLeft().v << ',' << node->getBottom().v << ','
          << static_cast<int>(node->getOrient().getValue()) << ';';
    }
    for (GridY y{0}; y < dp_->getGrid()->getRowCount(); ++y) {
      for (GridX x{0}; x < dp_->getGrid()->getRowSiteCount(); ++x) {
        const Pixel* pixel = dp_->getGrid()->gridPixel(x, y);
        out << pixel->cell << ',' << pixel->padding_reserved_by << ',' << pixel->util << ';';
      }
    }
    return out.str();
  }
  void expectNormalized(const std::vector<CellChangeRecord>& changes,
                        int masterId, int x, int row)
  {
    int targets = 0;
    std::set<eUNL::LeafCellID> committedIds;
    std::set<std::string> pendingNames;
    for (const CellChangeRecord& change : changes) {
      if (change.op_ == OpType::Add) {
        ASSERT_TRUE(std::holds_alternative<std::string>(change.cell_data_));
        EXPECT_TRUE(pendingNames.insert(std::get<std::string>(change.cell_data_)).second);
        const Master* master = dp_->getNetwork()->getMaster(change.new_lib_cell_);
        ASSERT_NE(master, nullptr);
        EXPECT_TRUE(master->isFiller());
      } else {
        ASSERT_TRUE(std::holds_alternative<eUNL::LeafCellID>(change.cell_data_));
        const auto id = std::get<eUNL::LeafCellID>(change.cell_data_);
        EXPECT_TRUE(committedIds.insert(id).second);
        const Node* old = dp_->getNetwork()->getNode(id);
        ASSERT_NE(old, nullptr);
        if (id == target()->getDbInst()) {
          ++targets;
          EXPECT_EQ(change.op_, OpType::Replace);
          EXPECT_EQ(change.orig_lib_cell_, target()->getMaster()->getDbMaster());
          EXPECT_EQ(change.new_lib_cell_, eLIB::LibCellID(0, masterId));
          EXPECT_EQ(change.x_, eUTL::UvDist(x));
          EXPECT_EQ(change.y_, eUTL::UvDist(row * frt::kRowHeight));
        } else {
          EXPECT_TRUE(old->isFiller());
        }
      }
    }
    EXPECT_EQ(targets, 1);
  }
  void expectConsistent()
  {
    for (const auto& [id, node] : dp_->getNetwork()->getNodes()) {
      EXPECT_EQ(node->getId(), id);
      EXPECT_EQ(dp_->getNetwork()->getNode(node->getDbInst()), node.get());
      const auto physical = design_->desMgr()->getPhysCell(node->getDbInst());
      ASSERT_TRUE(physical.isValid());
      EXPECT_EQ(physical.getPhysMaster().getLibCellId(), node->getMaster()->getDbMaster());
      EXPECT_EQ(physical.getOrigin().getX(), eUTL::UvDist(node->getLeft().v + offsetX_));
      EXPECT_EQ(physical.getOrigin().getY(), eUTL::UvDist(node->getBottom().v + offsetY_));
      EXPECT_EQ(physical.getOrient(), node->getOrient());
      const auto box = dp_->getGrid()->gridCovering(node.get());
      for (GridY y = box.ylo; y < box.yhi; ++y) {
        for (GridX x = box.xlo; x < box.xhi; ++x) {
          EXPECT_EQ(dp_->getGrid()->gridPixel(x, y)->cell, node.get());
        }
      }
    }
    for (GridY y{0}; y < dp_->getGrid()->getRowCount(); ++y) {
      for (GridX x{0}; x < dp_->getGrid()->getRowSiteCount(); ++x) {
        const Node* node = dp_->getGrid()->gridPixel(x, y)->cell;
        ASSERT_NE(node, nullptr) << "unfilled site (" << x.v << ',' << y.v << ')';
        EXPECT_EQ(dp_->getNetwork()->getNode(node->getId()), node);
      }
    }
    for (const auto& [id, physical] : design_->desMgr()->cells_) {
      EXPECT_EQ(dp_->getNetwork()->getNode(id) != nullptr, physical.valid);
    }
  }

 private:
  void addPhysical(int id, int master, int x, int row)
  {
    design_->desMgr()->addCell(eUNL::LeafCellID(0, id),
        &design_->design()->getLibAcc().getPhysLibCell(master), offsetX_ + x,
        row * frt::kRowHeight,
        row % 2 == 0 ? eUTL::PhysOrientationE::MX : eUTL::PhysOrientationE::R0);
  }
  int offsetX_;
  int offsetY_;
  std::unique_ptr<frt::E2ETestProvider> provider_;
  std::unique_ptr<frt::E2ETestDesign> design_;
  std::unique_ptr<DePlace> dp_;
};

TEST(DePlaceResizeMove, OriginalPositionSupportsSizeDownAndSizeUp)
{
  for (const int master : {16, 9}) {
    SCOPED_TRACE(master);
    ResizeMoveFixture fixture;
    Node* original = fixture.target();
    const auto before = fixture.snapshot();
    std::vector<CellChangeRecord> changes;
    ASSERT_TRUE(fixture.dp().isLegal(original->getDbInst(), eLIB::LibCellID(0, master), changes));
    EXPECT_EQ(fixture.snapshot(), before);
    fixture.expectNormalized(changes, master, 8, 2);
    if (master == 16) {
      EXPECT_TRUE(std::any_of(changes.begin(), changes.end(), [](const auto& change) {
        return change.op_ == OpType::Add;
      }));
    } else {
      EXPECT_TRUE(std::any_of(changes.begin(), changes.end(), [](const auto& change) {
        return change.op_ == OpType::Delete;
      }));
    }
    ASSERT_TRUE(fixture.dp().commit(changes));
    EXPECT_EQ(fixture.target(), original);
    fixture.expectConsistent();
  }
}

TEST(DePlaceResizeMove, PartialMoveCancelsPendingFillersAndRetilesOldFootprint)
{
  for (bool shifted : {false, true}) {
    SCOPED_TRACE(shifted);
    ResizeMoveFixture fixture(ResizeMoveFixture::Layout::SingleHeight, shifted);
    auto destination = fixture.area(10, 2, 4);
    const auto before = fixture.snapshot();
    std::vector<CellChangeRecord> changes;
    ASSERT_TRUE(fixture.dp().isLegal(fixture.target()->getDbInst(),
        eLIB::LibCellID(0, 1), &destination, 4, changes));
    EXPECT_EQ(fixture.snapshot(), before);
    fixture.expectNormalized(changes, 1, 10, 2);
    ASSERT_TRUE(fixture.dp().commit(changes));
    EXPECT_TRUE(fixture.dp().getGrid()->gridPixel(GridX{8}, GridY{2})->cell->isFiller());
    EXPECT_TRUE(fixture.dp().getGrid()->gridPixel(GridX{9}, GridY{2})->cell->isFiller());
    fixture.expectConsistent();
  }
}

TEST(DePlaceResizeMove, DisjointSameRowMovePreservesInstanceAndPinBinding)
{
  ResizeMoveFixture fixture;
  Node* original = fixture.target();
  const auto id = original->getDbInst();
  const int networkId = original->getId();
  const auto name = fixture.dp().getDesMgr()->cells_.at(id).name;
  Edge net;
  Pin pin;
  pin.setNode(original);
  pin.setEdge(&net);
  net.addPin(&pin);
  auto destination = fixture.area(6, 2, 2);
  std::vector<CellChangeRecord> changes;
  ASSERT_TRUE(fixture.dp().isLegal(id, eLIB::LibCellID(0, 16), &destination, 4, changes));
  fixture.expectNormalized(changes, 16, 6, 2);
  ASSERT_TRUE(fixture.dp().commit(changes));
  ASSERT_EQ(fixture.target(), original);
  EXPECT_EQ(original->getDbInst(), id);
  EXPECT_EQ(original->getId(), networkId);
  EXPECT_EQ(fixture.dp().getDesMgr()->cells_.at(id).name, name);
  ASSERT_EQ(net.getPins().size(), 1u);
  EXPECT_EQ(net.getPins().front(), &pin);
  EXPECT_EQ(pin.getNode(), original);
  EXPECT_EQ(pin.getEdge(), &net);
  for (int x = 8; x < 12; ++x) {
    EXPECT_TRUE(fixture.dp().getGrid()->gridPixel(GridX{x}, GridY{2})->cell->isFiller());
  }
  fixture.expectConsistent();
}

TEST(DePlaceResizeMove, CrossRowMoveUsesDestinationOrientationAndLiveChecker)
{
  ResizeMoveFixture fixture(ResizeMoveFixture::Layout::SingleHeight, true, true);
  auto* checker = fixture.checker();
  auto destination = fixture.area(6, 1, 4);
  std::vector<CellChangeRecord> changes;
  ASSERT_TRUE(fixture.dp().isLegal(fixture.target()->getDbInst(),
      eLIB::LibCellID(0, 1), &destination, 12, changes));
  fixture.expectNormalized(changes, 1, 6, 1);
  const auto targetChange = std::find_if(changes.begin(), changes.end(), [&](const auto& change) {
    return change.cell_data_ == CellData{fixture.target()->getDbInst()};
  });
  ASSERT_NE(targetChange, changes.end());
  EXPECT_EQ(targetChange->orientation_, eUTL::PhysOrientationE::R0);
  ASSERT_TRUE(fixture.dp().commit(changes));
  fixture.expectConsistent();
  EXPECT_EQ(fixture.checker(), checker);
  changes.clear();
  ASSERT_TRUE(fixture.dp().isLegal(fixture.target()->getDbInst(), eLIB::LibCellID(0, 16), changes));
  fixture.expectNormalized(changes, 16, 6, 1);
  ASSERT_TRUE(fixture.dp().commit(changes));
  EXPECT_EQ(fixture.checker(), checker);
  fixture.expectConsistent();
}

TEST(DePlaceResizeMove, SupportsOneRowToTwoRowsAndTwoRowsToOneRow)
{
  for (bool grow : {false, true}) {
    SCOPED_TRACE(grow);
    ResizeMoveFixture fixture(grow ? ResizeMoveFixture::Layout::GrowHeight
                                  : ResizeMoveFixture::Layout::DoubleHeight);
    const int master = grow ? 12 : 1;
    const auto before = fixture.snapshot();
    Node* original = fixture.target();
    std::vector<CellChangeRecord> changes;
    ASSERT_TRUE(fixture.dp().isLegal(original->getDbInst(), eLIB::LibCellID(0, master), changes));
    EXPECT_EQ(fixture.snapshot(), before);
    fixture.expectNormalized(changes, master, 8, 2);
    ASSERT_TRUE(fixture.dp().commit(changes));
    EXPECT_EQ(fixture.target(), original);
    EXPECT_EQ(original->getHeight().v, (grow ? 2 : 1) * frt::kRowHeight);
    fixture.expectConsistent();
  }
}

TEST(DePlaceResizeMove, HeightIncreaseCanSearchAwayFromBlockedOriginalLocation)
{
  ResizeMoveFixture fixture(ResizeMoveFixture::Layout::MoveHeight);
  const auto before = fixture.snapshot();
  std::vector<CellChangeRecord> changes;
  ASSERT_FALSE(fixture.dp().isLegal(fixture.target()->getDbInst(),
      eLIB::LibCellID(0, 17), changes));
  EXPECT_TRUE(changes.empty());
  EXPECT_EQ(fixture.snapshot(), before);
  auto destination = fixture.area(6, 2, 2, 2);
  ASSERT_TRUE(fixture.dp().isLegal(fixture.target()->getDbInst(),
      eLIB::LibCellID(0, 17), &destination, 12, changes));
  fixture.expectNormalized(changes, 17, 6, 2);
  EXPECT_EQ(fixture.snapshot(), before);
  ASSERT_TRUE(fixture.dp().commit(changes));
  EXPECT_EQ(fixture.target()->getHeight().v, 2 * frt::kRowHeight);
  fixture.expectConsistent();
}

TEST(DePlaceResizeMove, FailedRemovalFillDoesNotRejectLegalFinalPlacement)
{
  // An odd-width old cell cannot be independently tiled by width-two fillers.
  ResizeMoveFixture fixture(ResizeMoveFixture::Layout::OddWidth, true, false,
                            "FL2 FH2 FS2");
  const auto before = fixture.snapshot();
  std::vector<CellChangeRecord> changes;
  ASSERT_TRUE(fixture.dp().isLegal(fixture.target()->getDbInst(), eLIB::LibCellID(0, 9), changes));
  EXPECT_EQ(fixture.snapshot(), before);
  fixture.expectNormalized(changes, 9, 8, 2);
  ASSERT_EQ(changes.size(), 1u);
  ASSERT_TRUE(fixture.dp().commit(changes));
  fixture.expectConsistent();
}

TEST(DePlaceResizeMove, ImpossibleResizeAndBlockedSearchLeaveInputUntouched)
{
  ResizeMoveFixture fixture;
  const CellChangeRecord sentinel{OpType::Add, std::string("EXISTING_PENDING"),
      eUTL::UvDist(0), eUTL::UvDist(0), eLIB::LibCellID(),
      eLIB::LibCellID(0, 18), eUTL::PhysOrientationE::MX};
  const auto before = fixture.snapshot();
  std::vector<CellChangeRecord> changes{sentinel};
  // A second row intersects committed standard cells that cannot be removed.
  EXPECT_FALSE(fixture.dp().isLegal(fixture.target()->getDbInst(), eLIB::LibCellID(0, 12), changes));
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes.front().cell_data_, sentinel.cell_data_);
  EXPECT_EQ(fixture.snapshot(), before);
  auto blocked = fixture.area(0, 0, 2);
  EXPECT_FALSE(fixture.dp().isLegal(fixture.target()->getDbInst(),
      eLIB::LibCellID(0, 16), &blocked, 24, changes));
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes.front().cell_data_, sentinel.cell_data_);
  EXPECT_EQ(fixture.snapshot(), before);
  eUNL::VoltageArea empty;
  EXPECT_FALSE(fixture.dp().isLegal(fixture.target()->getDbInst(),
      eLIB::LibCellID(0, 16), &empty, 24, changes));
  EXPECT_EQ(fixture.snapshot(), before);
}

TEST(DePlaceResizeMove, RefillInsideTargetPaddingIsRejectedBeforePublishingChanges)
{
  for (bool move : {false, true}) {
    SCOPED_TRACE(move);
    ResizeMoveFixture fixture;
    const auto id = fixture.target()->getDbInst();
    fixture.dp().setPadding(id, move ? 1 : 0, move ? 0 : 1);
    auto destination = fixture.area(10, 2, 4);
    const auto before = fixture.snapshot();
    std::vector<CellChangeRecord> changes;
    const bool accepted = move
        ? fixture.dp().isLegal(id, eLIB::LibCellID(0, 1), &destination, 4, changes)
        : fixture.dp().isLegal(id, eLIB::LibCellID(0, 16), changes);
    EXPECT_FALSE(accepted);
    EXPECT_TRUE(changes.empty());
    EXPECT_EQ(fixture.snapshot(), before);
  }
}

TEST(DePlaceResizeMove, ConcurrentProbesKeepRemovalAndFinalTransactionsRequestLocal)
{
  ResizeMoveFixture fixture;
  const auto before = fixture.snapshot();
  const auto id = fixture.target()->getDbInst();
  auto destination = fixture.area(10, 2, 4);
  std::array<std::vector<CellChangeRecord>, 4> changes;
  std::array<bool, 4> accepted{};
  std::array<std::thread, 4> workers;
  std::latch start(4);
  for (size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::thread([&, index] {
      start.arrive_and_wait();
      accepted[index] = index % 2 == 0
          ? fixture.dp().isLegal(id, eLIB::LibCellID(0, 16), changes[index])
          : fixture.dp().isLegal(id, eLIB::LibCellID(0, 1), &destination, 4, changes[index]);
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  EXPECT_EQ(fixture.snapshot(), before);
  for (size_t index = 0; index < workers.size(); ++index) {
    SCOPED_TRACE(index);
    ASSERT_TRUE(accepted[index]);
    fixture.expectNormalized(changes[index], index % 2 == 0 ? 16 : 1,
                              index % 2 == 0 ? 8 : 10, 2);
    const auto& reference = changes[index % 2];
    ASSERT_EQ(changes[index].size(), reference.size());
    for (size_t record = 0; record < reference.size(); ++record) {
      const auto& actual = changes[index][record];
      const auto& expected = reference[record];
      EXPECT_EQ(actual.op_, expected.op_);
      EXPECT_EQ(actual.cell_data_, expected.cell_data_);
      EXPECT_EQ(actual.x_, expected.x_);
      EXPECT_EQ(actual.y_, expected.y_);
      EXPECT_EQ(actual.orig_lib_cell_, expected.orig_lib_cell_);
      EXPECT_EQ(actual.new_lib_cell_, expected.new_lib_cell_);
      EXPECT_EQ(actual.orientation_, expected.orientation_);
    }
  }
}

}  // namespace
