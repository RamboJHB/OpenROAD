// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Database-free rectangular tiler used by initial filler insertion.

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace dpl2::fillerInsertion::internal {

struct SiteGrid
{
  int rowCount = 0;
  int columnCount = 0;
  // Row-major. A non-zero entry is an empty legal site that should be filled.
  std::vector<uint8_t> fillable;
};

struct FillerFootprint
{
  int masterId = -1;
  int widthSites = 0;
  int heightRows = 0;
  // Optional row-major mask with the same dimensions as SiteGrid. A non-zero
  // entry permits this master to be anchored at that site.
  std::vector<uint8_t> allowedOrigins;
};

struct TiledFiller
{
  int masterId = -1;
  int rowId = 0;
  int columnId = 0;
};

struct PlannerConfig
{
  bool fitSpace = true;
  std::size_t maxSolutions = 64;
  std::size_t maxSearchStates = 250000;
  // Widths are in sites. Pairs are directional so callers may express either
  // side independently; fillerSetting supplies both directions.
  std::set<std::pair<int, int>> forbiddenWidthAbutments;
};

enum class PlannerStatus
{
  Complete,
  Partial,
  Impossible,
  BudgetExceeded,
  InvalidInput
};

struct PlannerResult
{
  PlannerStatus status = PlannerStatus::InvalidInput;
  std::vector<std::vector<TiledFiller>> solutions;
  std::size_t fillableSites = 0;
  std::size_t coveredSites = 0;
  std::size_t searchStates = 0;
};

// Footprint order is candidate order. The first complete solution is therefore
// stable and reflects fillerSetting::FollowOrder when the adapter requests it.
PlannerResult planFillers(const SiteGrid& grid,
                          std::vector<FillerFootprint> footprints,
                          const PlannerConfig& config = {});

}  // namespace dpl2::fillerInsertion::internal
