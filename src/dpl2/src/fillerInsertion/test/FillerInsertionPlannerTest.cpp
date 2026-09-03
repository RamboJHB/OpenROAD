// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerInsertion/FillerInsertionPlanner.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace fi = dpl2::fillerInsertion::internal;

namespace {

fi::SiteGrid fullGrid(int rows, int columns)
{
  return {rows,
          columns,
          std::vector<uint8_t>(static_cast<std::size_t>(rows) * columns, 1)};
}

TEST(FillerInsertionPlanner, UsesMultiHeightMasterAsOneTile)
{
  const fi::PlannerResult result
      = fi::planFillers(fullGrid(2, 4), {{10, 2, 2, {}}, {11, 1, 1, {}}});
  ASSERT_EQ(result.status, fi::PlannerStatus::Complete);
  ASSERT_FALSE(result.solutions.empty());
  ASSERT_EQ(result.solutions.front().size(), 2U);
  EXPECT_EQ(result.solutions.front()[0].masterId, 10);
  EXPECT_EQ(result.solutions.front()[1].masterId, 10);
  EXPECT_EQ(result.coveredSites, 8U);
}

TEST(FillerInsertionPlanner, CandidateOrderIsAuthoritative)
{
  const fi::PlannerResult smallFirst
      = fi::planFillers(fullGrid(2, 2), {{1, 1, 1, {}}, {2, 2, 2, {}}});
  const fi::PlannerResult tallFirst
      = fi::planFillers(fullGrid(2, 2), {{2, 2, 2, {}}, {1, 1, 1, {}}});
  ASSERT_FALSE(smallFirst.solutions.empty());
  ASSERT_FALSE(tallFirst.solutions.empty());
  EXPECT_EQ(smallFirst.solutions.front().size(), 4U);
  EXPECT_EQ(tallFirst.solutions.front().size(), 1U);
  EXPECT_EQ(tallFirst.solutions.front().front().masterId, 2);
}

TEST(FillerInsertionPlanner, NeverCoversOccupiedOrInvalidHole)
{
  fi::SiteGrid grid = fullGrid(2, 3);
  grid.fillable[1] = 0;
  const fi::PlannerResult result
      = fi::planFillers(grid, {{7, 1, 1, {}}, {8, 2, 2, {}}});
  ASSERT_EQ(result.status, fi::PlannerStatus::Complete);
  ASSERT_FALSE(result.solutions.empty());
  EXPECT_EQ(result.solutions.front().size(), 5U);
  for (const fi::TiledFiller& tile : result.solutions.front()) {
    EXPECT_FALSE(tile.rowId == 0 && tile.columnId == 1);
  }
}

TEST(FillerInsertionPlanner, RejectsUntileableExactSpace)
{
  const fi::PlannerResult result
      = fi::planFillers(fullGrid(1, 3), {{1, 2, 1, {}}});
  EXPECT_EQ(result.status, fi::PlannerStatus::Impossible);
  EXPECT_TRUE(result.solutions.empty());
}

TEST(FillerInsertionPlanner, FitSpaceFalseReturnsMaximalPartialPacking)
{
  fi::PlannerConfig config;
  config.fitSpace = false;
  const fi::PlannerResult result
      = fi::planFillers(fullGrid(1, 3), {{1, 2, 1, {}}}, config);
  ASSERT_EQ(result.status, fi::PlannerStatus::Partial);
  ASSERT_EQ(result.solutions.size(), 1U);
  ASSERT_EQ(result.solutions.front().size(), 1U);
  EXPECT_EQ(result.coveredSites, 2U);
}

TEST(FillerInsertionPlanner, ForbiddenAbutmentRejectsOtherwiseExactTiling)
{
  fi::PlannerConfig config;
  config.forbiddenWidthAbutments.insert({1, 1});
  const fi::PlannerResult result
      = fi::planFillers(fullGrid(1, 2), {{1, 1, 1, {}}}, config);
  EXPECT_EQ(result.status, fi::PlannerStatus::Impossible);
  EXPECT_TRUE(result.solutions.empty());
}

TEST(FillerInsertionPlanner, PerMasterOriginMaskControlsHeightCompatibility)
{
  fi::FillerFootprint restricted{1, 2, 1, {0, 0, 1, 0}};
  const fi::PlannerResult result
      = fi::planFillers(fullGrid(2, 2), {restricted});
  EXPECT_EQ(result.status, fi::PlannerStatus::Impossible);
}

TEST(FillerInsertionPlanner, ZeroBudgetFailsWithoutPartialResult)
{
  fi::PlannerConfig config;
  config.fitSpace = false;
  config.maxSearchStates = 0;
  const fi::PlannerResult result
      = fi::planFillers(fullGrid(1, 3), {{1, 2, 1, {}}}, config);
  EXPECT_EQ(result.status, fi::PlannerStatus::BudgetExceeded);
  EXPECT_TRUE(result.solutions.empty());
}

TEST(FillerInsertionPlanner, InvalidMaskFailsClosed)
{
  const fi::PlannerResult result
      = fi::planFillers({2, 2, {1, 1, 1}}, {{1, 1, 1, {}}});
  EXPECT_EQ(result.status, fi::PlannerStatus::InvalidInput);
  EXPECT_TRUE(result.solutions.empty());
}

TEST(FillerInsertionPlanner, RepeatedCallsAreByteOrderDeterministic)
{
  const fi::SiteGrid grid = fullGrid(2, 5);
  const std::vector<fi::FillerFootprint> footprints{
      {3, 2, 2, {}}, {2, 2, 1, {}}, {1, 1, 1, {}}};
  const fi::PlannerResult first = fi::planFillers(grid, footprints);
  const fi::PlannerResult second = fi::planFillers(grid, footprints);
  ASSERT_EQ(first.solutions.size(), second.solutions.size());
  ASSERT_FALSE(first.solutions.empty());
  ASSERT_EQ(first.solutions.front().size(), second.solutions.front().size());
  for (std::size_t index = 0; index < first.solutions.front().size(); ++index) {
    EXPECT_EQ(first.solutions.front()[index].masterId,
              second.solutions.front()[index].masterId);
    EXPECT_EQ(first.solutions.front()[index].rowId,
              second.solutions.front()[index].rowId);
    EXPECT_EQ(first.solutions.front()[index].columnId,
              second.solutions.front()[index].columnId);
  }
}

}  // namespace
