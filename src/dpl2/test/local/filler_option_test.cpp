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
  static constexpr int kRows = 5;
  static constexpr int kCols = 30;
  static constexpr int kHeight = 8;
  fake_udm::DesignDb db;
  std::unique_ptr<DePlace> dp;
  std::shared_ptr<Padding> padding = std::make_shared<Padding>();
  std::vector<eUNL::LeafCellID> fillerIds;
  std::unique_ptr<ipl::ImplantLayerChecker> checker;
  std::unique_ptr<fillerRepair::FillerRepairEngine> engine;
  int siteWidth;

  OptionFixture(const std::string& pattern,
                const std::vector<FillerRect>& fillers,
                std::string candidates = "F2 F1",
                bool followOrder = true,
                int widthDbu = 5)
      : siteWidth(widthDbu)
  {
    db.coreSite.name_ = "CORE";
    db.coreSite.width_ = eUTL::UvDist(siteWidth);
    db.coreSite.height_ = eUTL::UvDist(kHeight);
    db.tech().addLayer("VTL_N", true, 0, siteWidth, siteWidth);
    db.tech().addLayer("VTL_P", true, 1, siteWidth, siteWidth);
    const auto addMaster = [&](const std::string& name,
                               int id,
                               int width,
                               int height,
                               bool filler) {
      auto& master
          = db.addMaster(name, id, width * siteWidth, height * kHeight, filler);
      for (int row = 0; row < height; ++row) {
        const int bottomLayer = (row + height - 1) % 2;
        fake_udm::DesignDb::addShape(
            master, bottomLayer, row * kHeight, row * kHeight + kHeight / 2);
        fake_udm::DesignDb::addShape(master,
                                     1 - bottomLayer,
                                     row * kHeight + kHeight / 2,
                                     (row + 1) * kHeight);
      }
    };
    for (int width : {1, 2, 3, 4, 6}) {
      for (int height : {1, 2}) {
        addMaster(fillerName(width, height),
                  100 + width * 10 + height,
                  width,
                  height,
                  true);
      }
    }
    for (int width : {1, 2, 3}) {
      addMaster("S" + std::to_string(width), 1000 + width, width, 1, false);
    }
    for (int row = 0; row < kRows; ++row) {
      auto& physicalRow = db.desMgr().addRow(
          0, row * kHeight, siteWidth, kHeight, kCols * siteWidth);
      physicalRow.site_.name_ = "CORE";
      physicalRow.orient_ = orientation(row);
    }
    int id = 100;
    std::set<std::pair<int, int>> occupied;
    for (const auto& filler : fillers) {
      const std::string name = fillerName(filler.width, filler.height);
      candidates
          += " " + name;  // deleted/unchanged filler masters are configured too
      const eUNL::LeafCellID cellId(0, id++);
      fillerIds.push_back(cellId);
      db.desMgr().addCell(cellId,
                          &master(name),
                          filler.x * siteWidth,
                          filler.row * kHeight,
                          orientation(filler.row),
                          eUNL::PhysObjStatus::PLACED,
                          "OLD_" + std::to_string(id));
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
          db.desMgr().addCell(cellId,
                              &master("S1"),
                              col * siteWidth,
                              row * kHeight,
                              orientation(row),
                              eUNL::PhysObjStatus::PLACED,
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
    const eUTL::Rect core(eUTL::UvDist(0),
                          eUTL::UvDist(0),
                          eUTL::UvDist(kCols * siteWidth),
                          eUTL::UvDist(kRows * kHeight));
    padding->setDesginManager(&db.desMgr());
    dp->getGrid()->setCore(core);
    dp->getGrid()->examineRows(&db.desMgr());
    dp->getGrid()->initGrid(&db.desMgr(), padding, 100, 100);
    dp->getNetwork()->setCore(core);
    const EdgeTypeTable edges;
    for (const auto& lib : db.design.getLibAcc().getLibCellIter(true, false)) {
      dp->getNetwork()->addMaster(
          db.design.getLibAcc().getPhysLibCell(lib.getId()),
          *setting,
          dp->getGrid(),
          &edges);
    }
    for (const auto& [cellId, data] : db.desMgr().cells_) {
      (void) data;
      dp->getNetwork()->addNode(cellId, &db.desMgr());
      dp->getGrid()->paintPixel(dp->getNetwork()->getNode(cellId));
    }
    checker = std::make_unique<ipl::ImplantLayerChecker>(
        dp->getGrid(), &db.design, dp->getNetwork());
    engine = std::make_unique<fillerRepair::FillerRepairEngine>(*checker);
  }

  static eUTL::PhysOrientation orientation(int row)
  {
    return row % 2 == 0 ? eUTL::PhysOrientationE::MX
                        : eUTL::PhysOrientationE::R0;
  }
  const eLIB::PhysLibCell& master(const std::string& name)
  {
    const auto& library = db.design.getLibAcc();
    return library.getPhysLibCell(
        library.getLibCell(library.findModule(name))->getId());
  }
  std::string snapshot()
  {
    std::ostringstream out;
    for (const auto& [id, data] : db.desMgr().cells_) {
      const Node* node = dp->getNetwork()->getNode(id);
      out << id.getIndexValue() << ',' << data.valid << ',' << data.name << ','
          << data.master->getLibCellId().getIndexValue() << ','
          << data.origin.getX().getStorage() << ','
          << data.origin.getY().getStorage() << ','
          << static_cast<int>(data.orient.getValue()) << ',' << node << ';';
    }
    for (GridY row{0}; row < dp->getGrid()->getRowCount(); ++row) {
      for (GridX col{0}; col < dp->getGrid()->getRowSiteCount(); ++col) {
        const Pixel* pixel = dp->getGrid()->gridPixel(col, row);
        out << pixel->cell << ',' << pixel->padding_reserved_by << ';';
      }
    }
    return out.str();
  }
  fillerRepair::RepairOutcome repair(int deleted, int x, int row, int width = 1)
  {
    const Node* old = dp->getNetwork()->getNode(fillerIds.at(deleted));
    Node temporary = *old;
    temporary.setMaster(dp->getNetwork()->getMaster(
        master("S" + std::to_string(width)).getLibCellId()));
    temporary.setType(Node::CELL);
    temporary.setWidth(DbuX{width * siteWidth});
    temporary.setHeight(DbuY{kHeight});
    temporary.setPlaced(false);
    const auto oldMaster = old->getMaster()->getDbMaster();
    const ipl::CheckRequest request{&temporary,
                                    GridX{x},
                                    GridY{row},
                                    orientation(row),
                                    {{OpType::Delete,
                                      old->getDbInst(),
                                      eUTL::UvDist(old->getLeft().v),
                                      eUTL::UvDist(old->getBottom().v),
                                      oldMaster,
                                      oldMaster,
                                      old->getOrient()}}};
    EXPECT_TRUE(engine->isReady());
    const std::string before = snapshot();
    const auto result = engine->repair(request);
    EXPECT_EQ(snapshot(), before);
    if (result.hasSolution) {
      const auto checked = checker->checkPlaceWithOverlays(
          request,
          eUTL::Rect(eUTL::UvDist(0),
                     eUTL::UvDist(0),
                     eUTL::UvDist(kCols * siteWidth),
                     eUTL::UvDist(kRows * kHeight - 1)),
          {result.changes});
      EXPECT_EQ(checked.size(), 1u);
      if (!checked.empty()) {
        EXPECT_TRUE(checked.front().isLegal);
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
        widths.push_back(db.design.getLibAcc()
                             .getPhysLibCell(change.new_lib_cell_)
                             .getWidth()
                             .getStorage()
                         / siteWidth);
      }
    }
    return widths;
  }
};

TEST(FillerAvoidPattern,
     ParsesAbsoluteSymmetricWidthsAndRejectsMalformedInputAtomically)
{
  fillerSetting setting(nullptr);
  setting.addAvoidPattern("1:2 3:3");
  EXPECT_TRUE(setting.needAvoidAbut({1, 2}));
  EXPECT_TRUE(setting.needAvoidAbut({2, 1}));
  EXPECT_TRUE(setting.needAvoidAbut({3, 3}));
  EXPECT_FALSE(setting.needAvoidAbut({2, 4}));
  const auto before = setting.getAvoidPattern();
  for (const auto* invalid : {"1",
                              "1:",
                              ":2",
                              "1:2:3",
                              "1:2x",
                              "-1:2",
                              "0:2",
                              "+1:2",
                              "1.5:2",
                              "2147483648:2"}) {
    SCOPED_TRACE(invalid);
    EXPECT_THROW(setting.addAvoidPattern(std::string("4:5 ") + invalid),
                 std::invalid_argument);
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
      EXPECT_FALSE(f.repair(0, left ? 11 : 10, 2, 3).hasSolution);
    }
  }
  OptionFixture dbuNotSites("5:10", {{10, 2, 4}, {8, 2, 2}});
  EXPECT_TRUE(dbuNotSites.repair(0, 11, 2, 3).hasSolution);
}

TEST(FillerAvoidPattern, OneToTwoDoesNotForbidTwoToFour)
{
  OptionFixture allowed("1:2", {{10, 2, 3}, {6, 2, 4}}, "F2");
  const auto result = allowed.repair(0, 12, 2);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(allowed.addWidths(result), (std::vector<int>{2}));
  OptionFixture forbidden("2:4", {{10, 2, 3}, {6, 2, 4}}, "F2");
  EXPECT_FALSE(forbidden.repair(0, 12, 2).hasSolution);
}

TEST(FillerAvoidPattern,
     NewNeighboursBacktrackToAValidTilingInEitherOrderingMode)
{
  for (bool followOrder : {true, false}) {
    OptionFixture f("1:2", {{10, 2, 4}}, "F2 F1", followOrder);
    const auto result = f.repair(0, 10, 2);
    ASSERT_TRUE(result.hasSolution);
    EXPECT_EQ(f.addWidths(result), (std::vector<int>{1, 1, 1}));
  }
  OptionFixture impossible("1:2 1:1", {{10, 2, 4}});
  EXPECT_FALSE(impossible.repair(0, 10, 2).hasSolution);
}

TEST(FillerAvoidPattern,
     SameWidthPatternStillAllowsMixedWidthsAndStdCellAbutment)
{
  OptionFixture f("1:1", {{10, 2, 4}}, "F1 F2");
  const auto result = f.repair(0, 10, 2);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(f.addWidths(result), (std::vector<int>{1, 2}));
}

TEST(FillerAvoidPattern,
     DeletedNeighboursAndVerticalOrCornerContactsDoNotParticipate)
{
  for (int neighbourX : {11, 12}) {
    OptionFixture f("1:2", {{10, 0, 2}, {neighbourX, 1, 2}});
    const auto result = f.repair(0, 10, 0);
    ASSERT_TRUE(result.hasSolution);
    EXPECT_EQ(f.addWidths(result), (std::vector<int>{1}));
  }
}

TEST(FillerAvoidPattern, TwoRowAddsCheckEveryRowOfTheirSideBoundary)
{
  OptionFixture f("1:2", {{10, 0, 2, 2}, {12, 1, 2}}, "F1D F2 F1");
  const auto result = f.repair(0, 10, 0);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(f.addWidths(result), (std::vector<int>{1, 2}));
  for (const auto& change : result.changes) {
    EXPECT_EQ(f.db.design.getLibAcc()
                  .getPhysLibCell(change.new_lib_cell_)
                  .getHeight()
                  .getStorage(),
              OptionFixture::kHeight);
  }
  OptionFixture impossible("1:2", {{10, 0, 2, 2}, {12, 0, 2}}, "F1D F2 F1");
  EXPECT_FALSE(impossible.repair(0, 10, 0).hasSolution);
}

TEST(FillerAvoidPattern, MixedHeightAddsCheckTheirSharedRow)
{
  OptionFixture unrestricted("", {{10, 0, 3, 2}}, "F1D F2 F1");
  const auto seed = unrestricted.repair(0, 10, 0, 2);
  ASSERT_TRUE(seed.hasSolution);
  EXPECT_EQ(unrestricted.addWidths(seed), (std::vector<int>{1, 2}));
  ASSERT_FALSE(seed.changes.empty());
  EXPECT_EQ(unrestricted.db.design.getLibAcc()
                .getPhysLibCell(seed.changes.front().new_lib_cell_)
                .getHeight()
                .getStorage(),
            2 * OptionFixture::kHeight);

  OptionFixture constrained("1:2", {{10, 0, 3, 2}}, "F1D F2 F1");
  const auto result = constrained.repair(0, 10, 0, 2);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(constrained.addWidths(result), (std::vector<int>{1, 1, 1}));
}

TEST(FillerAvoidPattern, ReusedEngineReadsChangedNeighbourWidthAfterCommit)
{
  OptionFixture f("1:2", {{10, 0, 2}, {12, 0, 2}});
  EXPECT_FALSE(f.repair(0, 10, 0).hasSolution);
  Node* neighbour = f.dp->getNetwork()->getNode(f.fillerIds.at(1));
  ASSERT_TRUE(f.dp->commit({{OpType::Replace,
                             neighbour->getDbInst(),
                             eUTL::UvDist(12 * f.siteWidth),
                             eUTL::UvDist(0),
                             neighbour->getMaster()->getDbMaster(),
                             f.master("F1").getLibCellId(),
                             OptionFixture::orientation(0)},
                            {OpType::Add,
                             std::string("NEW_STD"),
                             eUTL::UvDist(13 * f.siteWidth),
                             eUTL::UvDist(0),
                             eLIB::LibCellID(),
                             f.master("S1").getLibCellId(),
                             OptionFixture::orientation(0)}}));
  const auto result = f.repair(0, 10, 0);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_EQ(f.addWidths(result), (std::vector<int>{1}));
}

TEST(FillerAvoidPattern, ExactCoverDoesNotRetileExistingForbiddenPairs)
{
  OptionFixture f("1:2", {{12, 2, 2}, {8, 2, 1}, {9, 2, 2}});
  const auto result = f.repair(0, 12, 2, 2);
  ASSERT_TRUE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
}
}  // namespace
