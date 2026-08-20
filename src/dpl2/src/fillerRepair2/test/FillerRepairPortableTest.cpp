#include <drc/ImplantLayerChecker.h>
#include <drc/ImplantLayerCheckerHelper.h>
#include <fillerRepair/FillerRepairEngine.h>
#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace dpl2::ipl {
namespace {

constexpr Dbu kSiteWidth = 10;
constexpr Dbu kRowHeight = 100;
constexpr Dbu kMinRule = 20;
constexpr RowId kRowCount = 3;
constexpr ColId kColCount = 12;

::Rect rect(Dbu xl, Dbu yl, Dbu xh, Dbu yh)
{
  return ::Rect(UvDist(xl), UvDist(yl), UvDist(xh), UvDist(yh));
}

MasterItem master(MasterId id, LayerId nLayer, bool filler)
{
  MasterItem item;
  item.masterId = id;
  item.width = kSiteWidth;
  item.height = kRowHeight;
  item.siteHeight = kRowHeight;
  item.siteName = "core";
  item.isFiller = filler;
  item.shapes = {{id, 2 * id, nLayer, rect(0, 0, kSiteWidth, kRowHeight / 2)},
                 {id,
                  2 * id + 1,
                  nLayer + 3,
                  rect(0, kRowHeight / 2, kSiteWidth, kRowHeight)}};
  item.rawShapes = item.shapes;
  return item;
}

ImplantInput input()
{
  ImplantInput data;
  data.layers = {{0, "F1_N", Layer::Vt::L, Layer::Polar::N},
                 {1, "F2_N", Layer::Vt::H, Layer::Polar::N},
                 {2, "F3_N", Layer::Vt::UL, Layer::Polar::N},
                 {3, "F1_P", Layer::Vt::L, Layer::Polar::P},
                 {4, "F2_P", Layer::Vt::H, Layer::Polar::P},
                 {5, "F3_P", Layer::Vt::UL, Layer::Polar::P}};
  int ruleId = 0;
  for (LayerId layer = 0; layer < 6; ++layer) {
    data.rules.emplace_back(ruleId++, RuleSource::Width, layer, kMinRule);
    data.rules.emplace_back(ruleId++, RuleSource::Spacing, layer, kMinRule);
  }
  data.masters = {master(0, 0, false),
                  master(1, 1, false),
                  master(2, 2, false),
                  master(3, 0, true),
                  master(4, 1, true),
                  master(5, 2, true)};
  const std::array<MasterId, 6> pattern{0, 3, 1, 4, 2, 5};
  for (RowId row = 0; row < kRowCount; ++row) {
    for (ColId col = 0; col < kColCount; ++col) {
      const MasterId masterId
          = pattern[static_cast<size_t>(col % pattern.size())];
      data.placedInsts.push_back(
          {row * kColCount + col,
           masterId,
           row,
           col,
           row % 2 == 0 ? PhysOrientationE::R0 : PhysOrientationE::MX,
           masterId >= 3});
    }
  }
  data.rowCount = kRowCount;
  data.colCount = kColCount;
  data.basePolar = Layer::Polar::N;
  data.rowHeight = kRowHeight;
  data.siteWidth = kSiteWidth;
  data.fillerSetting.present = true;
  data.fillerSetting.fillerMasterIds = {3, 4, 5};
  return data;
}

TEST(FillerRepairPortableTest, CheckerRepairsTemporaryNodeWithoutMutation)
{
  ImplantLayerCheckerHelper helper;
  helper.initialize(input());
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);
  checker.setFillerRepairEnabled(true);

  constexpr RowId targetRow = 1;
  constexpr ColId targetCol = 4;
  constexpr ColId bridgeCol = 5;
  Network& network = *helper.getNetwork();
  Node* replaced = network.getNode(targetRow * kColCount + targetCol);
  Node* bridge = network.getNode(targetRow * kColCount + bridgeCol);
  ASSERT_NE(replaced, nullptr);
  ASSERT_NE(bridge, nullptr);
  ASSERT_NE(replaced->getMaster(), nullptr);
  ASSERT_NE(bridge->getMaster(), nullptr);
  const MasterId oldTargetMaster = replaced->getMaster()->getId();
  const MasterId oldBridgeMaster = bridge->getMaster()->getId();

  Node temporary;
  temporary.setId(replaced->getId());
  temporary.setDbInst(replaced->getDbInst());
  temporary.setMaster(network.getMaster(0));
  temporary.setType(Node::CELL);
  temporary.setWidth(replaced->getWidth());
  temporary.setHeight(replaced->getHeight());
  temporary.setLeft(replaced->getLeft());
  temporary.setBottom(replaced->getBottom());
  temporary.setOrient(PhysOrientationE::MX);

  std::vector<CellChangeRecord> overlay{{OpType::Delete,
                                         replaced->getDbInst(),
                                         UvDist(replaced->getLeft().v),
                                         UvDist(replaced->getBottom().v),
                                         replaced->getMaster()->getDbMaster(),
                                         replaced->getMaster()->getDbMaster(),
                                         replaced->getOrient()}};
  FillerChanges changes;
  EXPECT_TRUE(checker.check(&temporary,
                            GridX(targetCol),
                            GridY(targetRow),
                            PhysOrientationE::MX,
                            changes,
                            overlay));
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes.front().op_, OpType::Replace);
  EXPECT_EQ(std::get<LeafCellID>(changes.front().cell_data_),
            bridge->getDbInst());
  EXPECT_EQ(changes.front().new_lib_cell_, network.getMaster(3)->getDbMaster());
  EXPECT_EQ(replaced->getMaster()->getId(), oldTargetMaster);
  EXPECT_EQ(bridge->getMaster()->getId(), oldBridgeMaster);
}

}  // namespace
}  // namespace dpl2::ipl
