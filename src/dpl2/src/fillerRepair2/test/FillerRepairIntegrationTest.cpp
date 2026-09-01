#include <drc/ImplantLayerChecker.h>
#include <drc/ImplantLayerCheckerHelper.h>
#include <fillerRepair/FillerRepairEngine.h>
#include <infrastructure/Grid.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace dpl2::ipl {
namespace {

constexpr Dbu kSiteWidth = 10;
constexpr Dbu kRowHeight = 100;
constexpr Dbu kMinRule = 20;
constexpr RowId kRowCount = 5;
constexpr ColId kColCount = 20;
constexpr int kDefaultUtilization = 90;

constexpr InstanceId nodeId(RowId row, ColId col)
{
  return row * kColCount + col;
}

constexpr ColId occupiedColumns(int utilization)
{
  return kColCount * utilization / 100;
}

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
        || left[index].x_ != right[index].x_
        || left[index].y_ != right[index].y_
        || left[index].orig_lib_cell_ != right[index].orig_lib_cell_
        || left[index].new_lib_cell_ != right[index].new_lib_cell_
        || left[index].orientation_ != right[index].orientation_) {
      return false;
    }
  }
  return true;
}

struct FixtureStats
{
  int occupiedSites = 0;
  std::vector<int> cellsByRow;
};

FixtureStats fixtureStats(const ImplantInput& input)
{
  FixtureStats stats;
  stats.cellsByRow.resize(static_cast<size_t>(input.rowCount));
  std::vector<std::vector<bool>> occupied(
      static_cast<size_t>(input.rowCount),
      std::vector<bool>(static_cast<size_t>(input.colCount), false));
  for (const PlacedInst& placed : input.placedInsts) {
    const MasterItem& item
        = input.masters.at(static_cast<size_t>(placed.masterId));
    const int widthInSites
        = std::max(1, (item.width + input.siteWidth - 1) / input.siteWidth);
    const int heightInRows
        = std::max(1, (item.height + input.rowHeight - 1) / input.rowHeight);
    for (int row = placed.rowId; row < placed.rowId + heightInRows; ++row) {
      if (row < 0 || row >= input.rowCount) {
        continue;
      }
      ++stats.cellsByRow[static_cast<size_t>(row)];
      for (int col = placed.colId; col < placed.colId + widthInSites; ++col) {
        if (col >= 0 && col < input.colCount) {
          occupied[static_cast<size_t>(row)][static_cast<size_t>(col)] = true;
        }
      }
    }
  }
  for (const std::vector<bool>& row : occupied) {
    stats.occupiedSites
        += static_cast<int>(std::count(row.begin(), row.end(), true));
  }
  return stats;
}

void initializeFixture(ImplantLayerCheckerHelper& helper,
                       const ImplantInput& input,
                       int expectedUtilization)
{
  EXPECT_GE(input.rowCount, 5);
  const FixtureStats stats = fixtureStats(input);
  EXPECT_EQ(stats.cellsByRow.size(), static_cast<size_t>(input.rowCount));
  for (size_t row = 0; row < stats.cellsByRow.size(); ++row) {
    EXPECT_GE(stats.cellsByRow[row], 6) << "row " << row;
  }
  EXPECT_EQ(100 * stats.occupiedSites,
            expectedUtilization * input.rowCount * input.colCount);
  helper.initialize(input);
}

ImplantInput input(int utilization = kDefaultUtilization)
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
  const ColId cellCount = occupiedColumns(utilization);
  for (RowId row = 0; row < kRowCount; ++row) {
    for (ColId col = 0; col < cellCount; ++col) {
      const MasterId masterId
          = pattern[static_cast<size_t>(col % pattern.size())];
      data.placedInsts.push_back(
          {nodeId(row, col),
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

ImplantInput multiDeleteInput(int utilization = kDefaultUtilization)
{
  ImplantInput data = input(utilization);
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

ImplantInput nonExactInput(int utilization,
                           bool includeUnitWidthFillers = true)
{
  ImplantInput data = input(utilization);
  data.rules.clear();  // isolate layout-transaction behavior from implant DRC
  data.masters.push_back(master(6, 0, true, 2 * kSiteWidth));
  data.masters.push_back(master(7, 0, false, 2 * kSiteWidth));
  data.placedInsts.erase(
      std::remove_if(data.placedInsts.begin(),
                     data.placedInsts.end(),
                     [](const PlacedInst& placed) {
                       return placed.rowId == 1
                              && (placed.colId == 3 || placed.colId == 4);
                     }),
      data.placedInsts.end());
  data.placedInsts.push_back(
      {nodeId(1, 3), 6, 1, 3, PhysOrientationE::MX, true});
  data.fillerSetting.fillerMasterIds
      = includeUnitWidthFillers ? std::vector<MasterId>{3, 4, 5, 6}
                                : std::vector<MasterId>{6};
  if (!includeUnitWidthFillers) {
    for (MasterId id = 3; id <= 5; ++id) {
      data.masters[static_cast<size_t>(id)].isFiller = false;
    }
    for (PlacedInst& placed : data.placedInsts) {
      if (placed.masterId >= 3 && placed.masterId <= 5) {
        placed.isFiller = false;
      }
    }
  }
  return data;
}

ImplantInput twoRowMultiDeleteInput(int utilization = kDefaultUtilization)
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
                  twoRowMaster(1, false, 2 * kSiteWidth),
                  master(2, 0, false)};
  data.placedInsts = {{10, 0, 0, 2, PhysOrientationE::R0, true},
                      {11, 0, 0, 3, PhysOrientationE::R0, true}};
  InstanceId instance = 100;
  for (RowId row = 0; row < kRowCount; ++row) {
    for (ColId col = 0; col < occupiedColumns(utilization); ++col) {
      if (row <= 1 && (col == 2 || col == 3)) {
        continue;
      }
      data.placedInsts.push_back(
          {instance++,
           2,
           row,
           col,
           row % 2 == 0 ? PhysOrientationE::R0 : PhysOrientationE::MX,
           false});
    }
  }
  data.rowCount = kRowCount;
  data.colCount = kColCount;
  data.basePolar = Layer::Polar::N;
  data.rowHeight = kRowHeight;
  data.siteWidth = kSiteWidth;
  data.fillerSetting.present = true;
  data.fillerSetting.fillerMasterIds = {0};
  return data;
}

struct OverlayProbeCase
{
  std::string name;
  int utilization;
  bool multiDelete;
  std::vector<InstanceId> overlayIds;
  MasterId targetMasterId;
  bool accepted;
  std::string diagnostic;
};

const std::vector<OverlayProbeCase>& overlayProbeCases()
{
  static const std::vector<OverlayProbeCase> cases{
      {"std_to_std_50_percent",
       50,
       false,
       {nodeId(1, 4)},
       0,
       true,
       ""},
      {"std_to_std_75_percent",
       75,
       false,
       {nodeId(1, 4)},
       0,
       true,
       ""},
      {"std_to_std_90_percent",
       90,
       false,
       {nodeId(1, 4)},
       0,
       true,
       ""},
      {"single_filler_to_std",
       90,
       false,
       {nodeId(1, 3)},
       1,
       true,
       ""},
      {"multiple_fillers_to_std",
       90,
       true,
       {nodeId(1, 3), nodeId(1, 4)},
       6,
       true,
       ""},
      {"std_to_filler_master",
       90,
       false,
       {nodeId(1, 4)},
       3,
       false,
       "target_master_is_filler"},
      {"mixed_std_and_filler_overlays",
       90,
       true,
       {nodeId(1, 3), nodeId(1, 6)},
       6,
       false,
       "mixed_target_overlay"}};
  return cases;
}

class FillerRepairOverlayProbeTest
    : public ::testing::TestWithParam<OverlayProbeCase>
{
};

TEST_P(FillerRepairOverlayProbeTest, BuildsTemporaryNodeAndCallsChecker)
{
  const OverlayProbeCase& testCase = GetParam();
  SCOPED_TRACE(testCase.name);

  ImplantLayerCheckerHelper helper;
  const ImplantInput fixture = testCase.multiDelete
                                   ? multiDeleteInput(testCase.utilization)
                                   : input(testCase.utilization);
  initializeFixture(helper, fixture, testCase.utilization);
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);
  checker.setFillerRepairEnabled(true);

  Grid& grid = *helper.getGrid();
  Network& network = *helper.getNetwork();
  Master* targetMaster = network.getMaster(testCase.targetMasterId);
  ASSERT_NE(targetMaster, nullptr);
  const auto targetItem
      = checker.getMasterItems().find(testCase.targetMasterId);
  ASSERT_NE(targetItem, checker.getMasterItems().end());

  std::vector<Node*> overlays;
  int left = std::numeric_limits<int>::max();
  int bottom = std::numeric_limits<int>::max();
  for (const InstanceId id : testCase.overlayIds) {
    Node* node = network.getNode(id);
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->getMaster(), nullptr);
    overlays.push_back(node);
    left = std::min(left, node->getLeft().v);
    bottom = std::min(bottom, node->getBottom().v);
  }
  ASSERT_FALSE(overlays.empty());

  const GridX x = grid.gridX(DbuX{left});
  const GridY y = grid.gridSnapDownY(DbuY{bottom});
  const Pixel* origin = grid.gridPixel(x, y);
  ASSERT_NE(origin, nullptr);
  ASSERT_NE(origin->cell, nullptr);

  Node temporary;
  temporary.setId(origin->cell->getId());
  temporary.setDbInst(origin->cell->getDbInst());
  temporary.setMaster(targetMaster);
  temporary.setType(targetMaster->isFiller() ? Node::FILLER : Node::CELL);
  temporary.setWidth(DbuX{targetItem->second.width});
  temporary.setHeight(DbuY{targetItem->second.height});
  temporary.setLeft(DbuX{left});
  temporary.setBottom(DbuY{bottom});
  temporary.setOrient(overlays.front()->getOrient());
  temporary.setFixed(false);
  temporary.setPlaced(false);

  std::vector<CellChangeRecord> overlayChanges;
  std::vector<MasterId> originalMasters;
  overlayChanges.reserve(overlays.size());
  originalMasters.reserve(overlays.size());
  for (const Node* node : overlays) {
    overlayChanges.push_back(deleteRecord(*node));
    originalMasters.push_back(node->getMaster()->getId());
  }

  const CheckRequest request{
      &temporary, x, y, temporary.getOrient(), overlayChanges};
  const CheckResult direct = checker.checkDirect(request);
  if (!testCase.diagnostic.empty()) {
    EXPECT_TRUE(hasDiagnostic(direct, testCase.diagnostic));
  }

  FillerChanges changes;
  EXPECT_EQ(
      checker.check(
          &temporary, x, y, temporary.getOrient(), changes, overlayChanges),
      testCase.accepted);
  for (size_t index = 0; index < overlays.size(); ++index) {
    EXPECT_EQ(overlays[index]->getMaster()->getId(), originalMasters[index]);
  }
}

INSTANTIATE_TEST_SUITE_P(
    OverlayKinds,
    FillerRepairOverlayProbeTest,
    ::testing::ValuesIn(overlayProbeCases()),
    [](const ::testing::TestParamInfo<OverlayProbeCase>& info) {
      return info.param.name;
    });

class FillerRepairIntegrationTest : public ::testing::TestWithParam<int>
{
};

TEST_P(FillerRepairIntegrationTest,
       CheckerRepairsTemporaryNodeWithoutMutation)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, input(utilization), utilization);
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

TEST_P(FillerRepairIntegrationTest,
       RequestCoordinatesOverrideTemporaryNodePlacement)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, input(utilization), utilization);
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);

  constexpr RowId targetRow = 1;
  constexpr ColId targetCol = 4;
  Network& network = *helper.getNetwork();
  Node* replaced = network.getNode(nodeId(targetRow, targetCol));
  ASSERT_NE(replaced, nullptr);
  ASSERT_NE(replaced->getMaster(), nullptr);
  const MasterId originalMaster = replaced->getMaster()->getId();

  Node temporary;
  temporary.setId(replaced->getId());
  temporary.setDbInst(replaced->getDbInst());
  temporary.setMaster(replaced->getMaster());
  temporary.setType(Node::CELL);
  temporary.setWidth(replaced->getWidth());
  temporary.setHeight(replaced->getHeight());
  temporary.setLeft(DbuX{-1000});
  temporary.setBottom(DbuY{-1000});
  temporary.setOrient(PhysOrientationE::R0);

  const CheckRequest request{&temporary,
                             GridX{targetCol},
                             GridY{targetRow},
                             replaced->getOrient(),
                             {deleteRecord(*replaced)}};
  const CheckResult result = checker.checkDirect(request);

  EXPECT_TRUE(result.isLegal);
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_EQ(replaced->getMaster()->getId(), originalMaster);
}

TEST_P(FillerRepairIntegrationTest,
       MultipleFillerDeletesExactCoverAndNeverBecomeRepairCandidates)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, multiDeleteInput(utilization), utilization);
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

TEST_P(FillerRepairIntegrationTest,
       MultipleFillerDeletesValidateCoverageAndIntersection)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, multiDeleteInput(utilization), utilization);
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

  CheckRequest request{&temporary,
                              GridX{targetCol},
                              GridY{targetRow},
                              PhysOrientationE::MX,
                              {deleteRecord(*first)}};
  CheckResult result = checker.checkDirect(request);
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result, "target_overlaps_unchanged_instance"));

  request.overlayChanges = {deleteRecord(*first),
                            deleteRecord(*second),
                            deleteRecord(*outside)};
  result = checker.checkDirect(request);
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result, "target_overlay_does_not_intersect"));

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

TEST_P(FillerRepairIntegrationTest,
       NonExactSingleFillerAddsReleasedSiteWithoutDeleteOutput)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, nonExactInput(utilization), utilization);
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);
  checker.setFillerRepairEnabled(true);

  constexpr RowId targetRow = 1;
  constexpr ColId targetCol = 3;
  Network& network = *helper.getNetwork();
  Node* wideFiller = network.getNode(nodeId(targetRow, targetCol));
  ASSERT_NE(wideFiller, nullptr);
  ASSERT_TRUE(wideFiller->isFiller());
  ASSERT_EQ(wideFiller->getWidth().v, 2 * kSiteWidth);

  Node temporary;
  temporary.setId(wideFiller->getId());
  temporary.setDbInst(wideFiller->getDbInst());
  temporary.setMaster(network.getMaster(0));
  temporary.setType(Node::CELL);
  temporary.setWidth(DbuX{kSiteWidth});
  temporary.setHeight(DbuY{kRowHeight});
  temporary.setLeft(DbuX{targetCol * kSiteWidth});
  temporary.setBottom(DbuY{targetRow * kRowHeight});
  temporary.setOrient(PhysOrientationE::MX);
  std::vector<CellChangeRecord> overlay{deleteRecord(*wideFiller)};
  const CheckRequest request{&temporary,
                             GridX{targetCol},
                             GridY{targetRow},
                             PhysOrientationE::MX,
                             overlay};

  // With no implant rules the target-only snapshot is legal. Non-exact
  // geometry must still dispatch repair instead of returning early.
  EXPECT_TRUE(checker.checkDirect(request).isLegal);
  const MasterId originalMaster = wideFiller->getMaster()->getId();
  FillerChanges changes;
  EXPECT_TRUE(checker.check(&temporary,
                            GridX{targetCol},
                            GridY{targetRow},
                            PhysOrientationE::MX,
                            changes,
                            overlay));

  ASSERT_EQ(changes.size(), 1U);
  const CellChangeRecord& addition = changes.front();
  EXPECT_EQ(addition.op_, OpType::Add);
  const std::string* name = std::get_if<std::string>(&addition.cell_data_);
  ASSERT_NE(name, nullptr);
  EXPECT_FALSE(name->empty());
  EXPECT_EQ(addition.x_.getStorage(), 4 * kSiteWidth);
  EXPECT_EQ(addition.y_.getStorage(), targetRow * kRowHeight);
  EXPECT_TRUE(network.getMaster(
                  network.getMasterId(addition.new_lib_cell_))->isFiller());
  EXPECT_EQ(wideFiller->getMaster()->getId(), originalMaster);
  EXPECT_TRUE(std::none_of(changes.begin(), changes.end(), [](const auto& item) {
    return item.op_ == OpType::Delete;
  }));
}

TEST_P(FillerRepairIntegrationTest,
       NonExactMultipleFillersAddOnlyTheReleasedDifference)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, nonExactInput(utilization), utilization);
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);
  checker.setFillerRepairEnabled(true);

  constexpr RowId targetRow = 1;
  Network& network = *helper.getNetwork();
  Node* wideFiller = network.getNode(nodeId(targetRow, 3));
  Node* rightFiller = network.getNode(nodeId(targetRow, 5));
  ASSERT_NE(wideFiller, nullptr);
  ASSERT_NE(rightFiller, nullptr);
  ASSERT_TRUE(wideFiller->isFiller());
  ASSERT_TRUE(rightFiller->isFiller());

  Node temporary;
  temporary.setId(wideFiller->getId());
  temporary.setDbInst(wideFiller->getDbInst());
  temporary.setMaster(network.getMaster(7));
  temporary.setType(Node::CELL);
  temporary.setWidth(DbuX{2 * kSiteWidth});
  temporary.setHeight(DbuY{kRowHeight});
  temporary.setLeft(DbuX{4 * kSiteWidth});
  temporary.setBottom(DbuY{targetRow * kRowHeight});
  temporary.setOrient(PhysOrientationE::MX);
  std::vector<CellChangeRecord> overlay{deleteRecord(*rightFiller),
                                        deleteRecord(*wideFiller)};
  FillerChanges changes;

  ASSERT_TRUE(checker.check(&temporary,
                            GridX{4},
                            GridY{targetRow},
                            PhysOrientationE::MX,
                            changes,
                            overlay));
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes.front().op_, OpType::Add);
  EXPECT_EQ(changes.front().x_.getStorage(), 3 * kSiteWidth);
  EXPECT_EQ(changes.front().y_.getStorage(), targetRow * kRowHeight);
}

TEST_P(FillerRepairIntegrationTest,
       NonExactUnfillableReleasedSiteReturnsNoSolution)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(
      helper, nonExactInput(utilization, false), utilization);
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);
  checker.setFillerRepairEnabled(true);

  constexpr RowId targetRow = 1;
  constexpr ColId targetCol = 3;
  Network& network = *helper.getNetwork();
  Node* wideFiller = network.getNode(nodeId(targetRow, targetCol));
  ASSERT_NE(wideFiller, nullptr);
  Node temporary;
  temporary.setId(wideFiller->getId());
  temporary.setDbInst(wideFiller->getDbInst());
  temporary.setMaster(network.getMaster(0));
  temporary.setType(Node::CELL);
  temporary.setWidth(DbuX{kSiteWidth});
  temporary.setHeight(DbuY{kRowHeight});
  temporary.setLeft(DbuX{targetCol * kSiteWidth});
  temporary.setBottom(DbuY{targetRow * kRowHeight});
  temporary.setOrient(PhysOrientationE::MX);
  std::vector<CellChangeRecord> overlay{deleteRecord(*wideFiller)};
  FillerChanges changes;

  EXPECT_FALSE(checker.check(&temporary,
                             GridX{targetCol},
                             GridY{targetRow},
                             PhysOrientationE::MX,
                             changes,
                             overlay));
  EXPECT_TRUE(changes.empty());
}

TEST_P(FillerRepairIntegrationTest,
       TwoRowTargetAcceptsMultipleFillerExactCover)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, twoRowMultiDeleteInput(utilization), utilization);
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
  CheckRequest request{&temporary,
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

TEST_P(FillerRepairIntegrationTest, MalformedOverlayRecordsFailClosed)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, input(utilization), utilization);
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);

  constexpr RowId targetRow = 1;
  constexpr ColId targetCol = 4;
  Network& network = *helper.getNetwork();
  Node* replaced = network.getNode(targetRow * kColCount + targetCol);
  ASSERT_NE(replaced, nullptr);
  ASSERT_NE(replaced->getMaster(), nullptr);

  Node temporary;
  temporary.setId(replaced->getId());
  temporary.setDbInst(replaced->getDbInst());
  temporary.setMaster(network.getMaster(0));
  temporary.setType(Node::CELL);
  temporary.setWidth(replaced->getWidth());
  temporary.setHeight(replaced->getHeight());
  temporary.setLeft(replaced->getLeft());
  temporary.setBottom(replaced->getBottom());
  temporary.setOrient(replaced->getOrient());
  CheckRequest request{&temporary,
                              GridX{targetCol},
                              GridY{targetRow},
                              replaced->getOrient(),
                              {deleteRecord(*replaced)}};

  request.overlayChanges.clear();
  CheckResult result = checker.checkDirect(request);
  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result, "invalid_target_overlay_count"));

  CellChangeRecord malformed = deleteRecord(*replaced);
  malformed.op_ = OpType::Replace;
  request.overlayChanges = {malformed};
  result = checker.checkDirect(request);
  EXPECT_TRUE(hasDiagnostic(result, "invalid_target_overlay"));

  malformed = deleteRecord(*replaced);
  malformed.cell_data_ = std::string("not-a-leaf-cell");
  request.overlayChanges = {malformed};
  result = checker.checkDirect(request);
  EXPECT_TRUE(hasDiagnostic(result, "invalid_target_overlay"));

  malformed = deleteRecord(*replaced);
  malformed.cell_data_ = LeafCellID(0, 99999);
  request.overlayChanges = {malformed};
  result = checker.checkDirect(request);
  EXPECT_TRUE(hasDiagnostic(result, "unknown_target_overlay"));

  malformed = deleteRecord(*replaced);
  malformed.orig_lib_cell_ = network.getMaster(1)->getDbMaster();
  request.overlayChanges = {malformed};
  result = checker.checkDirect(request);
  EXPECT_TRUE(hasDiagnostic(result, "target_original_master_mismatch"));

  request.cell = nullptr;
  result = checker.checkDirect(request);
  EXPECT_TRUE(hasDiagnostic(result, "invalid_target_cell"));
  EXPECT_EQ(replaced->getMaster()->getId(), 2);
}

TEST_P(FillerRepairIntegrationTest,
       StandardCellReplacementRejectsMoveAndFootprintChange)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, multiDeleteInput(utilization), utilization);
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);

  constexpr RowId targetRow = 1;
  constexpr ColId targetCol = 6;
  Network& network = *helper.getNetwork();
  Node* replaced = network.getNode(targetRow * kColCount + targetCol);
  ASSERT_NE(replaced, nullptr);
  ASSERT_FALSE(replaced->isFiller());

  Node temporary;
  temporary.setId(replaced->getId());
  temporary.setDbInst(replaced->getDbInst());
  temporary.setType(Node::CELL);
  temporary.setLeft(replaced->getLeft());
  temporary.setBottom(replaced->getBottom());
  temporary.setOrient(replaced->getOrient());
  CheckRequest request{&temporary,
                              GridX{targetCol},
                              GridY{targetRow},
                              replaced->getOrient(),
                              {deleteRecord(*replaced)}};

  temporary.setMaster(network.getMaster(6));
  temporary.setWidth(DbuX{2 * kSiteWidth});
  temporary.setHeight(DbuY{kRowHeight});
  CheckResult result = checker.checkDirect(request);
  EXPECT_TRUE(hasDiagnostic(result, "target_footprint_mismatch"));

  temporary.setMaster(network.getMaster(0));
  temporary.setWidth(DbuX{kSiteWidth});
  request.x = GridX{targetCol + 1};
  result = checker.checkDirect(request);
  EXPECT_TRUE(hasDiagnostic(result, "target_move_unsupported"));
}

TEST_P(FillerRepairIntegrationTest,
       MultiFillerOverlayRejectsFillersUnrelatedToTarget)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, multiDeleteInput(utilization), utilization);
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);

  constexpr RowId targetRow = 1;
  Network& network = *helper.getNetwork();
  Node* first = network.getNode(targetRow * kColCount + 3);
  Node* second = network.getNode(targetRow * kColCount + 4);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  Node temporary;
  temporary.setId(first->getId());
  temporary.setDbInst(first->getDbInst());
  temporary.setMaster(network.getMaster(6));
  temporary.setType(Node::CELL);
  temporary.setWidth(DbuX{2 * kSiteWidth});
  temporary.setHeight(DbuY{kRowHeight});
  temporary.setLeft(DbuX{5 * kSiteWidth});
  temporary.setBottom(DbuY{targetRow * kRowHeight});
  temporary.setOrient(PhysOrientationE::MX);
  const CheckRequest request{&temporary,
                                    GridX{5},
                                    GridY{targetRow},
                                    PhysOrientationE::MX,
                                    {deleteRecord(*first),
                                     deleteRecord(*second)}};

  const CheckResult result = checker.checkDirect(request);

  EXPECT_FALSE(result.isLegal);
  EXPECT_TRUE(hasDiagnostic(result, "target_overlay_does_not_intersect"));
}

TEST_P(FillerRepairIntegrationTest, DisabledRepairDoesNotPublishChanges)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, input(utilization), utilization);
  ImplantLayerChecker checker(helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);
  ASSERT_FALSE(checker.isFillerRepairEnabled());

  constexpr RowId targetRow = 1;
  constexpr ColId targetCol = 4;
  Network& network = *helper.getNetwork();
  Node* replaced = network.getNode(targetRow * kColCount + targetCol);
  ASSERT_NE(replaced, nullptr);
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
  std::vector<CellChangeRecord> overlays{deleteRecord(*replaced)};
  FillerChanges changes;

  EXPECT_FALSE(checker.check(&temporary,
                             GridX{targetCol},
                             GridY{targetRow},
                             PhysOrientationE::MX,
                             changes,
                             overlays));
  EXPECT_TRUE(changes.empty());
}

TEST_P(FillerRepairIntegrationTest, EngineWithoutGridIsUnavailable)
{
  const int utilization = GetParam();
  ImplantLayerCheckerHelper helper;
  initializeFixture(helper, input(utilization), utilization);
  ImplantLayerChecker checker(nullptr, nullptr, helper.getNetwork());
  helper.initChecker(checker);

  const fillerRepair::FillerRepairEngine engine(checker);

  EXPECT_FALSE(engine.isReady());
}

INSTANTIATE_TEST_SUITE_P(
    PlacementUtilizations,
    FillerRepairIntegrationTest,
    ::testing::Values(50, 75, kDefaultUtilization),
    [](const ::testing::TestParamInfo<int>& info) {
      return "Utilization" + std::to_string(info.param);
    });

}  // namespace
}  // namespace dpl2::ipl
