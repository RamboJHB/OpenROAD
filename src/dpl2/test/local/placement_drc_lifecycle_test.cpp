#include <PlacementDRC.h>

#include <drc/DRCChecker.h>
#include <gtest/gtest.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>

#include <memory>
#include <vector>

namespace dpl2 {
namespace {

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
  InterfaceChecker(Grid* grid, int& directCalls, int& overlayCalls)
      : DRCChecker(grid, nullptr),
        direct_calls_(directCalls),
        overlay_calls_(overlayCalls)
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
    cellChanges.push_back(CellChangeRecord{});
    return true;
  }

 private:
  int& direct_calls_;
  int& overlay_calls_;
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
                     &grid, firstDirect, firstOverlay));
  drc.addChecker(DRCCheckerType::ImplantLayer,
                 std::make_unique<InterfaceChecker>(
                     &grid, secondDirect, secondOverlay));
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
  EXPECT_EQ(changes.size(), 2U);
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
                     &grid, firstCalls, true, true));
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
  EXPECT_EQ(firstCalls, 1);
  EXPECT_EQ(secondCalls, 1);
  EXPECT_TRUE(changes.empty());
}

}  // namespace
}  // namespace dpl2
