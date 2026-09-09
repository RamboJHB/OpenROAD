// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace dpl::filler_internal {

struct SiteGrid
{
  int row_count = 0;
  int column_count = 0;
  // Row-major. A non-zero entry is a legal, vacant site to fill.
  std::vector<uint8_t> fillable;
};

struct FillerFootprint
{
  int master_index = -1;
  int width_sites = 0;
  int height_rows = 0;
  // Optional row-major origin mask with the same dimensions as SiteGrid.
  std::vector<uint8_t> allowed_origins;
};

struct TiledFiller
{
  int master_index = -1;
  int row = 0;
  int column = 0;

  bool operator==(const TiledFiller& other) const
  {
    return master_index == other.master_index && row == other.row
           && column == other.column;
  }
};

struct PlannerConfig
{
  bool fit_space = true;
  std::size_t max_search_states = 250000;
  std::size_t max_search_depth = 4096;
  // Widths are in sites. Pairs are directional.
  std::set<std::pair<int, int>> forbidden_width_abutments;
};

enum class PlannerStatus
{
  complete,
  partial,
  impossible,
  budget_exceeded,
  invalid_input
};

struct PlannerResult
{
  PlannerStatus status = PlannerStatus::invalid_input;
  std::vector<TiledFiller> fillers;
  std::size_t fillable_sites = 0;
  std::size_t covered_sites = 0;
  std::size_t search_states = 0;
};

// Footprint order is authoritative candidate order. The planner first tries a
// linear deterministic packing. Exact requests that need backtracking use a
// bounded search and therefore fail without a partial plan when the budget is
// exhausted.
PlannerResult planFillers(const SiteGrid& grid,
                          const std::vector<FillerFootprint>& footprints,
                          const PlannerConfig& config = {});

}  // namespace dpl::filler_internal
