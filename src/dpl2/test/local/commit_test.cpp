// Local DB mutation tests: real DePlace/command/checker/Grid/Network, fake UDM.
#include <PlacementDRC.h>
#include <dpl2/DePlace.h>
#include <drc/ImplantLayerChecker.h>
#include <fake_udm.h>
#include <gtest/gtest.h>
#include <infrastructure/Grid.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <testFillerRepairCmd.hh>
#include <vector>

#include "E2ETestProvider.h"

namespace {
using namespace dpl2;
namespace frt = dpl2::fillerRepair::test;

struct Fixture
{
  std::unique_ptr<frt::E2ETestProvider> provider = frt::makeE2ETestProvider();
  std::unique_ptr<frt::E2ETestDesign> design;
  std::unique_ptr<DePlace> owned;
  DePlace* dp = nullptr;

  explicit Fixture(bool singleton = false,
                   int ruleWidth = 1,
                   bool doubleHeight = false)
  {
    frt::DesignSetup setup;
    setup.rowOriginX.fill(17);
    setup.implantRuleWidth = ruleWidth;
    setup.doubleHeightRepairLayout = doubleHeight;
    design = provider->createDesign(setup);
    if (singleton) {
      // Three adjacent fillers cover six sites; TX5 will release a one-site
      // gap.
      auto& cells = design->desMgr()->cells_;
      const auto old = cells.at(eUNL::LeafCellID(0, 142));
      cells.erase(eUNL::LeafCellID(0, 142));
      for (int i = 0; i < 3; ++i) {
        design->desMgr()->addCell(
            eUNL::LeafCellID(0, 200 + i),
            &design->design()->getLibAcc().getPhysLibCell(3),
            old.origin.getX().getStorage() + 2 * i,
            old.origin.getY().getStorage(),
            old.orient);
      }
    }
    for (auto& [id, cell] : design->desMgr()->cells_) {
      cell.name = "I" + std::to_string(id.getIndexValue());
      cell.origin = eUTL::Point2D(cell.origin.getX(),
                                  cell.origin.getY() + eUTL::UvDist(20));
    }
    if (singleton) {
      design->desMgr()->cells_.at(eUNL::LeafCellID(0, 100)).name
          = "FR_TARGET_103";
    }
    for (auto& row : design->desMgr()->rows_) {
      row.origin_ = eUTL::Point2D(row.origin_.getX(),
                                  row.origin_.getY() + eUTL::UvDist(20));
      row.bbox_.move(eUTL::UvDist(0), eUTL::UvDist(20));
    }
    if (singleton) {
      dp = DePlace::get();
    } else {
      owned = std::make_unique<DePlace>();
      dp = owned.get();
    }
    dp->getFillerSetting()->addFillerCell("FL2 FH2 FS2 FH1 FL2D FH1D");
    dp->getFillerSetting()->setPrefix("TEST_FILL_");
    dp->getNetwork()->updateFillerClassification(*dp->getFillerSetting());
    // Finalize the fixture's static filler configuration before any repairs.
    dp->getPlacementDRC()->addChecker(
        DRCCheckerType::ImplantLayer,
        std::make_unique<ipl::ImplantLayerChecker>(
            dp->getGrid(), design->design(), dp->getNetwork()));
  }

  Node* node(int id)
  {
    return dp->getNetwork()->getNode(eUNL::LeafCellID(0, id));
  }
  ipl::ImplantLayerChecker* checker()
  {
    return dynamic_cast<ipl::ImplantLayerChecker*>(
        dp->getPlacementDRC()->getChecker(DRCCheckerType::ImplantLayer));
  }
  CellChangeRecord remove(int id)
  {
    const Node* old = node(id);
    return {OpType::Delete,
            old->getDbInst(),
            eUTL::UvDist(old->getLeft().v),
            eUTL::UvDist(old->getBottom().v),
            old->getMaster()->getDbMaster(),
            old->getMaster()->getDbMaster(),
            old->getOrient()};
  }
  CellChangeRecord add(std::string name,
                       int master,
                       int x,
                       int y,
                       eUTL::PhysOrientation orient
                       = eUTL::PhysOrientationE::MX)
  {
    return {OpType::Add,
            std::move(name),
            eUTL::UvDist(x),
            eUTL::UvDist(y),
            eLIB::LibCellID(),
            eLIB::LibCellID(0, master),
            orient};
  }
  std::string snapshot()
  {
    std::ostringstream out;
    for (const auto& [id, cell] : design->desMgr()->cells_) {
      out << id.getIndexValue() << ',' << cell.valid << ',' << cell.name << ','
          << cell.master->getLibCellId().getIndexValue() << ','
          << cell.origin.getX().getStorage() << ','
          << cell.origin.getY().getStorage() << ','
          << static_cast<int>(cell.orient.getValue()) << ','
          << static_cast<int>(cell.status) << ';';
    }
    for (const auto& [id, cell] : dp->getNetwork()->getNodes()) {
      out << id << ',' << cell->getDbInst().getIndexValue() << ','
          << cell->getMaster()->getId() << ',' << cell->getLeft().v << ','
          << cell->getBottom().v << ','
          << static_cast<int>(cell->getOrient().getValue()) << ';';
    }
    for (GridY y{0}; y < dp->getGrid()->getRowCount(); ++y) {
      for (GridX x{0}; x < dp->getGrid()->getRowSiteCount(); ++x) {
        const Pixel* p = dp->getGrid()->gridPixel(x, y);
        out << p->cell << ',' << p->padding_reserved_by << ',' << p->util
            << ';';
      }
    }
    return out.str();
  }
  void consistent()
  {
    std::set<int> dbIds;
    for (const auto& [id, cell] : dp->getNetwork()->getNodes()) {
      ASSERT_EQ(cell->getId(), id);
      ASSERT_EQ(dp->getNetwork()->getNode(cell->getDbInst()), cell.get());
      ASSERT_TRUE(dbIds.insert(cell->getDbInst().getIndexValue()).second);
      const auto physical = design->desMgr()->getPhysCell(cell->getDbInst());
      ASSERT_TRUE(physical.isValid());
      EXPECT_EQ(physical.getPhysMaster().getLibCellId(),
                cell->getMaster()->getDbMaster());
      EXPECT_EQ(physical.getOrient(), cell->getOrient());
      EXPECT_EQ(physical.getStatus(), eUNL::PhysObjStatus::PLACED);
      EXPECT_TRUE(cell->isPlaced());
      EXPECT_EQ(physical.getOrigin().getX().getStorage(),
                cell->getLeft().v + 17);
      EXPECT_EQ(physical.getOrigin().getY().getStorage(),
                cell->getBottom().v + 20);
      EXPECT_EQ(cell->isFiller(), cell->getMaster()->isFiller());
      const auto box = dp->getGrid()->gridCovering(cell.get());
      for (GridY y = box.ylo; y < box.yhi; ++y) {
        for (GridX x = box.xlo; x < box.xhi; ++x) {
          ASSERT_EQ(dp->getGrid()->gridPixel(x, y)->cell, cell.get());
        }
      }
    }
    for (const auto& [id, physical] : design->desMgr()->cells_) {
      EXPECT_EQ(dp->getNetwork()->getNode(id) != nullptr, physical.valid);
    }
    for (GridY y{0}; y < dp->getGrid()->getRowCount(); ++y) {
      for (GridX x{0}; x < dp->getGrid()->getRowSiteCount(); ++x) {
        const auto* pixel = dp->getGrid()->gridPixel(x, y);
        for (const auto* cell : {pixel->cell, pixel->padding_reserved_by}) {
          if (cell != nullptr) {
            EXPECT_EQ(dp->getNetwork()->getNode(cell->getId()), cell);
          }
        }
      }
    }
  }
};

TEST(DePlaceCommit, DefaultConstructorInitializesGridBeforeChecker)
{
  const auto provider = frt::makeE2ETestProvider();
  const auto design = provider->createDesign({});
  DePlace dp;
  const auto* checker = dynamic_cast<ipl::ImplantLayerChecker*>(
      dp.getPlacementDRC()->getChecker(DRCCheckerType::ImplantLayer));
  ASSERT_NE(checker, nullptr);
  EXPECT_TRUE(checker->getDiags().empty());
  ASSERT_FALSE(checker->getMasterItems().empty());
  const Node* target
      = dp.getNetwork()->getNode(design->cell(frt::CellRole::Target));
  ASSERT_NE(target, nullptr);
  const auto master = target->getMaster()->getDbMaster();
  const ipl::CheckRequest request{target,
                                  dp.getGrid()->gridX(target),
                                  dp.getGrid()->gridSnapDownY(target),
                                  target->getOrient(),
                                  {{OpType::Delete,
                                    target->getDbInst(),
                                    eUTL::UvDist(target->getLeft().v),
                                    eUTL::UvDist(target->getBottom().v),
                                    master,
                                    master,
                                    target->getOrient()}}};
  EXPECT_TRUE(checker->checkDirect(request).isLegal);
}

TEST(DePlaceCommit, GapAddsAndSecondRepairUseTheSameLiveChecker)
{
  Fixture f;
  ASSERT_NE(f.checker(), nullptr);
  Node* old = f.node(103);
  ASSERT_NE(old, nullptr);
  auto* checker = f.checker();
  const int oldNetworkId = old->getId();
  Node target = *old;
  target.setMaster(f.dp->getNetwork()->getMaster(eLIB::LibCellID(0, 18)));
  target.setType(Node::CELL);
  target.setWidth(DbuX{1});
  target.setPlaced(false);
  std::vector<CellChangeRecord> overlays{f.remove(103)};
  std::vector<CellChangeRecord> changes;
  ASSERT_TRUE(checker->check(
      &target, GridX{18}, GridY{0}, target.getOrient(), changes, overlays));
  ASSERT_EQ(changes.size(), 1u);
  ASSERT_EQ(changes.front().op_, OpType::Add);
  EXPECT_EQ(
      std::get<std::string>(changes.front().cell_data_).find("TEST_FILL_"), 0u);
  auto transaction = changes;  // order-independent final-occupancy preflight
  transaction.push_back(f.add("NEW_STD", 18, 18, 0));
  transaction.insert(transaction.end(), overlays.begin(), overlays.end());
  ASSERT_TRUE(f.dp->commit(transaction));
  EXPECT_EQ(f.node(103), nullptr);
  EXPECT_EQ(f.dp->getNetwork()->getNode(oldNetworkId), nullptr);
  f.consistent();
  Node* gap = f.dp->getGrid()->gridPixel(GridX{19}, GridY{0})->cell;
  ASSERT_NE(gap, nullptr);
  ASSERT_TRUE(gap->isFiller());
  const auto gapId = gap->getDbInst();
  EXPECT_GT(gapId.getIndexValue(), 143);
  target = *gap;
  target.setMaster(f.dp->getNetwork()->getMaster(eLIB::LibCellID(0, 18)));
  target.setType(Node::CELL);
  target.setPlaced(false);
  overlays = {f.remove(gapId.getIndexValue())};
  changes.clear();
  ASSERT_TRUE(checker->check(
      &target, GridX{19}, GridY{0}, target.getOrient(), changes, overlays));
  EXPECT_EQ(f.checker(), checker);
  EXPECT_TRUE(changes.empty());
  transaction = overlays;
  transaction.push_back(f.add("SECOND_STD", 18, 19, 0));
  ASSERT_TRUE(f.dp->commit(transaction));
  f.consistent();
  const auto before = f.snapshot();
  EXPECT_FALSE(f.dp->commit(transaction));
  EXPECT_EQ(f.snapshot(), before);
}

TEST(DePlaceCommit, TwoRowDeletionCommitsTheEntireLShapedGap)
{
  Fixture f(false, 1, true);
  // Prefer the largest legal footprint so the gap contains a new two-row
  // filler. The next request must run FR again to split that new instance.
  f.dp->getFillerSetting()->setFollowOrder(false);
  ASSERT_NE(f.node(121), nullptr);
  Node target = *f.node(121);
  target.setMaster(f.dp->getNetwork()->getMaster(eLIB::LibCellID(0, 18)));
  target.setType(Node::CELL);
  target.setWidth(DbuX{1});
  target.setHeight(DbuY{frt::kRowHeight});
  target.setPlaced(false);
  std::vector<CellChangeRecord> overlays{f.remove(121)};
  std::vector<CellChangeRecord> changes;
  ASSERT_TRUE(f.checker()->check(
      &target, GridX{6}, GridY{2}, target.getOrient(), changes, overlays));
  ASSERT_FALSE(changes.empty());
  auto transaction = changes;
  transaction.push_back(f.add("NEW_STD", 18, 6, 2 * frt::kRowHeight));
  transaction.insert(transaction.end(), overlays.begin(), overlays.end());
  ASSERT_TRUE(f.dp->commit(transaction));
  EXPECT_EQ(f.node(121), nullptr);
  int fillerSites = 0;
  for (int row : {2, 3}) {
    for (int col : {6, 7}) {
      const auto* cell
          = f.dp->getGrid()->gridPixel(GridX{col}, GridY{row})->cell;
      ASSERT_NE(cell, nullptr);
      fillerSites += cell->isFiller();
    }
  }
  EXPECT_EQ(fillerSites, 3);
  f.consistent();
  Node* added = f.dp->getGrid()->gridPixel(GridX{7}, GridY{2})->cell;
  ASSERT_NE(added, nullptr);
  ASSERT_EQ(added->getHeight().v, 2 * frt::kRowHeight);
  const auto addedId = added->getDbInst();
  target = *added;
  target.setMaster(f.dp->getNetwork()->getMaster(eLIB::LibCellID(0, 18)));
  target.setType(Node::CELL);
  target.setHeight(DbuY{frt::kRowHeight});
  target.setPlaced(false);
  const auto orientation = f.dp->getGrid()->getSiteOrientation(
      GridX{7},
      GridY{3},
      target.getMaster()->getPhysLibCell()->getTechSite()->getName());
  ASSERT_TRUE(orientation.has_value());
  auto* checker = f.checker();
  overlays = {f.remove(addedId.getIndexValue())};
  changes.clear();
  ASSERT_TRUE(checker->check(
      &target, GridX{7}, GridY{3}, *orientation, changes, overlays));
  ASSERT_FALSE(changes.empty());
  EXPECT_EQ(f.checker(), checker);
  transaction = overlays;
  transaction.push_back(
      f.add("NEXT_STD", 18, 7, 3 * frt::kRowHeight, *orientation));
  transaction.insert(transaction.end(), changes.begin(), changes.end());
  ASSERT_TRUE(f.dp->commit(transaction));
  EXPECT_EQ(f.dp->getNetwork()->getNode(addedId), nullptr);
  f.consistent();
}

TEST(DePlaceCommit, InvalidBatchesLeaveDatabaseNetworkAndGridUntouched)
{
  Fixture f;
  const auto before = f.snapshot();
  const auto good = f.add("NEW", 3, 18, 0);
  std::vector<std::vector<CellChangeRecord>> batches{
      {f.remove(103), good, f.add("I122", 3, 18, 2)},
      {f.remove(103), good, good},
      {f.remove(103), good, f.add("SAME_SITE", 3, 18, 0)},
      {f.remove(103), f.remove(103), good},
      {f.remove(103), f.add("BAD_MASTER", 9999, 18, 0)},
      {f.remove(103), good, f.add("OVERLAP", 3, 17, 0)},
      {f.remove(103), f.add("BAD_BOUND", 3, 20, 0)},
      {f.remove(103), f.add("HUGE", 3, std::numeric_limits<int>::max(), 0)},
      {f.remove(103), f.add("BAD_ROW", 3, 18, 1)}};
  auto stale = f.remove(122);
  stale.op_ = OpType::Replace;
  stale.orig_lib_cell_ = eLIB::LibCellID(0, 3);
  batches.push_back({f.remove(103), stale});
  auto unknown = f.remove(122);
  unknown.op_ = static_cast<OpType>(99);
  batches.push_back({f.remove(103), unknown});
  for (size_t i = 0; i < batches.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_FALSE(f.dp->commit(batches[i]));
    EXPECT_EQ(f.snapshot(), before);
  }
}

TEST(DePlaceCommit, SwapMovesAndReorientsWithoutChangingIdentity)
{
  Fixture f;
  Node* original = f.node(103);
  auto swap = f.remove(103);
  swap.op_ = OpType::Replace;
  swap.new_lib_cell_ = eLIB::LibCellID(0, 4);
  swap.y_ = eUTL::UvDist(f.node(113)->getBottom().v);
  swap.orientation_ = f.node(113)->getOrient();
  ASSERT_TRUE(f.dp->commit({swap, f.remove(113)}));
  EXPECT_EQ(f.node(103), original);
  EXPECT_EQ(f.design->design()
                ->getHierMgr()
                ->getLeafCell(original->getDbInst())
                .getName(),
            "I103");
  EXPECT_EQ(f.dp->getGrid()->gridPixel(GridX{18}, GridY{0})->cell, nullptr);
  f.consistent();
}

TEST(DePlaceCommit, DeleteClearsPaddingBeforeDestroyingNode)
{
  Fixture f;
  Node* original = f.node(102);
  f.dp->setPadding(original->getDbInst(), 0, 1);
  f.dp->getGrid()->paintCellPadding(original);
  ASSERT_EQ(
      f.dp->getGrid()->gridPixel(GridX{18}, GridY{0})->padding_reserved_by,
      original);
  ASSERT_TRUE(f.dp->commit({f.remove(102)}));
  EXPECT_EQ(
      f.dp->getGrid()->gridPixel(GridX{18}, GridY{0})->padding_reserved_by,
      nullptr);
  EXPECT_EQ(f.dp->getGrid()->gridPixel(GridX{12}, GridY{0})->cell, nullptr);
  f.consistent();
}

TEST(DePlaceCommit, ExplicitNetworkIdsAdvanceTheAllocator)
{
  Fixture f;
  auto extra = std::make_unique<Node>();
  extra->setId(10000);
  f.dp->getNetwork()->addNode(std::move(extra));
  auto automatic = std::make_unique<Node>();
  automatic->setId(-1);
  f.dp->getNetwork()->addNode(std::move(automatic));
  ASSERT_NE(f.dp->getNetwork()->getNode(10001), nullptr);
  EXPECT_EQ(f.dp->getNetwork()->getNode(10001)->getId(), 10001);
  ASSERT_TRUE(f.dp->commit({f.remove(103), f.add("NEW", 3, 18, 0)}));
  const Node* added = f.dp->getGrid()->gridPixel(GridX{18}, GridY{0})->cell;
  ASSERT_NE(added, nullptr);
  EXPECT_GT(added->getId(), 10000);
  EXPECT_EQ(f.dp->getNetwork()->getNode(added->getDbInst()), added);
}

TEST(TestFillerRepairCommand, CommitsTargetOverlaysAndFillersThenRunsAgain)
{
  // DePlace::get has process lifetime; keep its backing design alive as long.
  static Fixture f(true);
  TestFillerRepairCmd command;
  std::string error;
  const auto originalId = f.node(122)->getDbInst();
  ASSERT_TRUE(command.run({"-inst", "I122", "-master", "TH4"}, error));
  ASSERT_NE(f.node(122), nullptr);
  EXPECT_EQ(f.node(122)->getDbInst(), originalId);
  EXPECT_EQ(f.node(122)->getMaster()->getDbMaster(), eLIB::LibCellID(0, 2));
  f.consistent();

  ASSERT_TRUE(command.run({"-inst", "I103", "-master", "BUF1"}, error));
  EXPECT_EQ(f.node(103), nullptr);
  const Node* inserted = f.dp->getGrid()->gridPixel(GridX{18}, GridY{0})->cell;
  ASSERT_NE(inserted, nullptr);
  EXPECT_EQ(f.design->design()
                ->getHierMgr()
                ->getLeafCell(inserted->getDbInst())
                .getName(),
            "FR_TARGET_103_1");
  Node* gap = f.dp->getGrid()->gridPixel(GridX{19}, GridY{0})->cell;
  ASSERT_NE(gap, nullptr);
  ASSERT_TRUE(gap->isFiller());
  const std::string name = f.design->design()
                               ->getHierMgr()
                               ->getLeafCell(gap->getDbInst())
                               .getName();
  ASSERT_TRUE(command.run({"-inst", name, "-master", "BUF1"}, error));
  f.consistent();
  ASSERT_TRUE(
      command.run({"-inst", "I200 I201 I202", "-master", "TX5"}, error));
  for (int id : {200, 201, 202}) {
    EXPECT_EQ(f.node(id), nullptr);
  }
  ASSERT_TRUE(
      f.dp->getGrid()->gridPixel(GridX{17}, GridY{4})->cell->isFiller());
  f.consistent();
  const auto before = f.snapshot();
  EXPECT_FALSE(command.run({"-inst", "I103", "-master", "BUF1"}, error));
  EXPECT_EQ(f.snapshot(), before);
  EXPECT_FALSE(command.run({"-inst", "I122", "-master", "TH5"}, error));
  EXPECT_EQ(f.snapshot(), before);
}

TEST(DePlaceCommit, StdReplacementCommitsTheReturnedFillerSwap)
{
  Fixture f(false, 6);
  Node target = *f.node(122);
  target.setMaster(f.dp->getNetwork()->getMaster(eLIB::LibCellID(0, 2)));
  target.setPlaced(false);
  std::vector<CellChangeRecord> overlays{f.remove(122)};
  std::vector<CellChangeRecord> changes;
  ASSERT_TRUE(f.checker()->check(
      &target, GridX{8}, GridY{2}, target.getOrient(), changes, overlays));
  ASSERT_EQ(changes.size(), 1u);
  ASSERT_EQ(changes.front().op_, OpType::Replace);
  const auto changedId = std::get<eUNL::LeafCellID>(changes.front().cell_data_);
  const auto newMaster = changes.front().new_lib_cell_;
  auto replacement = overlays.front();
  replacement.op_ = OpType::Replace;
  replacement.new_lib_cell_ = target.getMaster()->getDbMaster();
  changes.push_back(replacement);
  ASSERT_TRUE(f.dp->commit(changes));
  ASSERT_NE(f.dp->getNetwork()->getNode(changedId), nullptr);
  EXPECT_EQ(f.dp->getNetwork()->getNode(changedId)->getMaster()->getDbMaster(),
            newMaster);
  EXPECT_EQ(f.node(122)->getMaster()->getDbMaster(), eLIB::LibCellID(0, 2));
  f.consistent();
}

TEST(DePlaceCommit, DatabaseOnlyNameCollisionsAreRejectedBeforeDeletion)
{
  Fixture f;
  f.design->desMgr()->addCell(
      eUNL::LeafCellID(0, 900),
      &f.design->design()->getLibAcc().getPhysLibCell(3),
      0,
      0,
      eUTL::PhysOrientationE::R0,
      eUNL::PhysObjStatus::UNKNOWN,
      "UNPLACED");
  const auto before = f.snapshot();
  EXPECT_FALSE(f.dp->commit({f.remove(103), f.add("UNPLACED", 3, 18, 0)}));
  EXPECT_EQ(f.snapshot(), before);
}

TEST(DePlaceCommit, IdExhaustionIsPreflightedForTheEntireBatch)
{
  Fixture f;
  auto& tombstone = f.design->desMgr()->addCell(
      eUNL::LeafCellID(0, std::numeric_limits<int>::max() - 1),
      &f.design->design()->getLibAcc().getPhysLibCell(3),
      0,
      0);
  tombstone.valid = false;
  const auto before = f.snapshot();
  EXPECT_FALSE(f.dp->commit(
      {f.remove(103), f.add("A", 10, 18, 0), f.add("B", 10, 19, 0)}));
  EXPECT_EQ(f.snapshot(), before);
}

TEST(DePlaceCommit, BlockedOrReservedFinalSitesAreNotOverwritten)
{
  Fixture f;
  Pixel* pixel = f.dp->getGrid()->gridPixel(GridX{19}, GridY{0});
  pixel->is_valid = false;
  auto before = f.snapshot();
  EXPECT_FALSE(f.dp->commit({f.remove(103), f.add("A", 3, 18, 0)}));
  EXPECT_EQ(f.snapshot(), before);
  pixel->is_valid = true;
  pixel->padding_reserved_by = f.node(102);
  before = f.snapshot();
  EXPECT_FALSE(f.dp->commit({f.remove(103), f.add("A", 3, 18, 0)}));
  EXPECT_EQ(f.snapshot(), before);
}
}  // namespace
