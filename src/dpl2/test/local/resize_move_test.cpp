// Read-only resize/move probes and commits: std changes stay in Network/Grid,
// while filler changes update the fake UDM as well.
#include <PlacementDRC.h>
#include <dpl2/DePlace.h>
#include <drc/ImplantLayerChecker.h>
#include <fake_udm.h>
#include <fillerRepair/FillerRepairEngine.h>
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

namespace dpl2 {
class TestDePlaceCmd
{
 public:
  static void assertPlacementHelpers(DePlace& dp, Node* cell)
  {
    const GridPt expected{dp.getGrid()->gridX(cell), dp.getGrid()->gridSnapDownY(cell)};
    const auto expectedOrientation = cell->getOrient();
    const auto runtime = dp.initialLocation(cell, false);
    EXPECT_EQ(runtime.x, cell->getLeft());
    EXPECT_EQ(runtime.y, cell->getBottom());
    const auto runtimeGrid = dp.legalGridPt(cell, false);
    EXPECT_EQ(runtimeGrid.x, expected.x);
    EXPECT_EQ(runtimeGrid.y, expected.y);
    const auto top = dp.legalPt(cell, DbuPt{cell->getLeft(), DbuY{1000}});
    EXPECT_EQ(top.x, cell->getLeft());
    EXPECT_EQ(top.y, dp.getGrid()->gridYToDbu(dp.getGrid()->getRowCount()) - cell->getHeight());
    dp.setPadding(cell->getDbInst(), 2, 0);
    const auto padded = dp.initialLocation(cell, true);
    EXPECT_EQ(padded.x, cell->getLeft() - gridToDbu(GridX{2}, dp.getGrid()->getSiteWidth()));
    EXPECT_EQ(padded.y, cell->getBottom());
    dp.setPadding(cell->getDbInst(), 0, 0);

    dp.unplaceCell(cell);
    cell->setOrient(PhysOrientationE::MX);
    dp.placeCell(cell, expected.x, expected.y);
    EXPECT_EQ(cell->getOrient(), expectedOrientation);
  }
};
}  // namespace dpl2

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
    originalStdUdm_ = standardUdmSnapshot();
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
    EXPECT_EQ(standardUdmSnapshot(), originalStdUdm_);
    for (const auto& [id, node] : dp_->getNetwork()->getNodes()) {
      EXPECT_EQ(node->getId(), id);
      EXPECT_EQ(dp_->getNetwork()->getNode(node->getDbInst()), node.get());
      const auto physical = design_->desMgr()->getPhysCell(node->getDbInst());
      ASSERT_TRUE(physical.isValid());
      if (node->isFiller()) {
        EXPECT_EQ(physical.getPhysMaster().getLibCellId(), node->getMaster()->getDbMaster());
        EXPECT_EQ(physical.getOrigin().getX(), eUTL::UvDist(node->getLeft().v + offsetX_));
        EXPECT_EQ(physical.getOrigin().getY(), eUTL::UvDist(node->getBottom().v + offsetY_));
        EXPECT_EQ(physical.getOrient(), node->getOrient());
      }
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
  std::string standardUdmSnapshot() const
  {
    std::ostringstream out;
    for (const auto& [id, cell] : design_->desMgr()->cells_) {
      const Master* master = dp_->getNetwork()->getMaster(cell.master->getLibCellId());
      if (master == nullptr || master->isFiller()) {
        continue;
      }
      out << id.getIndexValue() << ',' << cell.valid << ',' << cell.name << ','
          << cell.master->getLibCellId().getIndexValue() << ','
          << cell.origin.getX().getStorage() << ',' << cell.origin.getY().getStorage() << ','
          << static_cast<int>(cell.orient.getValue()) << ',' << static_cast<int>(cell.status) << ';';
    }
    return out.str();
  }
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
  std::string originalStdUdm_;
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

TEST(DePlaceResizeMove, SameFootprintSwapSkipsRemovalPreparation)
{
  struct CountingChecker : DRCChecker
  {
    CountingChecker(Grid* grid, eUNL::Design* design, Network* network)
        : DRCChecker(grid, design), checker(grid, design, network) {}
    ipl::ImplantLayerChecker checker;
    mutable int removals = 0;
    bool check(const Node* node, GridX x, GridY y,
                const eUTL::PhysOrientation& orientation) const override
    {
      return checker.check(node, x, y, orientation);
    }
    bool check(const Node* node, GridX x, GridY y,
                const eUTL::PhysOrientation& orientation,
                std::vector<CellChangeRecord>& changes,
                std::vector<CellChangeRecord>& overlays) const override
    {
      removals += node == nullptr;
      return checker.check(node, x, y, orientation, changes, overlays);
    }
  };
  ResizeMoveFixture fixture;
  auto counted = std::make_unique<CountingChecker>(fixture.dp().getGrid(),
      fixture.checker()->getDesign(), fixture.dp().getNetwork());
  auto* const checker = counted.get();
  fixture.dp().getPlacementDRC()->addChecker(DRCCheckerType::ImplantLayer, std::move(counted));
  const auto before = fixture.snapshot();
  std::vector<CellChangeRecord> changes;
  ASSERT_TRUE(fixture.dp().isLegal(fixture.target()->getDbInst(), eLIB::LibCellID(0, 2), changes));
  fixture.expectNormalized(changes, 2, 8, 2);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(checker->removals, 0);
  EXPECT_EQ(fixture.snapshot(), before);
  changes.clear();
  ASSERT_TRUE(fixture.dp().isLegal(fixture.target()->getDbInst(), eLIB::LibCellID(0, 16), changes));
  fixture.expectNormalized(changes, 16, 8, 2);
  EXPECT_EQ(checker->removals, 1);
  EXPECT_EQ(fixture.snapshot(), before);

  changes.clear();
  // Unlike isLegal's fast path, explicit findLeg always prepares removal.
  ASSERT_EQ(fixture.dp().findLeg(fixture.target(), nullptr, 0,
                                eLIB::LibCellID(0, 2), changes),
            std::make_pair(8, 2 * frt::kRowHeight));
  fixture.expectNormalized(changes, 2, 8, 2);
  EXPECT_EQ(changes.size(), 1u);
  EXPECT_EQ(checker->removals, 2);
  EXPECT_EQ(fixture.snapshot(), before);
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

TEST(DePlaceResizeMove, FindLegInstanceMovesAndShrinksWhilePreservingPinBinding)
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
  const auto before = fixture.snapshot();
  const auto failure = std::make_pair(-1, -1);
  const eLIB::LibCellID narrower(0, 16);
  const CellChangeRecord sentinel{OpType::Add, std::string("EXISTING_PENDING"),
      eUTL::UvDist(0), eUTL::UvDist(0), eLIB::LibCellID(), narrower,
      eUTL::PhysOrientationE::MX};
  std::vector<CellChangeRecord> changes{sentinel};
  // The only site inside the voltage area is two DBU from the old origin.
  for (const int diameter : {0, 1}) {
    SCOPED_TRACE(diameter);
    EXPECT_EQ(fixture.dp().findLeg(original, &destination, diameter, narrower, changes),
              failure);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes.front().cell_data_, sentinel.cell_data_);
    EXPECT_EQ(changes.front().op_, sentinel.op_);
    EXPECT_EQ(changes.front().new_lib_cell_, sentinel.new_lib_cell_);
    EXPECT_EQ(fixture.snapshot(), before);
  }
  changes.clear();
  ASSERT_EQ(fixture.dp().findLeg(original, &destination, 2, narrower, changes),
            std::make_pair(6, 2 * frt::kRowHeight));
  EXPECT_EQ(fixture.snapshot(), before);
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

TEST(DePlaceResizeMove, FindLegInstanceGrowsAtOriginalSiteWithZeroDiameter)
{
  ResizeMoveFixture fixture;
  const Node* const original = fixture.target();
  const Node* const covered = fixture.dp().getGrid()->gridPixel(GridX{12}, GridY{2})->cell;
  ASSERT_NE(covered, nullptr);
  ASSERT_TRUE(covered->isFiller());
  const auto deletedFiller = covered->getDbInst();
  const auto before = fixture.snapshot();
  std::vector<CellChangeRecord> changes;
  ASSERT_EQ(fixture.dp().findLeg(original, nullptr, 0, eLIB::LibCellID(0, 9), changes),
            std::make_pair(8, 2 * frt::kRowHeight));
  EXPECT_EQ(fixture.snapshot(), before);
  fixture.expectNormalized(changes, 9, 8, 2);
  EXPECT_TRUE(std::any_of(changes.begin(), changes.end(), [&](const auto& change) {
    return change.op_ == OpType::Delete && change.cell_data_ == CellData{deletedFiller};
  }));
  ASSERT_TRUE(fixture.dp().commit(changes));
  EXPECT_EQ(fixture.target(), original);
  EXPECT_EQ(fixture.dp().getNetwork()->getNode(deletedFiller), nullptr);
  const Node* const addition = fixture.dp().getGrid()->gridPixel(GridX{13}, GridY{2})->cell;
  ASSERT_NE(addition, nullptr);
  EXPECT_TRUE(addition->isFiller());
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

TEST(DePlaceResizeMove, PlacementHelpersUseRuntimeOriginAndSiteOrientationAfterStdMove)
{
  ResizeMoveFixture fixture(ResizeMoveFixture::Layout::SingleHeight, true, true);
  DePlace& dp = fixture.dp();
  Node* const cell = fixture.target();
  const auto id = cell->getDbInst();
  const auto originalPhysical = dp.getDesMgr()->getPhysCell(id);
  const auto originalOrigin = originalPhysical.getOrigin();
  const auto originalOrientation = originalPhysical.getOrient();
  ASSERT_EQ(originalOrientation, PhysOrientationE::MX);

  auto destination = fixture.area(6, 1, 4);
  std::vector<CellChangeRecord> changes;
  ASSERT_TRUE(dp.isLegal(id, cell->getMaster()->getDbMaster(), &destination, 12, changes));
  ASSERT_TRUE(dp.commit(changes));
  ASSERT_EQ(cell->getLeft(), DbuX{6});
  ASSERT_EQ(cell->getBottom(), DbuY{frt::kRowHeight});
  ASSERT_EQ(cell->getOrient(), PhysOrientationE::R0);

  // Std commits deliberately leave UDM at its old row/orientation. Neither
  // legalization seeds nor later placement may fall back to that stale state.
  const auto beforeHelpers = fixture.snapshot();
  TestDePlaceCmd::assertPlacementHelpers(dp, cell);
  EXPECT_EQ(fixture.snapshot(), beforeHelpers);

  const auto physical = dp.getDesMgr()->getPhysCell(id);
  EXPECT_EQ(physical.getOrigin().getX(), originalOrigin.getX());
  EXPECT_EQ(physical.getOrigin().getY(), originalOrigin.getY());
  EXPECT_EQ(physical.getOrient(), originalOrientation);
  fixture.expectConsistent();
}

TEST(DePlaceResizeMove, FailedCandidatesDoNotPublishTheirChangesOrOrientation)
{
  struct RejectFirstRow : DRCChecker
  {
    RejectFirstRow(Grid* grid, eUNL::Design* design) : DRCChecker(grid, design) {}
    mutable bool rejectedMirrored = false;
    mutable PhysOrientation acceptedOrientation{PhysOrientationE::R180};
    bool check(const Node*, GridX, GridY y, const PhysOrientation&) const override
    {
      return y.v == 1;
    }
    bool check(const Node*, GridX, GridY y, const PhysOrientation& orientation,
               std::vector<CellChangeRecord>& changes,
               std::vector<CellChangeRecord>&) const override
    {
      if (y.v != 1) {
        rejectedMirrored |= y.v == 0 && orientation == PhysOrientationE::MX;
        changes.push_back({OpType::Add, std::string("REJECTED_CANDIDATE"), UvDist(0),
            UvDist(0), LibCellID(), LibCellID(0, 18), orientation});
        return false;
      }
      acceptedOrientation = orientation;
      return true;
    }
  };
  ResizeMoveFixture fixture;
  auto gate = std::make_unique<RejectFirstRow>(fixture.dp().getGrid(), fixture.checker()->getDesign());
  const auto* observed = gate.get();
  fixture.dp().getPlacementDRC()->addChecker(DRCCheckerType::EdgeSpacing, std::move(gate));
  const auto pin = eUNL::PinID(0, 900);
  fixture.dp().getDesMgr()->addPin(eUNL::PhysPinID(pin), 35, 20);
  const auto before = fixture.snapshot();
  std::vector<CellChangeRecord> changes;
  const auto location = fixture.dp().findLeg(pin, nullptr, 8, LibCellID(0, 18), changes);
  ASSERT_NE(location, (std::pair<int, int>{-1, -1}));
  EXPECT_TRUE(observed->rejectedMirrored);
  EXPECT_EQ(observed->acceptedOrientation, PhysOrientationE::R0);
  EXPECT_EQ(location.second, frt::kRowHeight);
  ASSERT_FALSE(changes.empty());
  EXPECT_EQ(changes.back().op_, OpType::Add);
  EXPECT_EQ(changes.back().new_lib_cell_, LibCellID(0, 18));
  EXPECT_EQ(changes.back().orientation_, observed->acceptedOrientation);
  for (const auto& change : changes) {
    if (const auto* name = std::get_if<std::string>(&change.cell_data_)) {
      EXPECT_NE(*name, "REJECTED_CANDIDATE");
    }
  }
  EXPECT_EQ(fixture.snapshot(), before);
}

TEST(DePlaceResizeMove, NewInsertionDoesNotTreatNetworkNodeZeroAsItsReplacement)
{
  ResizeMoveFixture fixture;
  const Node* zero = fixture.dp().getNetwork()->getNode(0);
  ASSERT_NE(zero, nullptr);
  ASSERT_FALSE(zero->isFiller());
  const auto origin = fixture.dp().getDesMgr()->getPhysCell(zero->getDbInst()).getOrigin();
  const auto pin = eUNL::PinID(0, 900);
  fixture.dp().getDesMgr()->addPin(eUNL::PhysPinID(pin), origin.getX().getStorage(), origin.getY().getStorage());
  const auto before = fixture.snapshot();
  std::vector<CellChangeRecord> changes;
  EXPECT_EQ(fixture.dp().findLeg(pin, nullptr, 0, LibCellID(0, 18), changes),
            (std::pair<int, int>{-1, -1}));
  EXPECT_TRUE(changes.empty());
  EXPECT_EQ(fixture.dp().getNetwork()->getNode(0), zero);
  EXPECT_EQ(fixture.snapshot(), before);
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
  EXPECT_EQ(changes.front().orientation_, sentinel.orientation_);
  EXPECT_EQ(changes.front().x_, sentinel.x_);
  EXPECT_EQ(changes.front().y_, sentinel.y_);
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

class SingleHeightFillerRemovalTest : public ::testing::TestWithParam<int>
{
};

TEST_P(SingleHeightFillerRemovalTest, TwoRowStandardCellUsesFourSingleRowFillers)
{
  constexpr int rows = 5;
  constexpr int columns = 20;
  constexpr int rowHeight = 8;
  constexpr int offsetX = 17;
  constexpr int offsetY = 20;
  fake_udm::DesignDb db;
  db.coreSite.name_ = "core";
  db.coreSite.width_ = eUTL::UvDist(1);
  db.coreSite.height_ = eUTL::UvDist(rowHeight);
  db.tech().addLayer("VTL_N", true, 0, 1, 1);
  db.tech().addLayer("VTL_P", true, 1, 1, 1);
  auto& filler = db.addMaster("FILL1", 0, 1, rowHeight, true);
  filler.symmetry_ = 3;
  fake_udm::DesignDb::addShape(filler, 0, 0, rowHeight / 2);
  fake_udm::DesignDb::addShape(filler, 1, rowHeight / 2, rowHeight);
  auto& standard = db.addMaster("STD2H", 1, 2, 2 * rowHeight, false);
  standard.symmetry_ = 3;
  fake_udm::DesignDb::addShape(standard, 1, 0, rowHeight / 2);
  fake_udm::DesignDb::addShape(standard, 0, rowHeight / 2, rowHeight);
  fake_udm::DesignDb::addShape(standard, 0, rowHeight, 3 * rowHeight / 2);
  fake_udm::DesignDb::addShape(standard, 1, 3 * rowHeight / 2, 2 * rowHeight);
  const int occupiedColumns = columns * GetParam() / 100;
  for (int row = 0; row < rows; ++row) {
    const eUTL::PhysOrientation orientation
        = row % 2 == 0 ? eUTL::PhysOrientationE::MX : eUTL::PhysOrientationE::R0;
    auto& physicalRow = db.desMgr().addRow(offsetX, offsetY + row * rowHeight,
                                           1, rowHeight, columns);
    physicalRow.site_.name_ = db.coreSite.name_;
    physicalRow.orient_ = orientation;
    for (int col = 0; col < occupiedColumns; ++col) {
      if ((row == 2 || row == 3) && (col == 4 || col == 5)) {
        continue;
      }
      db.desMgr().addCell(eUNL::LeafCellID(0, 100 + row * columns + col),
          &filler, offsetX + col, offsetY + row * rowHeight, orientation);
    }
    EXPECT_GE(occupiedColumns - ((row == 2 || row == 3) ? 1 : 0), 6);
  }
  const eUNL::LeafCellID oldId(0, 900);
  db.desMgr().addCell(oldId, &standard, offsetX + 4, offsetY + 2 * rowHeight,
                        eUTL::PhysOrientationE::MX, eUNL::PhysObjStatus::PLACED, "TARGET_2H");
  EXPECT_EQ(100 * (static_cast<int>(db.desMgr().cells_.size()) - 1 + 4),
            GetParam() * rows * columns);
  db.activate();
  DePlace dp;
  dp.getFillerSetting()->addFillerCell("FILL1");
  dp.getNetwork()->updateFillerClassification(*dp.getFillerSetting());
  ipl::ImplantLayerChecker checker(dp.getGrid(), &db.design, dp.getNetwork());
  const Node* old = dp.getNetwork()->getNode(oldId);
  ASSERT_NE(old, nullptr);
  ASSERT_EQ(old->getHeight().v, 2 * rowHeight);
  for (const auto& [id, master] : dp.getNetwork()->getMasters()) {
    (void) id;
    if (master->isFiller()) {
      EXPECT_EQ(master->getPhysLibCell()->getHeight().getStorage(), rowHeight);
    }
  }
  const auto snapshot = [&] {
    std::ostringstream state;
    for (const auto& [id, cell] : db.desMgr().cells_) {
      state << id.getIndexValue() << ',' << cell.valid << ',' << cell.master << ','
            << cell.name << ',' << cell.origin.getX().getStorage() << ','
            << cell.origin.getY().getStorage() << ',' << static_cast<int>(cell.orient.getValue())
            << ',' << static_cast<int>(cell.status) << ';';
    }
    for (const auto& [id, master] : dp.getNetwork()->getMasters()) {
      state << id << ',' << master.get() << ',' << master->getDbMaster().getIndexValue()
            << ',' << master->isFiller() << ';';
    }
    for (const auto& [id, node] : dp.getNetwork()->getNodes()) {
      state << id << ',' << node.get() << ',' << node->getMaster() << ','
            << node->getDbInst().getIndexValue() << ',' << node->getLeft().v << ','
            << node->getBottom().v << ',' << node->getWidth().v << ',' << node->getHeight().v
            << ',' << static_cast<int>(node->getOrient().getValue()) << ','
            << static_cast<int>(node->getType()) << ',' << node->isPlaced() << ';';
    }
    for (GridY row{0}; row < dp.getGrid()->getRowCount(); ++row) {
      for (GridX col{0}; col < dp.getGrid()->getRowSiteCount(); ++col) {
        const Pixel* pixel = dp.getGrid()->gridPixel(col, row);
        state << pixel->cell << ',' << pixel->padding_reserved_by << ',' << pixel->util
              << ',' << pixel->is_valid << ',' << pixel->group << ',' << pixel->is_hopeless
              << ',' << static_cast<unsigned>(pixel->blocked_layers) << ';';
      }
    }
    return state.str();
  };
  const auto before = snapshot();
  const fillerRepair::FillerRepairEngine engine(checker);
  ASSERT_TRUE(engine.isReady());
  const auto oldMaster = old->getMaster()->getDbMaster();
  const ipl::CheckRequest request{nullptr, GridX{4}, GridY{2}, old->getOrient(),
      {{OpType::Delete, oldId, eUTL::UvDist(old->getLeft().v), eUTL::UvDist(old->getBottom().v),
        oldMaster, oldMaster, old->getOrient()}}};
  const auto outcome = engine.repair(request);
  ASSERT_TRUE(outcome.hasSolution);
  ASSERT_EQ(outcome.changes.size(), 4u);
  std::set<std::pair<int, int>> released{{2, 4}, {2, 5}, {3, 4}, {3, 5}};
  std::set<int> orientations;
  for (const auto& change : outcome.changes) {
    ASSERT_EQ(change.op_, OpType::Add);
    EXPECT_EQ(change.new_lib_cell_, filler.getLibCellId());
    const int x = change.x_.getStorage();
    const int row = change.y_.getStorage() / rowHeight;
    EXPECT_EQ(change.y_.getStorage() % rowHeight, 0);
    EXPECT_EQ(released.erase({row, x}), 1u);
    const auto orientation = dp.getGrid()->getSiteOrientation(GridX{x}, GridY{row},
                                                               db.coreSite.getName());
    ASSERT_TRUE(orientation.has_value());
    EXPECT_EQ(change.orientation_, *orientation);
    EXPECT_EQ(change.orientation_, row == 2 ? eUTL::PhysOrientationE::MX
                                           : eUTL::PhysOrientationE::R0);
    orientations.insert(static_cast<int>(change.orientation_.getValue()));
  }
  EXPECT_TRUE(released.empty());
  EXPECT_EQ(orientations.size(), 2u);
  const auto checked = checker.checkPlaceWithOverlays(request,
      eUTL::Rect(eUTL::UvDist(0), eUTL::UvDist(0), eUTL::UvDist(columns),
                  eUTL::UvDist(rows * rowHeight - 1)), {outcome.changes});
  ASSERT_EQ(checked.size(), 1u);
  EXPECT_TRUE(checked.front().isLegal);
  EXPECT_EQ(snapshot(), before);
  EXPECT_EQ(dp.getNetwork()->getNode(oldId), old);
}

INSTANTIATE_TEST_SUITE_P(PlacementUtilizations, SingleHeightFillerRemovalTest,
    ::testing::Values(50, 75, 90), [](const ::testing::TestParamInfo<int>& info) {
      return "Utilization" + std::to_string(info.param);
    });

TEST(DePlaceSitePatterns, ResizeRequiresTheDeclaredUpperSiteAndOrientation)
{
  fake_udm::DesignDb db;
  db.coreSite.width_ = UvDist(1);
  db.coreSite.height_ = UvDist(8);
  eLIB::TechSite upperSite = db.coreSite;
  upperSite.name_ = "upper";
  const auto addMaster = [&](const std::string& name, int id, int width,
                             int height, bool filler) -> eLIB::PhysLibCell& {
    auto& master = db.addMaster(name, id, width, height, filler);
    master.symmetry_ = 3;
    return master;
  };
  const auto& standard = addMaster("STD", 0, 2, 8, false);
  addMaster("FILL_LOWER", 1, 1, 8, true);
  auto& filler = addMaster("FILL_UPPER", 2, 1, 8, true);
  filler.site_ = &upperSite;
  const auto& implicit = addMaster("STD_IMPLICIT", 3, 2, 16, false);
  auto& explicitMaster = addMaster("STD_PATTERN", 4, 2, 16, false);
  explicitMaster.site_patterns_ = {{&db.coreSite, PhysOrientationE::MX},
                                   {&upperSite, PhysOrientationE::R0}};
  auto& wrongDirection = addMaster("STD_BAD_DIRECTION", 5, 2, 16, false);
  wrongDirection.site_patterns_ = {{&db.coreSite, PhysOrientationE::MX},
                                    {&upperSite, PhysOrientationE::MX}};
  for (int row = 0; row < 5; ++row) {
    auto& physical = db.desMgr().addRow(0, row * 8, 1, 8, 20);
    physical.site_ = row == 3 ? upperSite : db.coreSite;
    physical.orient_ = row % 2 == 0 ? PhysOrientationE::MX : PhysOrientationE::R0;
  }
  const LeafCellID oldId(0, 100);
  const LeafCellID firstFillerId(0, 101);
  const LeafCellID secondFillerId(0, 102);
  db.desMgr().addCell(oldId, &standard, 4, 16, PhysOrientationE::MX);
  db.desMgr().addCell(firstFillerId, &filler, 4, 24, PhysOrientationE::R0);
  db.desMgr().addCell(secondFillerId, &filler, 5, 24, PhysOrientationE::R0);
  db.activate();
  DePlace dp;
  dp.getFillerSetting()->addFillerCell("FILL_LOWER FILL_UPPER");
  dp.getNetwork()->updateFillerClassification(*dp.getFillerSetting());
  dp.getPlacementDRC()->addChecker(DRCCheckerType::ImplantLayer,
      std::make_unique<ipl::ImplantLayerChecker>(dp.getGrid(), &db.design, dp.getNetwork()));
  const Node* const original = dp.getNetwork()->getNode(oldId);
  ASSERT_NE(original, nullptr);
  std::vector<CellChangeRecord> changes;
  for (const auto* master : {&implicit, static_cast<const eLIB::PhysLibCell*>(&wrongDirection)}) {
    SCOPED_TRACE(master->getLibCell().getName());
    EXPECT_EQ(dp.findLeg(original, nullptr, 0, master->getLibCellId(), changes),
              std::make_pair(-1, -1));
    EXPECT_TRUE(changes.empty());
    const std::vector<CellChangeRecord> forced{
        {OpType::Replace, oldId, UvDist(4), UvDist(16), standard.getLibCellId(),
         master->getLibCellId(), PhysOrientationE::MX},
        {OpType::Delete, firstFillerId, UvDist(4), UvDist(24), filler.getLibCellId(),
         filler.getLibCellId(), PhysOrientationE::R0},
        {OpType::Delete, secondFillerId, UvDist(5), UvDist(24), filler.getLibCellId(),
         filler.getLibCellId(), PhysOrientationE::R0}};
    EXPECT_FALSE(dp.commit(forced));
    ASSERT_EQ(dp.getNetwork()->getNode(oldId), original);
    EXPECT_EQ(original->getHeight().v, 8);
    EXPECT_TRUE(db.desMgr().getPhysCell(firstFillerId).isValid());
    EXPECT_TRUE(db.desMgr().getPhysCell(secondFillerId).isValid());
  }
  ASSERT_EQ(dp.findLeg(original, nullptr, 0, explicitMaster.getLibCellId(), changes),
            std::make_pair(4, 16));
  ASSERT_EQ(changes.size(), 3u);
  ASSERT_TRUE(dp.commit(changes));
  ASSERT_EQ(dp.getNetwork()->getNode(oldId), original);
  EXPECT_EQ(original->getHeight().v, 16);
  EXPECT_EQ(original->getOrient(), PhysOrientationE::R0);
  EXPECT_EQ(dp.getGrid()->gridPixel(GridX{5}, GridY{3})->cell, original);
  EXPECT_FALSE(db.desMgr().getPhysCell(firstFillerId).isValid());
  EXPECT_FALSE(db.desMgr().getPhysCell(secondFillerId).isValid());
  EXPECT_EQ(db.desMgr().getPhysCell(oldId).getPhysMaster().getLibCellId(), standard.getLibCellId());
  EXPECT_EQ(db.desMgr().getPhysCell(oldId).getOrient(), PhysOrientationE::MX);
}

TEST(DePlaceSitePatterns, AmbiguousPatternUsesAnOrientationAllowedByMasterSymmetry)
{
  fake_udm::DesignDb db;
  db.coreSite.width_ = UvDist(1);
  db.coreSite.height_ = UvDist(8);
  auto& standard = db.addMaster("STD", 0, 2, 8, false);
  auto& filler = db.addMaster("FILL", 1, 1, 8, true);
  auto& tall = db.addMaster("STD_PATTERN", 2, 2, 16, false);
  tall.symmetry_ = 2;  // Allows MY but not MX.
  tall.site_patterns_ = {{&db.coreSite, PhysOrientationE::R0},
                         {&db.coreSite, PhysOrientationE::R180}};
  for (int row = 0; row < 5; ++row) {
    auto& physical = db.desMgr().addRow(0, row * 8, 1, 8, 20);
    physical.site_ = db.coreSite;
    physical.orient_ = row == 2 ? PhysOrientationE::MY : PhysOrientationE::MX;
  }
  const LeafCellID oldId(0, 100);
  db.desMgr().addCell(oldId, &standard, 4, 16, PhysOrientationE::MY);
  db.desMgr().addCell(LeafCellID(0, 101), &filler, 4, 24, PhysOrientationE::MX);
  db.desMgr().addCell(LeafCellID(0, 102), &filler, 5, 24, PhysOrientationE::MX);
  db.activate();
  DePlace dp;
  dp.getFillerSetting()->addFillerCell("FILL");
  dp.getNetwork()->updateFillerClassification(*dp.getFillerSetting());
  dp.getPlacementDRC()->addChecker(DRCCheckerType::ImplantLayer,
      std::make_unique<ipl::ImplantLayerChecker>(dp.getGrid(), &db.design, dp.getNetwork()));
  const auto geometric = dp.getGrid()->getPlacementSiteOrientation(GridX{4}, GridY{2}, tall);
  ASSERT_EQ(geometric, PhysOrientationE::MX);
  std::vector<CellChangeRecord> changes;
  ASSERT_TRUE(dp.isLegal(oldId, tall.getLibCellId(), changes));
  ASSERT_EQ(changes.size(), 3u);
  EXPECT_EQ(changes.back().orientation_, PhysOrientationE::MY);
  ASSERT_TRUE(dp.commit(changes));
  EXPECT_EQ(dp.getNetwork()->getNode(oldId)->getOrient(), PhysOrientationE::MY);
  EXPECT_EQ(db.desMgr().getPhysCell(oldId).getPhysMaster().getLibCellId(), standard.getLibCellId());
}

TEST(DePlaceSitePatterns, TallParentSiteDoesNotRequireTheSameNameOnTheUpperGridRow)
{
  fake_udm::DesignDb db;
  db.coreSite.width_ = UvDist(1);
  db.coreSite.height_ = UvDist(8);
  eLIB::TechSite parent = db.coreSite;
  parent.name_ = "parent";
  parent.height_ = UvDist(16);
  auto& master = db.addMaster("STD_PARENT", 0, 2, 16, false);
  master.site_ = &parent;
  for (int row = 0; row < 5; ++row) {
    db.desMgr().addRow(0, row * 8, 1, 8, 10);
  }
  auto& parentRow = db.desMgr().addRow(10, 16, 1, 16, 2);
  parentRow.site_ = parent;
  db.activate();
  DePlace dp;
  ASSERT_FALSE(dp.getGrid()->getSiteOrientation(GridX{10}, GridY{3}, parent.getName()));
  ASSERT_TRUE(dp.getGrid()->getPlacementSiteOrientation(GridX{10}, GridY{2}, master));
  // The upstream owner creates the UDM std instance; commit only imports its
  // stable identity and updates Network/Grid, without changing UDM placement.
  const LeafCellID addedId(0, 900);
  db.desMgr().addCell(addedId, &master, 0, 0, PhysOrientationE::R0,
                     eUNL::PhysObjStatus::UNKNOWN, "TALL");
  ASSERT_EQ(dp.getNetwork()->getNode(addedId), nullptr);
  const size_t physicalCount = db.desMgr().cells_.size();
  ASSERT_TRUE(dp.commit({{OpType::Add, std::string("TALL"), UvDist(10), UvDist(16),
                          LibCellID(), master.getLibCellId(), PhysOrientationE::R0}}));
  const Node* const added = dp.getGrid()->gridPixel(GridX{10}, GridY{2})->cell;
  ASSERT_NE(added, nullptr);
  EXPECT_EQ(added->getDbInst(), addedId);
  EXPECT_EQ(dp.getGrid()->gridPixel(GridX{11}, GridY{3})->cell, added);
  EXPECT_EQ(db.desMgr().cells_.size(), physicalCount);
  const auto physical = db.desMgr().getPhysCell(addedId);
  EXPECT_EQ(physical.getOrigin().getX(), UvDist(0));
  EXPECT_EQ(physical.getOrigin().getY(), UvDist(0));
  EXPECT_EQ(physical.getStatus(), eUNL::PhysObjStatus::UNKNOWN);
}

}  // namespace
