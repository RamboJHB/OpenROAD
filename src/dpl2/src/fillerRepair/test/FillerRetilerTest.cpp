// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerRepair/FillerRetiler.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace dpl2::fillerRepair::internal {
namespace {

std::set<std::pair<RowId, int>> coveredSites(
    const std::vector<TiledFiller>& tiling,
    const std::vector<FillerFootprint>& footprints)
{
  std::set<std::pair<RowId, int>> sites;
  for (const TiledFiller& tile : tiling) {
    const auto footprint = std::find_if(
        footprints.begin(), footprints.end(), [&](const auto& item) {
          return item.masterId == tile.masterId;
        });
    EXPECT_NE(footprint, footprints.end());
    if (footprint == footprints.end()) {
      continue;
    }
    for (int row = 0; row < footprint->heightRows; ++row) {
      for (int col = 0; col < footprint->widthSites; ++col) {
        EXPECT_TRUE(sites.emplace(tile.rowId + row, tile.colId + col).second);
      }
    }
  }
  return sites;
}

TEST(FillerRetilerTest, PrefersDeterministicLargestExactCover)
{
  const std::vector<SiteCell> released{{0, 0}, {0, 1}, {1, 0}, {1, 1}};
  const std::vector<FillerFootprint> footprints{{10, 1, 1},
                                                 {11, 2, 1},
                                                 {12, 2, 2}};
  const RetileResult result = enumerateRetilings(released, footprints);

  ASSERT_FALSE(result.solutions.empty());
  ASSERT_EQ(result.solutions.front().size(), 1U);
  EXPECT_EQ(result.solutions.front().front().masterId, 12);
  EXPECT_EQ(coveredSites(result.solutions.front(), footprints),
            (std::set<std::pair<RowId, int>>{
                {0, 0}, {0, 1}, {1, 0}, {1, 1}}));
}

TEST(FillerRetilerTest, RejectsReleasedAreaThatCannotBeExactlyCovered)
{
  const RetileResult result
      = enumerateRetilings({{0, 0}, {0, 1}, {0, 2}}, {{7, 2, 1}});

  EXPECT_TRUE(result.solutions.empty());
  EXPECT_FALSE(result.truncated);
}

TEST(FillerRetilerTest, NeverOverlapsOrFillsOutsideReleasedSites)
{
  const std::vector<SiteCell> released{{2, 4}, {2, 5}, {2, 6}, {2, 7}};
  const std::vector<FillerFootprint> footprints{{3, 1, 1}, {4, 2, 1}};
  const RetileResult result = enumerateRetilings(released, footprints);
  const std::set<std::pair<RowId, int>> expected{
      {2, 4}, {2, 5}, {2, 6}, {2, 7}};

  ASSERT_FALSE(result.solutions.empty());
  for (const auto& tiling : result.solutions) {
    EXPECT_EQ(coveredSites(tiling, footprints), expected);
  }
}

}  // namespace
}  // namespace dpl2::fillerRepair::internal
