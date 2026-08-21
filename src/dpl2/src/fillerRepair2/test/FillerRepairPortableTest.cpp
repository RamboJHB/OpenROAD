#include <drc/ImplantLayerChecker.h>
#include <drc/ImplantLayerCheckerHelper.h>
#include <fillerRepair/FillerRepairEngine.h>
#include <gtest/gtest.h>

#include <array>
#include <thread>
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

MasterItem master(MasterId id,
                  LayerId nLayer,
                  bool filler,
                  Dbu width = kSiteWidth)
{
  MasterItem item;
  item.masterId = id;
  item.width = width;
  item.height = kRowHeight;
  item.siteHeight = kRowHeight;
  item.siteName = "core";
  item.isFiller = filler;
  item.shapes = {{id, 2 * id, nLayer, rect(0, 0, width, kRowHeight / 2)},
                 {id,
                  2 * id + 1,
                  nLayer + 3,
                  rect(0, kRowHeight / 2, width, kRowHeight)}};
  item.rawShapes = item.shapes;
  return item;
}

MasterItem twoRowMaster(MasterId id, bool filler, Dbu width)
{
  MasterItem item;
  item.masterId = id;
  item.width = width;
  item.height = 2 * kRowHeight;
  item.siteHeight = kRowHeight;
  item.siteName = "core";
  item.isFiller = filler;
  item.shapes = {
      {id, 4 * id, 0, rect(0, 0, width, kRowHeight / 2)},
      {id, 4 * id + 1, 3, rect(0, kRowHeight / 2, width, kRowHeight)},
      {id, 4 * id + 2, 3,
       rect(0, kRowHeight, width, 3 * kRowHeight / 2)},
      {id, 4 * id + 3, 0,
       rect(0, 3 * kRowHeight / 2, width, 2 * kRowHeight)}};
  item.rawShapes = item.shapes;
  return item;
}

CellChangeRecord deleteRecord(const Node& node)
{
  return {OpType::Delete,
          node.getDbInst(),
          UvDist(node.getLeft().v),
          UvDist(node.getBottom().v),
          node.getMaster()->getDbMaster(),
          node.getMaster()->getDbMaster(),
          node.getOrient()};
}

bool hasDiagnostic(const CheckResult& result, const std::string& status)
{
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&status](const Diagnostic& diagnostic) {
                       return diagnostic.status == status;
                     });
}

bool sameChanges(const FillerChanges& left, const FillerChanges& right)
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

ImplantInput multiDeleteInput()
{
  ImplantInput data = input();
  data.masters.push_back(master(6, 0, false, 2 * kSiteWidth));
  for (PlacedInst& placed : data.placedInsts) {
    if (placed.rowId == 1 && placed.colId == 2) {
      placed.masterId = 4;
      placed.isFiller = true;
    } else if (placed.rowId == 1 && placed.colId == 4) {
      placed.masterId = 5;
      placed.isFiller = true;
    }
  }
  return data;
}

ImplantInput twoRowMultiDeleteInput()
{
  ImplantInput data;
  data.layers = {{0, "F1_N", Layer::Vt::L, Layer::Polar::N},
                 {1, "unused_N_1", Layer::Vt::H, Layer::Polar::N},
                 {2, "unused_N_2", Layer::Vt::UL, Layer::Polar::N},
                 {3, "F1_P", Layer::Vt::L, Layer::Polar::P},
                 {4, "unused_P_1", Layer::Vt::H, Layer::Polar::P},
                 {5, "unused_P_2", Layer::Vt::UL, Layer::Polar::P}};
  data.rules = {{0, RuleSource::Width, 0, 2 * kSiteWidth},
                {1, RuleSource::Width, 3, 2 * kSiteWidth}};
  data.masters = {twoRowMaster(0, true, kSiteWidth),
                  twoRowMaster(1, false, 2 * kSiteWidth)};
  data.placedInsts = {{10, 0, 0, 2, PhysOrientationE::R0, true},
                      {11, 0, 0, 3, PhysOrientationE::R0, true}};
  data.rowCount = 3;
  data.colCount = 6;
  data.basePolar = Layer::Polar::N;
  data.rowHeight = kRowHeight;
  data.siteWidth = kSiteWidth;
  data.fillerSetting.present = true;
  data.fillerSetting.fillerMasterIds = {0};
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
  EXPECT_FALSE(checker.check(&temporary, GridX(targetCol), GridY(targetRow),
                             PhysOrientationE::MX));
  EXPECT_TRUE(changes.empty());
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

TEST(FillerRepairPortableTest,
     MultipleFillerDeletesExactCoverAndNeverBecomeRepairCandidates)
{
  ImplantLayerCheckerHelper helper;
  helper.initialize(multiDeleteInput());
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);
  checker.setFillerRepairEnabled(true);

  constexpr RowId targetRow = 1;
  constexpr ColId targetCol = 3;
  Network& network = *helper.getNetwork();
  Node* leftDeleted = network.getNode(targetRow * kColCount + targetCol);
  Node* rightDeleted = network.getNode(targetRow * kColCount + targetCol + 1);
  ASSERT_NE(leftDeleted, nullptr);
  ASSERT_NE(rightDeleted, nullptr);

  Node temporary;
  temporary.setId(leftDeleted->getId());
  temporary.setDbInst(leftDeleted->getDbInst());
  temporary.setMaster(network.getMaster(6));
  temporary.setType(Node::CELL);
  temporary.setWidth(DbuX{2 * kSiteWidth});
  temporary.setHeight(DbuY{kRowHeight});
  temporary.setLeft(DbuX{targetCol * kSiteWidth});
  temporary.setBottom(DbuY{targetRow * kRowHeight});
  temporary.setOrient(PhysOrientationE::MX);

  // Deliberately reverse caller order: the target anchor comes from geometry,
  // not overlayChanges.front().
  std::vector<CellChangeRecord> overlay{deleteRecord(*rightDeleted),
                                        deleteRecord(*leftDeleted)};
  std::vector<MasterId> oldMasters;
  for (const auto& [id, node] : network.getNodes()) {
    (void) id;
    oldMasters.push_back(node->getMaster()->getId());
  }
  FillerChanges changes;

  EXPECT_TRUE(checker.check(&temporary,
                            GridX{targetCol},
                            GridY{targetRow},
                            PhysOrientationE::MX,
                            changes,
                            overlay));
  EXPECT_FALSE(changes.empty());
  for (const CellChangeRecord& change : changes) {
    const LeafCellID* id = std::get_if<LeafCellID>(&change.cell_data_);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(change.op_, OpType::Replace);
    EXPECT_NE(*id, leftDeleted->getDbInst());
    EXPECT_NE(*id, rightDeleted->getDbInst());
  }

  constexpr size_t kWorkers = 8;
  std::array<bool, kWorkers> legal{};
  std::array<FillerChanges, kWorkers> concurrentChanges;
  std::vector<std::thread> workers;
  for (size_t worker = 0; worker < kWorkers; ++worker) {
    workers.emplace_back([&, worker]() {
      std::vector<CellChangeRecord> localOverlay = overlay;
      legal[worker] = checker.check(&temporary,
                                    GridX{targetCol},
                                    GridY{targetRow},
                                    PhysOrientationE::MX,
                                    concurrentChanges[worker],
                                    localOverlay);
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  for (size_t worker = 0; worker < kWorkers; ++worker) {
    EXPECT_TRUE(legal[worker]);
    EXPECT_TRUE(sameChanges(changes, concurrentChanges[worker]));
  }

  size_t index = 0;
  for (const auto& [id, node] : network.getNodes()) {
    (void) id;
    EXPECT_EQ(node->getMaster()->getId(), oldMasters[index++]);
  }
}

TEST(FillerRepairPortableTest, MultipleFillerDeletesMustExactlyCoverTarget)
{
  ImplantLayerCheckerHelper helper;
  helper.initialize(multiDeleteInput());
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);

  constexpr RowId targetRow = 1;
  constexpr ColId targetCol = 3;
  Network& network = *helper.getNetwork();
  Node* first = network.getNode(targetRow * kColCount + targetCol);
  Node* second = network.getNode(targetRow * kColCount + targetCol + 1);
  Node* outside = network.getNode(targetRow * kColCount + targetCol + 2);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(outside, nullptr);

  Node temporary;
  temporary.setId(first->getId());
  temporary.setDbInst(first->getDbInst());
  temporary.setMaster(network.getMaster(6));
  temporary.setType(Node::CELL);
  temporary.setWidth(DbuX{2 * kSiteWidth});
  temporary.setHeight(DbuY{kRowHeight});
  temporary.setLeft(DbuX{targetCol * kSiteWidth});
  temporary.setBottom(DbuY{targetRow * kRowHeight});
  temporary.setOrient(PhysOrientationE::MX);

  CheckRequestOverlay request{&temporary,
                              GridX{targetCol},
                              GridY{targetRow},
                              PhysOrientationE::MX,
                              {deleteRecord(*first)}};
  CheckResult result = checker.checkDirect(request);
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result, "target_not_one_to_one"));

  request.overlayChanges = {deleteRecord(*first),
                            deleteRecord(*second),
                            deleteRecord(*outside)};
  result = checker.checkDirect(request);
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result, "target_overlay_exceeds_footprint"));

  request.overlayChanges = {deleteRecord(*first), deleteRecord(*first)};
  result = checker.checkDirect(request);
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result, "duplicate_target_overlay"));

  Node* standardCell
      = network.getNode(targetRow * kColCount + targetCol + 3);
  ASSERT_NE(standardCell, nullptr);
  ASSERT_FALSE(standardCell->isFiller());
  request.overlayChanges = {deleteRecord(*first), deleteRecord(*standardCell)};
  result = checker.checkDirect(request);
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result, "mixed_target_overlay"));
}

TEST(FillerRepairPortableTest, TwoRowTargetAcceptsMultipleFillerExactCover)
{
  ImplantLayerCheckerHelper helper;
  helper.initialize(twoRowMultiDeleteInput());
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);
  fillerRepair::FillerRepairEngine engine(checker);
  ASSERT_TRUE(engine.isReady());

  Network& network = *helper.getNetwork();
  Node* first = network.getNode(10);
  Node* second = network.getNode(11);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  Node temporary;
  temporary.setId(first->getId());
  temporary.setDbInst(first->getDbInst());
  temporary.setMaster(network.getMaster(1));
  temporary.setType(Node::CELL);
  temporary.setWidth(DbuX{2 * kSiteWidth});
  temporary.setHeight(DbuY{2 * kRowHeight});
  temporary.setLeft(DbuX{2 * kSiteWidth});
  temporary.setBottom(DbuY{0});
  temporary.setOrient(PhysOrientationE::R0);
  CheckRequestOverlay request{&temporary,
                              GridX{2},
                              GridY{0},
                              PhysOrientationE::R0,
                              {deleteRecord(*second), deleteRecord(*first)}};
  const MasterId firstMaster = first->getMaster()->getId();
  const MasterId secondMaster = second->getMaster()->getId();

  const fillerRepair::RepairOutcome outcome = engine.repair(request);

  EXPECT_TRUE(outcome.hasSolution);
  EXPECT_TRUE(outcome.changes.empty());
  EXPECT_EQ(first->getMaster()->getId(), firstMaster);
  EXPECT_EQ(second->getMaster()->getId(), secondMaster);
}

}  // namespace
}  // namespace dpl2::ipl
