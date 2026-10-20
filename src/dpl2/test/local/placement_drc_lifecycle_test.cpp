#include <PlacementDRC.h>

#include <drc/DRCChecker.h>
#include <drc/util.h>
#include <gtest/gtest.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace dpl2 {
namespace {

TEST(PlacementDRCLifecycleTest, SharedArenaSurvivesAnotherOwnerDestruction)
{
  Grid grid;
  PlacementDRC first(&grid);
  tbb::task_arena* shared = DrcUtil::getArena();
  ASSERT_NE(shared, nullptr);
  {
    PlacementDRC second(&grid);
    EXPECT_EQ(DrcUtil::getArena(), shared);
  }
  ASSERT_EQ(DrcUtil::getArena(), shared);
  std::atomic<int> calls{0};
  DrcUtil::parallelFor(32UL, [&](unsigned long) { ++calls; });
  EXPECT_EQ(calls.load(), 32);
}

class RecordingChecker final : public DRCChecker
{
 public:
  RecordingChecker(Grid* grid, int& calls, bool result, bool appendChange)
      : DRCChecker(grid, nullptr),
        calls_(calls),
        result_(result),
        append_change_(appendChange)
  {
  }

  bool check(const Node*,
             GridX,
             GridY,
             const eUTL::PhysOrientation&) const override
  {
    ++calls_;
    return result_;
  }

  bool check(const Node*,
             GridX,
             GridY,
             const eUTL::PhysOrientation&,
             std::vector<CellChangeRecord>& cellChanges,
             std::vector<CellChangeRecord>&) const override
  {
    ++calls_;
    if (append_change_) {
      cellChanges.push_back(CellChangeRecord{});
    }
    return result_;
  }

 private:
  int& calls_;
  bool result_;
  bool append_change_;
};

class InterfaceChecker final : public DRCChecker
{
 public:
  InterfaceChecker(Grid* grid, int& directCalls, int& overlayCalls, bool producesChanges)
      : DRCChecker(grid, nullptr),
        direct_calls_(directCalls),
        overlay_calls_(overlayCalls),
        produces_changes_(producesChanges)
  {
  }

  bool check(const Node*, GridX, GridY,
             const eUTL::PhysOrientation&) const override
  {
    ++direct_calls_;
    return true;
  }

  bool check(const Node*, GridX, GridY, const eUTL::PhysOrientation&,
             std::vector<CellChangeRecord>& cellChanges,
             std::vector<CellChangeRecord>&) const override
  {
    ++overlay_calls_;
    if (produces_changes_) {
      cellChanges.push_back(CellChangeRecord{});
    }
    return true;
  }

 private:
  int& direct_calls_;
  int& overlay_calls_;
  bool produces_changes_;
};

TEST(PlacementDRCLifecycleTest, SelectsCheckerInterfacePerRequest)
{
  Grid grid;
  PlacementDRC drc(&grid);
  int firstDirect = 0;
  int firstOverlay = 0;
  int secondDirect = 0;
  int secondOverlay = 0;
  drc.addChecker(DRCCheckerType::Padding,
                 std::make_unique<InterfaceChecker>(
                     &grid, firstDirect, firstOverlay, false));
  drc.addChecker(DRCCheckerType::ImplantLayer,
                 std::make_unique<InterfaceChecker>(
                     &grid, secondDirect, secondOverlay, true));
  Node node;
  std::vector<CellChangeRecord> changes;

  EXPECT_TRUE(drc.checkDRC(&node, GridX{0}, GridY{0},
                           eUTL::PhysOrientationE::R0, changes));
  EXPECT_EQ(firstDirect, 1);
  EXPECT_EQ(secondDirect, 1);
  EXPECT_EQ(firstOverlay, 0);
  EXPECT_EQ(secondOverlay, 0);
  EXPECT_TRUE(changes.empty());

  std::vector<CellChangeRecord> overlay;
  EXPECT_TRUE(drc.checkDRC(&node, GridX{0}, GridY{0},
                           eUTL::PhysOrientationE::R0, changes, overlay));
  EXPECT_EQ(firstDirect, 1);
  EXPECT_EQ(secondDirect, 1);
  EXPECT_EQ(firstOverlay, 1);
  EXPECT_EQ(secondOverlay, 1);
  EXPECT_EQ(changes.size(), 1U);
}

TEST(PlacementDRCLifecycleTest, ReplacesCheckerWithoutRunningStaleInstance)
{
  Grid grid;
  PlacementDRC drc(&grid);
  int oldCalls = 0;
  int newCalls = 0;
  drc.addChecker(DRCCheckerType::ImplantLayer,
                 std::make_unique<RecordingChecker>(
                     &grid, oldCalls, true, false));
  auto replacement = std::make_unique<RecordingChecker>(
      &grid, newCalls, true, false);
  DRCChecker* const replacementAddress = replacement.get();

  drc.addChecker(DRCCheckerType::ImplantLayer, std::move(replacement));
  Node node;
  std::vector<CellChangeRecord> changes;
  std::vector<CellChangeRecord> overlay;

  EXPECT_TRUE(drc.checkDRC(&node,
                           GridX{0},
                           GridY{0},
                           eUTL::PhysOrientationE::R0,
                           changes,
                           overlay));
  EXPECT_EQ(oldCalls, 0);
  EXPECT_EQ(newCalls, 1);
  EXPECT_EQ(drc.getChecker(DRCCheckerType::ImplantLayer), replacementAddress);
}

TEST(PlacementDRCLifecycleTest, FailedCheckerDoesNotPublishTrialChanges)
{
  Grid grid;
  PlacementDRC drc(&grid);
  int firstCalls = 0;
  int secondCalls = 0;
  drc.addChecker(DRCCheckerType::Padding,
                 std::make_unique<RecordingChecker>(
                     &grid, firstCalls, true, false));
  drc.addChecker(DRCCheckerType::ImplantLayer,
                 std::make_unique<RecordingChecker>(
                     &grid, secondCalls, false, false));
  Node node;
  std::vector<CellChangeRecord> changes;
  std::vector<CellChangeRecord> overlay;

  EXPECT_FALSE(drc.checkDRC(&node,
                            GridX{0},
                            GridY{0},
                            eUTL::PhysOrientationE::R0,
                            changes,
                            overlay));
  EXPECT_EQ(firstCalls, 0);
  EXPECT_EQ(secondCalls, 1);
  EXPECT_TRUE(changes.empty());
}

class TransactionChecker final : public DRCChecker
{
 public:
  using Check = std::function<bool(std::vector<CellChangeRecord>&,
                                  std::vector<CellChangeRecord>&)>;
  TransactionChecker(Grid* grid, Check check)
      : DRCChecker(grid, nullptr), check_(std::move(check)) {}
  bool check(const Node*, GridX, GridY,
             const eUTL::PhysOrientation&) const override { return true; }
  bool check(const Node*, GridX, GridY, const eUTL::PhysOrientation&,
             std::vector<CellChangeRecord>& changes,
             std::vector<CellChangeRecord>& overlays) const override
  {
    return check_(changes, overlays);
  }
 private:
  Check check_;
};

CellChangeRecord namedAdd(const std::string& name)
{
  return {OpType::Add, name, eUTL::UvDist(0), eUTL::UvDist(0),
          eLIB::LibCellID(), eLIB::LibCellID(0, 1), eUTL::PhysOrientationE::R0};
}

TEST(PlacementDRCLifecycleTest, FinalFillerValidationDoesNotDependOnRegistrationOrder)
{
  for (bool producerFirst : {false, true}) {
    Grid grid;
    PlacementDRC drc(&grid);
    int validationCalls = 0;
    auto producer = std::make_unique<TransactionChecker>(&grid,
        [](auto& changes, auto&) {
          changes.push_back(namedAdd("ILLEGAL_FILLER"));
          return true;
        });
    auto validator = std::make_unique<TransactionChecker>(&grid,
        [&](auto& changes, auto&) {
          ++validationCalls;
          return changes.empty();
        });
    if (producerFirst) {
      drc.addChecker(DRCCheckerType::ImplantLayer, std::move(producer));
      drc.addChecker(DRCCheckerType::EdgeSpacing, std::move(validator));
    } else {
      drc.addChecker(DRCCheckerType::EdgeSpacing, std::move(validator));
      drc.addChecker(DRCCheckerType::ImplantLayer, std::move(producer));
    }
    Node target;
    std::vector<CellChangeRecord> changes;
    std::vector<CellChangeRecord> overlays;
    EXPECT_FALSE(drc.checkDRC(&target, GridX{0}, GridY{0},
        eUTL::PhysOrientationE::R0, changes, overlays));
    EXPECT_EQ(validationCalls, 1);
    EXPECT_TRUE(changes.empty());
  }
}

TEST(PlacementDRCLifecycleTest, ValidatorsSeeFinalCandidateAndOnlyRealDeletes)
{
  Grid grid;
  PlacementDRC drc(&grid);
  const auto prefix = namedAdd("PREFIX");
  const auto final = namedAdd("FINAL_FILLER");
  CellChangeRecord old;
  old.op_ = OpType::Delete;
  old.cell_data_ = eUNL::LeafCellID(0, 42);
  CellChangeRecord pendingSwap = old;
  pendingSwap.op_ = OpType::Replace;
  std::vector<CellChangeRecord> overlay{old, namedAdd("TENTATIVE"), pendingSwap};
  drc.addChecker(DRCCheckerType::Padding,
      std::make_unique<TransactionChecker>(&grid, [&](auto& changes, auto& deletes) {
        EXPECT_EQ(changes.size(), 1u);
        EXPECT_EQ(changes.front().cell_data_, final.cell_data_);
        EXPECT_EQ(deletes.size(), 1u);
        EXPECT_EQ(deletes.front().op_, OpType::Delete);
        EXPECT_EQ(deletes.front().cell_data_, old.cell_data_);
        return true;
      }));
  drc.addChecker(DRCCheckerType::ImplantLayer,
      std::make_unique<TransactionChecker>(&grid, [&](auto& changes, auto& raw) {
        EXPECT_TRUE(changes.empty());
        EXPECT_EQ(raw.size(), 3u);
        changes.push_back(final);
        return true;
      }));
  Node target;
  std::vector<CellChangeRecord> changes{prefix};
  ASSERT_TRUE(drc.checkDRC(&target, GridX{0}, GridY{0},
      eUTL::PhysOrientationE::R0, changes, overlay));
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes.front().cell_data_, prefix.cell_data_);
  EXPECT_EQ(changes.back().cell_data_, final.cell_data_);
  EXPECT_EQ(overlay.size(), 3u);
}

TEST(PlacementDRCLifecycleTest, ValidatorCannotMutateTheAcceptedTransaction)
{
  for (bool mutateOverlay : {false, true}) {
    Grid grid;
    PlacementDRC drc(&grid);
    drc.addChecker(DRCCheckerType::ImplantLayer,
        std::make_unique<TransactionChecker>(&grid, [](auto& changes, auto&) {
          changes.push_back(namedAdd("FINAL_FILLER"));
          return true;
        }));
    drc.addChecker(DRCCheckerType::Padding,
        std::make_unique<TransactionChecker>(&grid, [&](auto& changes, auto& overlay) {
          if (mutateOverlay) {
            overlay.clear();
          } else {
            changes.clear();
          }
          return true;
        }));
    Node target;
    CellChangeRecord old;
    old.op_ = OpType::Delete;
    old.cell_data_ = eUNL::LeafCellID(0, 42);
    std::vector<CellChangeRecord> overlay{old};
    const auto prefix = namedAdd("PREFIX");
    std::vector<CellChangeRecord> changes{prefix};
    EXPECT_FALSE(drc.checkDRC(&target, GridX{0}, GridY{0},
        eUTL::PhysOrientationE::R0, changes, overlay));
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes.front().cell_data_, prefix.cell_data_);
    EXPECT_EQ(overlay.size(), 1u);
  }
}

}  // namespace
}  // namespace dpl2
