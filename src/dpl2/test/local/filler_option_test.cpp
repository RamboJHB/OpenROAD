// Filler policy integration: real engine/checker/DePlace over a local UDM DB.
#include <PlacementDRC.h>
#include <dpl2/DePlace.h>
#include <drc/ImplantLayerChecker.h>
#include <fake_udm.h>
#include <fillerRepair/FillerRepairEngine.h>
#include <gtest/gtest.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Padding.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <map>
#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dpl2;

struct FillerRect
{
  int x;
  int row;
  int width;
  int height = 1;
};

std::string fillerName(int width, int height)
{
  return "F" + std::to_string(width) + (height == 2 ? "D" : "");
}

struct OptionFixture
{
  static constexpr int kRows = 12;
  static constexpr int kCols = 40;
  static constexpr int kHeight = 8;
  fake_udm::DesignDb db;
  std::unique_ptr<DePlace> dp;
  std::shared_ptr<Padding> padding = std::make_shared<Padding>();
  std::vector<eUNL::LeafCellID> fillerIds;
  std::unique_ptr<ipl::ImplantLayerChecker> checker;
  std::unique_ptr<fillerRepair::FillerRepairEngine> engine;
  int siteWidth;

  OptionFixture(const std::string& pattern, const std::vector<FillerRect>& fillers, std::string candidates = "F2 F1",
                bool followOrder = true, int widthDbu = 5)
      : siteWidth(widthDbu)
  {
    db.coreSite.name_ = "CORE";
    db.coreSite.width_ = eUTL::UvDist(siteWidth);
    db.coreSite.height_ = eUTL::UvDist(kHeight);
    db.tech().addLayer("VTL_N", true, 0, siteWidth, siteWidth);
    db.tech().addLayer("VTL_P", true, 1, siteWidth, siteWidth);
    const auto addMaster = [&](const std::string& name, int id, int width, int height, bool filler) {
      auto& master = db.addMaster(name, id, width * siteWidth, height * kHeight, filler);
      for (int row = 0; row < height; ++row) {
        const int bottomLayer = (row + height - 1) % 2;
        fake_udm::DesignDb::addShape(master, bottomLayer, row * kHeight, row * kHeight + kHeight / 2);
        fake_udm::DesignDb::addShape(master, 1 - bottomLayer, row * kHeight + kHeight / 2, (row + 1) * kHeight);
      }
    };
    for (int width : {1, 2, 3, 4, 6}) {
      for (int height : {1, 2}) {
        addMaster(fillerName(width, height), 100 + width * 10 + height, width, height, true);
      }
    }
    for (int width : {1, 2, 3}) {
      addMaster("S" + std::to_string(width), 1000 + width, width, 1, false);
      addMaster("S" + std::to_string(width) + "D", 1100 + width, width, 2, false);
    }
    for (int row = 0; row < kRows; ++row) {
      auto& physicalRow = db.desMgr().addRow(0, row * kHeight, siteWidth, kHeight, kCols * siteWidth);
      physicalRow.site_.name_ = "CORE";
      physicalRow.orient_ = orientation(row);
    }
    int id = 100;
    std::set<std::pair<int, int>> occupied;
    for (const auto& filler : fillers) {
      const std::string name = fillerName(filler.width, filler.height);
      candidates += " " + name;  // deleted/unchanged filler masters are configured too
      const eUNL::LeafCellID cellId(0, id++);
      fillerIds.push_back(cellId);
      db.desMgr().addCell(
          cellId, &master(name), filler.x * siteWidth, filler.row * kHeight, orientation(filler.row),
          eUNL::PhysObjStatus::PLACED, "OLD_" + std::to_string(id));
      for (int row = filler.row; row < filler.row + filler.height; ++row) {
        for (int col = filler.x; col < filler.x + filler.width; ++col) {
          if (!occupied.emplace(row, col).second) {
            throw std::logic_error("overlapping fixture fillers");
          }
        }
      }
    }
    for (int row = 0; row < kRows; ++row) {
      for (int col = 0; col < kCols; ++col) {
        if (occupied.count({row, col}) == 0) {
          const eUNL::LeafCellID cellId(0, id++);
          db.desMgr().addCell(
              cellId, &master("S1"), col * siteWidth, row * kHeight, orientation(row), eUNL::PhysObjStatus::PLACED,
              "STD_" + std::to_string(id));
        }
      }
    }
    db.activate();
    dp = std::make_unique<DePlace>(&db.desMgr());
    auto* setting = dp->getFillerSetting();
    setting->addFillerCell(candidates);
    setting->addAvoidPattern(pattern);
    setting->setFollowOrder(followOrder);
    const eUTL::Rect core(
        eUTL::UvDist(0), eUTL::UvDist(0), eUTL::UvDist(kCols * siteWidth), eUTL::UvDist(kRows * kHeight));
    padding->setDesginManager(&db.desMgr());
    dp->getGrid()->setCore(core);
    dp->getGrid()->examineRows(&db.desMgr());
    dp->getGrid()->initGrid(&db.desMgr(), padding, 100, 100);
    dp->getNetwork()->setCore(core);
    const EdgeTypeTable edges;
    for (const auto& lib : db.design.getLibAcc().getLibCellIter(true, false)) {
      dp->getNetwork()->addMaster(db.design.getLibAcc().getPhysLibCell(lib.getId()), *setting, dp->getGrid(), &edges);
    }
    for (const auto& [cellId, data] : db.desMgr().cells_) {
      (void) data;
      dp->getNetwork()->addNode(cellId, &db.desMgr());
      dp->getGrid()->paintPixel(dp->getNetwork()->getNode(cellId));
    }
    for (int row = 0; row < kRows; ++row) {
      std::set<const Node*> cells;
      for (int col = 0; col < kCols; ++col) {
        cells.insert(dp->getGrid()->gridPixel(GridX{col}, GridY{row})->cell);
      }
      cells.erase(nullptr);
      EXPECT_GE(cells.size(), 10u) << "row=" << row;
    }
    for (int col = 0; col < kCols; ++col) {
      std::set<const Node*> cells;
      for (int row = 0; row < kRows; ++row) {
        cells.insert(dp->getGrid()->gridPixel(GridX{col}, GridY{row})->cell);
      }
      cells.erase(nullptr);
      EXPECT_GE(cells.size(), 10u) << "column=" << col;
    }
    checker = std::make_unique<ipl::ImplantLayerChecker>(dp->getGrid(), &db.design, dp->getNetwork());
    engine = std::make_unique<fillerRepair::FillerRepairEngine>(*checker);
  }

  static eUTL::PhysOrientation orientation(int row)
  {
    return row % 2 == 0 ? eUTL::PhysOrientationE::MX : eUTL::PhysOrientationE::R0;
  }
  const eLIB::PhysLibCell& master(const std::string& name)
  {
    const auto& library = db.design.getLibAcc();
    return library.getPhysLibCell(library.getLibCell(library.findModule(name))->getId());
  }
  std::string snapshot()
  {
    std::ostringstream out;
    for (const auto& [id, data] : db.desMgr().cells_) {
      const Node* node = dp->getNetwork()->getNode(id);
      out << id.getIndexValue() << ',' << data.valid << ',' << data.name << ','
          << data.master->getLibCellId().getIndexValue() << ',' << data.origin.getX().getStorage() << ','
          << data.origin.getY().getStorage() << ',' << static_cast<int>(data.orient.getValue()) << ',' << node << ';';
    }
    for (GridY row{0}; row < dp->getGrid()->getRowCount(); ++row) {
      for (GridX col{0}; col < dp->getGrid()->getRowSiteCount(); ++col) {
        const Pixel* pixel = dp->getGrid()->gridPixel(col, row);
        out << pixel->cell << ',' << pixel->padding_reserved_by << ';';
      }
    }
    for (const auto& [id, node] : dp->getNetwork()->getNodes()) {
      out << id << ',' << node->getMaster()->getId() << ',' << node->getLeft().v << ',' << node->getBottom().v << ','
          << node->getWidth().v << ',' << node->getHeight().v << ',' << static_cast<int>(node->getOrient().getValue())
          << ',' << node->isPlaced() << ';';
    }
    return out.str();
  }
  fillerRepair::RepairOutcome repair(
      const std::vector<int>& deleted, int x, int row, int width = 1, bool commitResult = false, int height = 1)
  {
    const Node* old = dp->getNetwork()->getNode(fillerIds.at(deleted.front()));
    Node temporary = *old;
    temporary.setMaster(
        dp->getNetwork()->getMaster(master("S" + std::to_string(width) + (height == 2 ? "D" : "")).getLibCellId()));
    temporary.setType(Node::CELL);
    temporary.setWidth(DbuX{width * siteWidth});
    temporary.setHeight(DbuY{height * kHeight});
    temporary.setPlaced(false);
    std::vector<CellChangeRecord> overlays;
    std::set<std::pair<int, int>> released;
    std::set<eUNL::LeafCellID> removedIds;
    for (int index : deleted) {
      const Node* removed = dp->getNetwork()->getNode(fillerIds.at(index));
      const auto oldMaster = removed->getMaster()->getDbMaster();
      overlays.emplace_back(
          OpType::Delete, removed->getDbInst(), eUTL::UvDist(removed->getLeft().v),
          eUTL::UvDist(removed->getBottom().v), oldMaster, oldMaster, removed->getOrient());
      removedIds.insert(removed->getDbInst());
      for (int r = removed->getBottom().v / kHeight; r < (removed->getBottom().v + removed->getHeight().v) / kHeight;
           ++r) {
        for (int c = removed->getLeft().v / siteWidth; c < (removed->getLeft().v + removed->getWidth().v) / siteWidth;
             ++c) {
          EXPECT_TRUE(released.emplace(r, c).second);
        }
      }
    }
    for (int r = row; r < row + height; ++r) {
      for (int c = x; c < x + width; ++c) {
        released.erase({r, c});
      }
    }
    const ipl::CheckRequest request{&temporary, GridX{x}, GridY{row}, orientation(row), overlays};
    EXPECT_TRUE(engine->isReady());
    const std::string before = snapshot();
    const auto result = engine->repair(request);
    EXPECT_EQ(snapshot(), before);
    if (result.hasSolution) {
      std::set<std::pair<int, int>> addedSites;
      std::set<std::string> addedNames;
      for (const auto& change : result.changes) {
        EXPECT_NE(change.op_, OpType::Delete);
        if (change.op_ != OpType::Add) {
          EXPECT_EQ(removedIds.count(std::get<eUNL::LeafCellID>(change.cell_data_)), 0u);
          continue;
        }
        EXPECT_TRUE(addedNames.insert(std::get<std::string>(change.cell_data_)).second);
        const auto& addedMaster = db.design.getLibAcc().getPhysLibCell(change.new_lib_cell_);
        const int col = change.x_.getStorage() / siteWidth;
        const int bottom = change.y_.getStorage() / kHeight;
        for (int r = bottom; r < bottom + addedMaster.getHeight().getStorage() / kHeight; ++r) {
          for (int c = col; c < col + addedMaster.getWidth().getStorage() / siteWidth; ++c) {
            EXPECT_TRUE(addedSites.emplace(r, c).second) << "overlapping Add at " << r << ',' << c;
          }
        }
      }
      EXPECT_EQ(addedSites, released);
      const auto checked = checker->checkPlaceWithOverlays(
          request,
          eUTL::Rect(
              eUTL::UvDist(0), eUTL::UvDist(0), eUTL::UvDist(kCols * siteWidth), eUTL::UvDist(kRows * kHeight - 1)),
          {result.changes});
      EXPECT_EQ(checked.size(), 1u);
      if (!checked.empty()) {
        EXPECT_TRUE(checked.front().isLegal);
      }
      if (commitResult) {
        auto batch = request.overlayChanges;
        batch.insert(batch.end(), result.changes.begin(), result.changes.end());
        batch.emplace_back(
            OpType::Add, std::string("TARGET_" + std::to_string(old->getDbInst().getIndexValue())),
            eUTL::UvDist(x * siteWidth), eUTL::UvDist(row * kHeight), eLIB::LibCellID(),
            temporary.getMaster()->getDbMaster(), orientation(row));
        EXPECT_TRUE(dp->commit(batch));
        for (int r = 0; r < kRows; ++r) {
          for (int c = 0; c < kCols; ++c) {
            const Node* placed = dp->getGrid()->gridPixel(GridX{c}, GridY{r})->cell;
            EXPECT_NE(placed, nullptr) << "gap after commit at " << r << ',' << c;
            if (placed != nullptr) {
              const auto physical = db.desMgr().getPhysCell(placed->getDbInst());
              EXPECT_TRUE(physical.isValid());
              EXPECT_EQ(physical.getPhysMaster().getLibCellId(), placed->getMaster()->getDbMaster());
              EXPECT_EQ(physical.getOrigin().getX().getStorage(), placed->getLeft().v);
              EXPECT_EQ(physical.getOrigin().getY().getStorage(), placed->getBottom().v);
              EXPECT_EQ(physical.getOrient(), placed->getOrient());
            }
          }
        }
      }
    } else {
      EXPECT_TRUE(result.changes.empty());
    }
    return result;
  }
  std::vector<int> addWidths(const fillerRepair::RepairOutcome& result)
  {
    std::vector<int> widths;
    for (const auto& change : result.changes) {
      if (change.op_ == OpType::Add) {
        widths.push_back(
            db.design.getLibAcc().getPhysLibCell(change.new_lib_cell_).getWidth().getStorage() / siteWidth);
      }
    }
    return widths;
  }
};

TEST(FillerAvoidPattern, ParsesAbsoluteSymmetricWidthsAndRejectsMalformedInputAtomically)
{
  fillerSetting setting(nullptr);
  setting.addAvoidPattern("1:2 3:3");
  EXPECT_TRUE(setting.needAvoidAbut({1, 2}));
  EXPECT_TRUE(setting.needAvoidAbut({2, 1}));
  EXPECT_TRUE(setting.needAvoidAbut({3, 3}));
  EXPECT_FALSE(setting.needAvoidAbut({2, 4}));
  const auto before = setting.getAvoidPattern();
  for (const auto* invalid : {"1", "1:", ":2", "1:2:3", "1:2x", "-1:2", "0:2", "+1:2", "1.5:2", "2147483648:2"}) {
    SCOPED_TRACE(invalid);
    EXPECT_THROW(setting.addAvoidPattern(std::string("4:5 ") + invalid), std::invalid_argument);
    EXPECT_EQ(setting.getAvoidPattern(), before);
  }
  EXPECT_NO_THROW(setting.addAvoidPattern("  \t"));
  EXPECT_EQ(setting.getAvoidPattern(), before);
}

TEST(FillerAvoidPattern, AddedFillersCheckBothExistingSideNeighboursInSiteUnits)
{
  for (const auto* pattern : {"1:2", "2:1"}) {
    for (bool left : {true, false}) {
      OptionFixture f(pattern, {{10, 2, 4}, {left ? 8 : 14, 2, 2}});
      EXPECT_FALSE(f.repair({0}, left ? 11 : 10, 2, 3).hasSolution);
    }
  }
  OptionFixture dbuNotSites("5:10", {{10, 2, 4}, {8, 2, 2}});
  EXPECT_TRUE(dbuNotSites.repair({0}, 11, 2, 3).hasSolution);
}

TEST(FillerAvoidPattern, OneToTwoDoesNotForbidTwoToFour)
{
  OptionFixture allowed("1:2", {{10, 2, 3}, {6, 2, 4}}, "F2");
  const auto result = allowed.repair({0}, 12, 2);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(allowed.addWidths(result), (std::vector<int>{2}));
  OptionFixture forbidden("2:4", {{10, 2, 3}, {6, 2, 4}}, "F2");
  EXPECT_FALSE(forbidden.repair({0}, 12, 2).hasSolution);
}

TEST(FillerAvoidPattern, NewNeighboursBacktrackToAValidTilingInEitherOrderingMode)
{
  for (bool followOrder : {true, false}) {
    OptionFixture f("1:2", {{10, 2, 4}}, "F2 F1", followOrder);
    const auto result = f.repair({0}, 10, 2);
    ASSERT_TRUE(result.hasSolution);
    EXPECT_EQ(f.addWidths(result), (std::vector<int>{1, 1, 1}));
  }
  OptionFixture impossible("1:2 1:1", {{10, 2, 4}});
  EXPECT_FALSE(impossible.repair({0}, 10, 2).hasSolution);
}

TEST(FillerAvoidPattern, SameWidthPatternStillAllowsMixedWidthsAndStdCellAbutment)
{
  OptionFixture f("1:1", {{10, 2, 4}}, "F1 F2");
  const auto result = f.repair({0}, 10, 2);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(f.addWidths(result), (std::vector<int>{1, 2}));
}

TEST(FillerAvoidPattern, DeletedNeighboursAndVerticalOrCornerContactsDoNotParticipate)
{
  for (int neighbourX : {11, 12}) {
    OptionFixture f("1:2", {{10, 0, 2}, {neighbourX, 1, 2}});
    const auto result = f.repair({0}, 10, 0);
    ASSERT_TRUE(result.hasSolution);
    EXPECT_EQ(f.addWidths(result), (std::vector<int>{1}));
  }
}

TEST(FillerAvoidPattern, TwoRowAddsCheckEveryRowOfTheirSideBoundary)
{
  OptionFixture f("1:2", {{10, 0, 2, 2}, {12, 1, 2}}, "F1D F2 F1");
  const auto result = f.repair({0}, 10, 0);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(f.addWidths(result), (std::vector<int>{1, 2}));
  for (const auto& change : result.changes) {
    EXPECT_EQ(
        f.db.design.getLibAcc().getPhysLibCell(change.new_lib_cell_).getHeight().getStorage(), OptionFixture::kHeight);
  }
  OptionFixture impossible("1:2", {{10, 0, 2, 2}, {12, 0, 2}}, "F1D F2 F1");
  EXPECT_FALSE(impossible.repair({0}, 10, 0).hasSolution);
}

TEST(FillerAvoidPattern, MixedHeightAddsCheckTheirSharedRow)
{
  OptionFixture unrestricted("", {{10, 0, 3, 2}}, "F1D F2 F1");
  const auto seed = unrestricted.repair({0}, 10, 0, 2);
  ASSERT_TRUE(seed.hasSolution);
  EXPECT_EQ(unrestricted.addWidths(seed), (std::vector<int>{1, 2}));
  ASSERT_FALSE(seed.changes.empty());
  EXPECT_EQ(
      unrestricted.db.design.getLibAcc().getPhysLibCell(seed.changes.front().new_lib_cell_).getHeight().getStorage(),
      2 * OptionFixture::kHeight);

  OptionFixture constrained("1:2", {{10, 0, 3, 2}}, "F1D F2 F1");
  const auto result = constrained.repair({0}, 10, 0, 2);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(constrained.addWidths(result), (std::vector<int>{1, 1, 1}));
}

TEST(FillerAvoidPattern, ReusedEngineReadsChangedNeighbourWidthAfterCommit)
{
  OptionFixture f("1:2", {{10, 0, 2}, {12, 0, 2}});
  EXPECT_FALSE(f.repair({0}, 10, 0).hasSolution);
  Node* neighbour = f.dp->getNetwork()->getNode(f.fillerIds.at(1));
  ASSERT_TRUE(f.dp->commit(
      {{OpType::Replace, neighbour->getDbInst(), eUTL::UvDist(12 * f.siteWidth), eUTL::UvDist(0),
        neighbour->getMaster()->getDbMaster(), f.master("F1").getLibCellId(), OptionFixture::orientation(0)},
       {OpType::Add, std::string("NEW_STD"), eUTL::UvDist(13 * f.siteWidth), eUTL::UvDist(0), eLIB::LibCellID(),
        f.master("S1").getLibCellId(), OptionFixture::orientation(0)}}));
  const auto result = f.repair({0}, 10, 0);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(f.addWidths(result), (std::vector<int>{1}));
}

TEST(FillerAvoidPattern, ExactCoverDoesNotRetileExistingForbiddenPairs)
{
  OptionFixture f("1:2", {{12, 2, 2}, {8, 2, 1}, {9, 2, 2}});
  const auto result = f.repair({0}, 12, 2, 2);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
}

TEST(FillerRetileCommit, GeneratedNameRemainsUniqueAfterCommittedFillerMoves)
{
  OptionFixture f("", {{10, 2, 2}});
  const auto first = f.repair({0}, 11, 2, 1, true);
  ASSERT_TRUE(first.hasSolution);
  ASSERT_EQ(first.changes.size(), 1u);
  const std::string originalName = std::get<std::string>(first.changes.front().cell_data_);
  auto* grid = f.dp->getGrid();
  Node* generated = grid->gridPixel(GridX{10}, GridY{2})->cell;
  Node* target = grid->gridPixel(GridX{11}, GridY{2})->cell;
  Node* movedStd = grid->gridPixel(GridX{20}, GridY{2})->cell;
  ASSERT_TRUE(generated->isFiller());
  const auto reopenedId = movedStd->getDbInst();
  const auto record = [&](OpType op, const Node* cell, int x, eLIB::LibCellID masterId) {
    return CellChangeRecord{
        op,
        cell->getDbInst(),
        eUTL::UvDist(x * f.siteWidth),
        eUTL::UvDist(2 * OptionFixture::kHeight),
        cell->getMaster()->getDbMaster(),
        masterId,
        cell->getOrient()};
  };
  ASSERT_TRUE(f.dp->commit(
      {record(OpType::Delete, target, 11, target->getMaster()->getDbMaster()),
       record(OpType::Replace, generated, 20, generated->getMaster()->getDbMaster()),
       record(OpType::Replace, movedStd, 10, f.master("F2").getLibCellId())}));
  // Also occupy the first suffix; names are live DB data, not setup metadata.
  const Node* other = grid->gridPixel(GridX{30}, GridY{2})->cell;
  f.db.desMgr().cells_.at(other->getDbInst()).name = originalName + "_1";
  f.fillerIds[0] = reopenedId;
  const auto second = f.repair({0}, 11, 2);
  ASSERT_TRUE(second.hasSolution);
  ASSERT_EQ(second.changes.size(), 1u);
  EXPECT_EQ(std::get<std::string>(second.changes.front().cell_data_), originalName + "_2");
  const auto committed = f.repair({0}, 11, 2, 1, true);
  ASSERT_TRUE(committed.hasSolution);
  ASSERT_EQ(committed.changes.size(), 1u);
  EXPECT_EQ(committed.changes.front().cell_data_, second.changes.front().cell_data_);
  EXPECT_NE(grid->gridPixel(GridX{10}, GridY{2})->cell, generated);
  EXPECT_EQ(grid->gridPixel(GridX{20}, GridY{2})->cell, generated);
}

struct RetileCase
{
  const char* name;
  std::vector<FillerRect> fillers;
  FillerRect target;
  std::string masters;
  bool legal;
};

class FillerRetileLayout : public ::testing::TestWithParam<RetileCase>
{
};

TEST_P(FillerRetileLayout, ProbeIsDeterministicAndFullCommitPreservesPlacement)
{
  const auto& test = GetParam();
  OptionFixture f("", test.fillers, test.masters);
  std::vector<int> deletes;
  for (size_t i = 0; i < test.fillers.size(); ++i) {
    deletes.push_back(static_cast<int>(i));
  }
  const auto& target = test.target;
  const auto first = f.repair(deletes, target.x, target.row, target.width, false, target.height);
  ASSERT_EQ(first.hasSolution, test.legal);
  // Overlay order is not part of the physical problem, nor should it choose a
  // different solution. Repeat on the SAME engine to expose cached placement.
  std::reverse(deletes.begin(), deletes.end());
  const auto repeated = f.repair(deletes, target.x, target.row, target.width, test.legal, target.height);
  ASSERT_EQ(repeated.hasSolution, first.hasSolution);
  ASSERT_EQ(repeated.changes.size(), first.changes.size());
  for (size_t i = 0; i < first.changes.size(); ++i) {
    const auto& a = first.changes[i];
    const auto& b = repeated.changes[i];
    EXPECT_EQ(a.op_, b.op_);
    EXPECT_EQ(a.cell_data_, b.cell_data_);
    EXPECT_EQ(a.orig_lib_cell_, b.orig_lib_cell_);
    EXPECT_EQ(a.new_lib_cell_, b.new_lib_cell_);
    EXPECT_EQ(a.x_, b.x_);
    EXPECT_EQ(a.y_, b.y_);
    EXPECT_EQ(a.orientation_, b.orientation_);
  }
}

INSTANTIATE_TEST_SUITE_P(
    RepresentativeGaps, FillerRetileLayout,
    ::testing::Values(
        RetileCase{"ExactSingle", {{10, 2, 2}}, {10, 2, 2}, "F2 F1", true},
        RetileCase{"BottomLeft", {{0, 0, 4}}, {1, 0, 2}, "F2 F1", true},
        RetileCase{"TopRight", {{36, 11, 4}}, {37, 11, 2}, "F2 F1", true},
        RetileCase{"SplitEvenGaps", {{10, 4, 6}}, {12, 4, 2}, "F2 F1", true},
        RetileCase{"AsymmetricOddRow", {{10, 3, 6}}, {11, 3, 1}, "F2 F1", true},
        RetileCase{"WidthGcdImpossible", {{10, 2, 4}}, {10, 2, 1}, "F2", false},
        RetileCase{"MultipleWideFillers", {{8, 2, 4}, {12, 2, 4}}, {10, 2, 3}, "F2 F1", true},
        RetileCase{"DoubleHeightLowerCut", {{10, 4, 4, 2}}, {11, 4, 2}, "F1D F2 F1", true},
        RetileCase{"DoubleHeightUpperCut", {{10, 4, 4, 2}}, {11, 5, 2}, "F1D F2 F1", true},
        RetileCase{"DoubleHeightTarget", {{10, 4, 4, 2}}, {11, 4, 2, 2}, "F1D F2 F1", true},
        RetileCase{
            "FourSingleHeightDeletes",
            {{10, 4, 2}, {12, 4, 2}, {10, 5, 2}, {12, 5, 2}},
            {11, 4, 2, 2},
            "F1D F2 F1",
            true},
        RetileCase{"StaggeredDisconnectedGaps", {{8, 4, 4}, {9, 5, 4}}, {10, 4, 2, 2}, "F1D F2 F1", true},
        RetileCase{"HeightParityImpossible", {{10, 4, 4, 2}}, {10, 4, 1}, "F1D", false}),
    [](const ::testing::TestParamInfo<RetileCase>& info) { return info.param.name; });
}  // namespace
