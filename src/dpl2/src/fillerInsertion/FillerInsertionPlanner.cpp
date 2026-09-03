// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerInsertion/FillerInsertionPlanner.h>

#include <algorithm>
#include <limits>

namespace dpl2::fillerInsertion::internal {
namespace {

class Search
{
 public:
  Search(const SiteGrid& grid,
         std::vector<FillerFootprint> footprints,
         const PlannerConfig& config)
      : grid_(grid),
        footprints_(std::move(footprints)),
        config_(config),
        covered_(grid.fillable.size(), false),
        skipped_(grid.fillable.size(), false),
        owner_(grid.fillable.size(), -1)
  {
  }

  PlannerResult run()
  {
    result_.fillableSites = static_cast<std::size_t>(std::count_if(
        grid_.fillable.begin(), grid_.fillable.end(), [](uint8_t value) {
          return value != 0;
        }));
    if (result_.fillableSites == 0) {
      result_.status = PlannerStatus::Complete;
      result_.solutions.emplace_back();
      return result_;
    }
    if (config_.maxSolutions == 0 || config_.maxSearchStates == 0) {
      result_.status = PlannerStatus::BudgetExceeded;
      return result_;
    }

    search(0);
    result_.searchStates = searchStates_;
    if (budgetExceeded_ && result_.solutions.empty()) {
      result_.status = PlannerStatus::BudgetExceeded;
      return result_;
    }
    if (!result_.solutions.empty()) {
      result_.status = PlannerStatus::Complete;
      result_.coveredSites = result_.fillableSites;
      return result_;
    }
    if (!config_.fitSpace && !budgetExceeded_ && bestCovered_ > 0) {
      result_.status = PlannerStatus::Partial;
      result_.coveredSites = bestCovered_;
      result_.solutions.push_back(best_);
      return result_;
    }
    result_.status = budgetExceeded_ ? PlannerStatus::BudgetExceeded
                                     : PlannerStatus::Impossible;
    return result_;
  }

 private:
  std::size_t index(int row, int column) const
  {
    return static_cast<std::size_t>(row) * grid_.columnCount + column;
  }

  bool originAllowed(const FillerFootprint& footprint, std::size_t anchor) const
  {
    return footprint.allowedOrigins.empty()
           || (footprint.allowedOrigins.size() == grid_.fillable.size()
               && footprint.allowedOrigins[anchor] != 0);
  }

  bool forbidden(int leftWidth, int rightWidth) const
  {
    return config_.forbiddenWidthAbutments.count({leftWidth, rightWidth}) != 0;
  }

  bool canPlace(const FillerFootprint& footprint,
                int row,
                int column,
                std::vector<std::size_t>& cells) const
  {
    if (row < 0 || column < 0 || footprint.widthSites <= 0
        || footprint.heightRows <= 0
        || row + footprint.heightRows > grid_.rowCount
        || column + footprint.widthSites > grid_.columnCount) {
      return false;
    }
    const std::size_t anchor = index(row, column);
    if (!originAllowed(footprint, anchor)) {
      return false;
    }

    cells.clear();
    cells.reserve(static_cast<std::size_t>(footprint.widthSites)
                  * footprint.heightRows);
    for (int rowOffset = 0; rowOffset < footprint.heightRows; ++rowOffset) {
      const int currentRow = row + rowOffset;
      if (column > 0) {
        const int adjacentOwner = owner_[index(currentRow, column - 1)];
        if (adjacentOwner >= 0
            && forbidden(footprints_[placements_[adjacentOwner].footprintIndex]
                             .widthSites,
                         footprint.widthSites)) {
          return false;
        }
      }
      if (column + footprint.widthSites < grid_.columnCount) {
        const int adjacentOwner
            = owner_[index(currentRow, column + footprint.widthSites)];
        if (adjacentOwner >= 0
            && forbidden(footprint.widthSites,
                         footprints_[placements_[adjacentOwner].footprintIndex]
                             .widthSites)) {
          return false;
        }
      }
      for (int columnOffset = 0; columnOffset < footprint.widthSites;
           ++columnOffset) {
        const std::size_t cell = index(currentRow, column + columnOffset);
        if (grid_.fillable[cell] == 0 || covered_[cell] || skipped_[cell]) {
          return false;
        }
        cells.push_back(cell);
      }
    }
    return true;
  }

  struct ActivePlacement
  {
    std::size_t footprintIndex = 0;
    TiledFiller tile;
  };

  void rememberPartial(std::size_t coveredCount)
  {
    if (coveredCount <= bestCovered_) {
      return;
    }
    bestCovered_ = coveredCount;
    best_.clear();
    best_.reserve(placements_.size());
    for (const ActivePlacement& placement : placements_) {
      best_.push_back(placement.tile);
    }
  }

  void search(std::size_t coveredCount)
  {
    if (result_.solutions.size() >= config_.maxSolutions) {
      return;
    }
    if (searchStates_ >= config_.maxSearchStates) {
      budgetExceeded_ = true;
      return;
    }
    ++searchStates_;
    if (!config_.fitSpace) {
      rememberPartial(coveredCount);
    }

    std::size_t first = 0;
    while (
        first < grid_.fillable.size()
        && (grid_.fillable[first] == 0 || covered_[first] || skipped_[first])) {
      ++first;
    }
    if (first == grid_.fillable.size()) {
      if (coveredCount == result_.fillableSites) {
        std::vector<TiledFiller> solution;
        solution.reserve(placements_.size());
        for (const ActivePlacement& placement : placements_) {
          solution.push_back(placement.tile);
        }
        result_.solutions.push_back(std::move(solution));
      }
      return;
    }

    const int row = static_cast<int>(first / grid_.columnCount);
    const int column = static_cast<int>(first % grid_.columnCount);
    for (std::size_t footprintIndex = 0; footprintIndex < footprints_.size();
         ++footprintIndex) {
      const FillerFootprint& footprint = footprints_[footprintIndex];
      std::vector<std::size_t> cells;
      if (!canPlace(footprint, row, column, cells)) {
        continue;
      }
      const int placementIndex = static_cast<int>(placements_.size());
      for (const std::size_t cell : cells) {
        covered_[cell] = true;
        owner_[cell] = placementIndex;
      }
      placements_.push_back(
          {footprintIndex, {footprint.masterId, row, column}});
      search(coveredCount + cells.size());
      placements_.pop_back();
      for (const std::size_t cell : cells) {
        covered_[cell] = false;
        owner_[cell] = -1;
      }
      if (result_.solutions.size() >= config_.maxSolutions || budgetExceeded_) {
        return;
      }
    }

    if (!config_.fitSpace) {
      skipped_[first] = true;
      search(coveredCount);
      skipped_[first] = false;
    }
  }

  const SiteGrid& grid_;
  std::vector<FillerFootprint> footprints_;
  const PlannerConfig& config_;
  std::vector<bool> covered_;
  std::vector<bool> skipped_;
  std::vector<int> owner_;
  std::vector<ActivePlacement> placements_;
  std::vector<TiledFiller> best_;
  PlannerResult result_;
  std::size_t bestCovered_ = 0;
  std::size_t searchStates_ = 0;
  bool budgetExceeded_ = false;
};

bool validInput(const SiteGrid& grid,
                const std::vector<FillerFootprint>& footprints)
{
  if (grid.rowCount < 0 || grid.columnCount < 0) {
    return false;
  }
  const auto expected = static_cast<std::size_t>(grid.rowCount)
                        * static_cast<std::size_t>(grid.columnCount);
  if (grid.fillable.size() != expected) {
    return false;
  }
  return std::all_of(
      footprints.begin(), footprints.end(), [expected](const auto& footprint) {
        return footprint.masterId >= 0 && footprint.widthSites > 0
               && footprint.heightRows > 0
               && (footprint.allowedOrigins.empty()
                   || footprint.allowedOrigins.size() == expected);
      });
}

}  // namespace

PlannerResult planFillers(const SiteGrid& grid,
                          std::vector<FillerFootprint> footprints,
                          const PlannerConfig& config)
{
  if (!validInput(grid, footprints)) {
    return {};
  }
  return Search(grid, std::move(footprints), config).run();
}

}  // namespace dpl2::fillerInsertion::internal
