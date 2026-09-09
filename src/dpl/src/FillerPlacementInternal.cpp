// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "FillerPlacementInternal.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace dpl::filler_internal {
namespace {

class Planner
{
 public:
  Planner(const SiteGrid& grid,
          const std::vector<FillerFootprint>& footprints,
          const PlannerConfig& config)
      : grid_(grid),
        footprints_(footprints),
        config_(config),
        covered_(grid.fillable.size(), false),
        skipped_(grid.fillable.size(), false),
        owner_(grid.fillable.size(), -1)
  {
  }

  PlannerResult run()
  {
    result_.fillable_sites = static_cast<std::size_t>(std::count_if(
        grid_.fillable.begin(), grid_.fillable.end(), [](uint8_t value) {
          return value != 0;
        }));
    if (result_.fillable_sites == 0) {
      result_.status = PlannerStatus::complete;
      return result_;
    }
    if (config_.max_search_states == 0 || config_.max_search_depth == 0) {
      result_.status = PlannerStatus::budget_exceeded;
      return result_;
    }

    const GreedyResult greedy = greedyPlan();
    if (greedy.complete) {
      result_.status = PlannerStatus::complete;
      result_.fillers = materializePlacements();
      result_.covered_sites = greedy.covered_sites;
      return result_;
    }
    if (!config_.fit_space) {
      result_.status = greedy.covered_sites == 0 ? PlannerStatus::impossible
                                                 : PlannerStatus::partial;
      result_.fillers = materializePlacements();
      result_.covered_sites = greedy.covered_sites;
      return result_;
    }

    clearPlacement();
    search(/*covered_sites=*/0, /*depth=*/0);
    result_.search_states = search_states_;
    if (solution_found_) {
      result_.status = PlannerStatus::complete;
      result_.fillers = solution_;
      result_.covered_sites = result_.fillable_sites;
    } else {
      result_.status = budget_exceeded_ ? PlannerStatus::budget_exceeded
                                        : PlannerStatus::impossible;
    }
    return result_;
  }

 private:
  struct ActivePlacement
  {
    int footprint_index = -1;
    TiledFiller tile;
  };

  struct GreedyResult
  {
    bool complete = false;
    std::size_t covered_sites = 0;
  };

  std::size_t index(int row, int column) const
  {
    return static_cast<std::size_t>(row) * grid_.column_count + column;
  }

  bool originAllowed(const FillerFootprint& footprint, std::size_t anchor) const
  {
    return footprint.allowed_origins.empty()
           || footprint.allowed_origins[anchor] != 0;
  }

  bool forbidden(int left_master, int right_master) const
  {
    return config_.forbidden_master_abutments.count({left_master, right_master})
           != 0;
  }

  bool canPlace(int footprint_index,
                int row,
                int column,
                std::vector<std::size_t>& sites) const
  {
    const FillerFootprint& footprint = footprints_[footprint_index];
    if (row < 0 || column < 0 || row + footprint.height_rows > grid_.row_count
        || column + footprint.width_sites > grid_.column_count
        || !originAllowed(footprint, index(row, column))) {
      return false;
    }

    sites.clear();
    sites.reserve(static_cast<std::size_t>(footprint.width_sites)
                  * footprint.height_rows);
    for (int row_offset = 0; row_offset < footprint.height_rows; ++row_offset) {
      const int current_row = row + row_offset;
      if (column > 0) {
        const int left_owner = owner_[index(current_row, column - 1)];
        if (left_owner >= 0
            && forbidden(footprints_[placements_[left_owner].footprint_index]
                             .master_index,
                         footprint.master_index)) {
          return false;
        }
      }
      if (column + footprint.width_sites < grid_.column_count) {
        const int right_owner
            = owner_[index(current_row, column + footprint.width_sites)];
        if (right_owner >= 0
            && forbidden(footprint.master_index,
                         footprints_[placements_[right_owner].footprint_index]
                             .master_index)) {
          return false;
        }
      }
      for (int column_offset = 0; column_offset < footprint.width_sites;
           ++column_offset) {
        const std::size_t site = index(current_row, column + column_offset);
        if (grid_.fillable[site] == 0 || covered_[site] || skipped_[site]) {
          return false;
        }
        sites.push_back(site);
      }
    }
    return true;
  }

  void place(int footprint_index,
             int row,
             int column,
             const std::vector<std::size_t>& sites)
  {
    const int owner = static_cast<int>(placements_.size());
    placements_.push_back(
        {footprint_index,
         {footprints_[footprint_index].master_index, row, column}});
    for (const std::size_t site : sites) {
      covered_[site] = true;
      owner_[site] = owner;
    }
  }

  void unplace(const std::vector<std::size_t>& sites)
  {
    for (const std::size_t site : sites) {
      covered_[site] = false;
      owner_[site] = -1;
    }
    placements_.pop_back();
  }

  std::size_t firstUncovered() const
  {
    std::size_t first = 0;
    while (
        first < grid_.fillable.size()
        && (grid_.fillable[first] == 0 || covered_[first] || skipped_[first])) {
      ++first;
    }
    return first;
  }

  GreedyResult greedyPlan()
  {
    std::size_t covered_sites = 0;
    while (true) {
      const std::size_t first = firstUncovered();
      if (first == grid_.fillable.size()) {
        return {covered_sites == result_.fillable_sites, covered_sites};
      }
      const int row = static_cast<int>(first / grid_.column_count);
      const int column = static_cast<int>(first % grid_.column_count);
      bool placed = false;
      for (int footprint_index = 0;
           footprint_index < static_cast<int>(footprints_.size());
           ++footprint_index) {
        std::vector<std::size_t> sites;
        if (!canPlace(footprint_index, row, column, sites)) {
          continue;
        }
        covered_sites += sites.size();
        place(footprint_index, row, column, sites);
        placed = true;
        break;
      }
      if (!placed) {
        if (config_.fit_space) {
          return {false, covered_sites};
        }
        skipped_[first] = true;
      }
    }
  }

  void clearPlacement()
  {
    std::fill(covered_.begin(), covered_.end(), false);
    std::fill(skipped_.begin(), skipped_.end(), false);
    std::fill(owner_.begin(), owner_.end(), -1);
    placements_.clear();
  }

  std::vector<TiledFiller> materializePlacements() const
  {
    std::vector<TiledFiller> fillers;
    fillers.reserve(placements_.size());
    for (const ActivePlacement& placement : placements_) {
      fillers.push_back(placement.tile);
    }
    return fillers;
  }

  void search(std::size_t covered_sites, std::size_t depth)
  {
    if (solution_found_) {
      return;
    }
    if (search_states_ >= config_.max_search_states
        || depth >= config_.max_search_depth) {
      budget_exceeded_ = true;
      return;
    }
    ++search_states_;

    const std::size_t first = firstUncovered();
    if (first == grid_.fillable.size()) {
      if (covered_sites == result_.fillable_sites) {
        solution_ = materializePlacements();
        solution_found_ = true;
      }
      return;
    }

    const int row = static_cast<int>(first / grid_.column_count);
    const int column = static_cast<int>(first % grid_.column_count);
    for (int footprint_index = 0;
         footprint_index < static_cast<int>(footprints_.size());
         ++footprint_index) {
      std::vector<std::size_t> sites;
      if (!canPlace(footprint_index, row, column, sites)) {
        continue;
      }
      place(footprint_index, row, column, sites);
      search(covered_sites + sites.size(), depth + 1);
      unplace(sites);
      if (solution_found_ || budget_exceeded_) {
        return;
      }
    }
  }

  const SiteGrid& grid_;
  const std::vector<FillerFootprint>& footprints_;
  const PlannerConfig& config_;
  std::vector<bool> covered_;
  std::vector<bool> skipped_;
  std::vector<int> owner_;
  std::vector<ActivePlacement> placements_;
  std::vector<TiledFiller> solution_;
  PlannerResult result_;
  std::size_t search_states_ = 0;
  bool solution_found_ = false;
  bool budget_exceeded_ = false;
};

bool validInput(const SiteGrid& grid,
                const std::vector<FillerFootprint>& footprints)
{
  if (grid.row_count < 0 || grid.column_count < 0
      || (grid.row_count != 0
          && static_cast<std::size_t>(grid.column_count)
                 > std::numeric_limits<std::size_t>::max()
                       / static_cast<std::size_t>(grid.row_count))) {
    return false;
  }
  const std::size_t expected
      = static_cast<std::size_t>(grid.row_count) * grid.column_count;
  if (grid.fillable.size() != expected) {
    return false;
  }
  return std::all_of(
      footprints.begin(), footprints.end(), [expected](const auto& footprint) {
        return footprint.master_index >= 0 && footprint.width_sites > 0
               && footprint.height_rows > 0
               && (footprint.allowed_origins.empty()
                   || footprint.allowed_origins.size() == expected);
      });
}

}  // namespace

PlannerResult planFillers(const SiteGrid& grid,
                          const std::vector<FillerFootprint>& footprints,
                          const PlannerConfig& config)
{
  if (!validInput(grid, footprints)) {
    return {};
  }
  return Planner(grid, footprints, config).run();
}

}  // namespace dpl::filler_internal
