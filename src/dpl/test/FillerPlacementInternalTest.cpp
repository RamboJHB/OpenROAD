// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <cstdint>
#include <vector>

#include "FillerPlacementInternal.h"
#include "gtest/gtest.h"

namespace dpl::filler_internal {
namespace {

SiteGrid fullGrid(int rows, int columns)
{
  return {rows,
          columns,
          std::vector<uint8_t>(static_cast<std::size_t>(rows) * columns, 1)};
}

TEST(FillerPlacementInternalTest, UsesMultiHeightMasterAsOneRectangle)
{
  const SiteGrid grid = fullGrid(2, 4);
  const std::vector<FillerFootprint> masters{{17, 2, 2, {}}, {5, 1, 1, {}}};

  const PlannerResult result = planFillers(grid, masters);

  EXPECT_EQ(result.status, PlannerStatus::complete);
  EXPECT_EQ(result.covered_sites, 8);
  EXPECT_EQ(result.fillers, (std::vector<TiledFiller>{{17, 0, 0}, {17, 0, 2}}));
}

TEST(FillerPlacementInternalTest, CandidateOrderIsAuthoritative)
{
  const SiteGrid grid = fullGrid(1, 2);

  const PlannerResult narrow_first
      = planFillers(grid, {{11, 1, 1, {}}, {22, 2, 1, {}}});
  const PlannerResult wide_first
      = planFillers(grid, {{22, 2, 1, {}}, {11, 1, 1, {}}});

  EXPECT_EQ(narrow_first.fillers,
            (std::vector<TiledFiller>{{11, 0, 0}, {11, 0, 1}}));
  EXPECT_EQ(wide_first.fillers, (std::vector<TiledFiller>{{22, 0, 0}}));
}

TEST(FillerPlacementInternalTest, LeavesOccupiedAndInvalidSitesUntouched)
{
  const SiteGrid grid{2, 3, {1, 1, 0, 1, 1, 0}};

  const PlannerResult result = planFillers(grid, {{3, 2, 2, {}}});

  EXPECT_EQ(result.status, PlannerStatus::complete);
  EXPECT_EQ(result.fillable_sites, 4);
  EXPECT_EQ(result.fillers, (std::vector<TiledFiller>{{3, 0, 0}}));
}

TEST(FillerPlacementInternalTest, ExactFailureReturnsNoPartialPlan)
{
  const SiteGrid grid = fullGrid(1, 3);

  const PlannerResult result = planFillers(grid, {{8, 2, 1, {}}});

  EXPECT_EQ(result.status, PlannerStatus::impossible);
  EXPECT_TRUE(result.fillers.empty());
  EXPECT_EQ(result.covered_sites, 0);
}

TEST(FillerPlacementInternalTest, PartialModeReturnsMaximalGreedyPacking)
{
  const SiteGrid grid = fullGrid(1, 3);
  PlannerConfig config;
  config.fit_space = false;

  const PlannerResult result = planFillers(grid, {{8, 2, 1, {}}}, config);

  EXPECT_EQ(result.status, PlannerStatus::partial);
  EXPECT_EQ(result.covered_sites, 2);
  EXPECT_EQ(result.fillers, (std::vector<TiledFiller>{{8, 0, 0}}));
}

TEST(FillerPlacementInternalTest, OriginMaskIsEnforced)
{
  const SiteGrid grid = fullGrid(2, 2);
  const std::vector<uint8_t> no_origins(4, 0);

  const PlannerResult result = planFillers(grid, {{4, 2, 2, no_origins}});

  EXPECT_EQ(result.status, PlannerStatus::impossible);
  EXPECT_TRUE(result.fillers.empty());
}

TEST(FillerPlacementInternalTest, BacktracksAfterGreedyDeadEnd)
{
  const SiteGrid grid = fullGrid(3, 3);

  const PlannerResult result
      = planFillers(grid, {{1, 2, 2, {}}, {2, 1, 3, {}}});

  EXPECT_EQ(result.status, PlannerStatus::complete);
  EXPECT_GT(result.search_states, 0);
  EXPECT_EQ(result.fillers,
            (std::vector<TiledFiller>{{2, 0, 0}, {2, 0, 1}, {2, 0, 2}}));
}

TEST(FillerPlacementInternalTest, SearchBudgetFailsClosed)
{
  const SiteGrid grid = fullGrid(3, 3);
  PlannerConfig config;
  config.max_search_states = 1;

  const PlannerResult result
      = planFillers(grid, {{1, 2, 2, {}}, {2, 1, 3, {}}}, config);

  EXPECT_EQ(result.status, PlannerStatus::budget_exceeded);
  EXPECT_TRUE(result.fillers.empty());
}

TEST(FillerPlacementInternalTest, ForbiddenMasterAbutmentChangesChoice)
{
  const SiteGrid grid = fullGrid(1, 4);
  PlannerConfig config;
  config.forbidden_master_abutments.insert({41, 41});

  const PlannerResult result
      = planFillers(grid, {{41, 2, 1, {}}, {17, 1, 1, {}}}, config);

  EXPECT_EQ(result.status, PlannerStatus::complete);
  EXPECT_EQ(result.fillers,
            (std::vector<TiledFiller>{{41, 0, 0}, {17, 0, 2}, {17, 0, 3}}));
}

TEST(FillerPlacementInternalTest, InvalidInputAndZeroBudgetFailClosed)
{
  const PlannerResult invalid = planFillers({2, 2, {1}}, {{1, 1, 1, {}}});
  EXPECT_EQ(invalid.status, PlannerStatus::invalid_input);

  PlannerConfig config;
  config.max_search_states = 0;
  const PlannerResult no_budget
      = planFillers(fullGrid(1, 1), {{1, 1, 1, {}}}, config);
  EXPECT_EQ(no_budget.status, PlannerStatus::budget_exceeded);
  EXPECT_TRUE(no_budget.fillers.empty());
}

TEST(FillerPlacementInternalTest, RepeatedCallsAreDeterministic)
{
  const SiteGrid grid = fullGrid(3, 3);
  const std::vector<FillerFootprint> masters{{1, 2, 2, {}}, {2, 1, 3, {}}};

  const PlannerResult first = planFillers(grid, masters);
  const PlannerResult second = planFillers(grid, masters);

  EXPECT_EQ(first.status, second.status);
  EXPECT_EQ(first.fillers, second.fillers);
  EXPECT_EQ(first.search_states, second.search_states);
}

}  // namespace
}  // namespace dpl::filler_internal
