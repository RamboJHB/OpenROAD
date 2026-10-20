#include <PlacementDRC.h>

#include <drc/util.h>
#include <infrastructure/Grid.h>
#include <infrastructure/network.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <tuple>

namespace dpl2 {

PlacementDRC::PlacementDRC(Grid* grid) : grid_(grid)
{
  // The shared arena outlives every PlacementDRC owner. Setup publishes it
  // before workers start; destroying one owner must not interrupt another.
  static tbb::task_arena arena(32);
  static std::once_flag published;
  std::call_once(published, [] {
    std::fputs("available thread number: 32\n", stdout);
    DrcUtil::setArena(&arena);
  });
}

PlacementDRC::~PlacementDRC() = default;

bool PlacementDRC::checkDRC(const Node* cell,
    std::vector<CellChangeRecord>& ccRecords) const
{
  if (cell == nullptr || grid_ == nullptr) {
    return false;
  }
  return checkDRC(
      cell, grid_->gridX(cell), grid_->gridRoundY(cell),
      cell->getOrient(), ccRecords);
}

bool PlacementDRC::checkDRC(const Node* cell,
                            const GridX x,
                            const GridY y,
                            const eUTL::PhysOrientation& orient,
                            std::vector<CellChangeRecord>&) const
{
  if (cell == nullptr || grid_ == nullptr) {
    return false;
  }
  for (const auto& checker : checkers_) {
    if (checker != nullptr && !checker->check(cell, x, y, orient)) {
      return false;
    }
  }
  return true;
}

bool PlacementDRC::checkDRC(const Node* cell,
                            const GridX x,
                            const GridY y,
                            const eUTL::PhysOrientation& orient,
                            std::vector<CellChangeRecord>& cellChanges,
                            std::vector<CellChangeRecord>& overlayChanges) const
{
  if (grid_ == nullptr) {
    return false;
  }

  const DRCChecker* implant = getChecker(DRCCheckerType::ImplantLayer);
  std::vector<CellChangeRecord> trial;
  if (cell == nullptr) {
    // Removal is tentative filler preparation. Every checker still runs on
    // the final non-null placement before its transaction is published.
    if (implant == nullptr
        || !implant->check(nullptr, x, y, orient, trial, overlayChanges)) {
      return false;
    }
    cellChanges.insert(cellChanges.end(), trial.begin(), trial.end());
    return true;
  }
  if (implant != nullptr) {
    // FR is the sole filler-transaction producer. Every other rule must see
    // the finalized layout, irrespective of checker registration order.
    if (!implant->check(cell, x, y, orient, trial, overlayChanges)) {
      return false;
    }
    std::vector<CellChangeRecord> removals;
    std::copy_if(overlayChanges.begin(), overlayChanges.end(),
                 std::back_inserter(removals), [](const auto& change) {
                   return change.op_ == OpType::Delete;
                 });
    const auto sameRecord = [](const auto& a, const auto& b) {
      return std::tie(a.op_, a.cell_data_, a.x_, a.y_, a.orig_lib_cell_,
                       a.new_lib_cell_, a.orientation_)
             == std::tie(b.op_, b.cell_data_, b.x_, b.y_, b.orig_lib_cell_,
                          b.new_lib_cell_, b.orientation_);
    };
    const auto unchanged = [&](const auto& a, const auto& b) {
      return a.size() == b.size()
             && std::equal(a.begin(), a.end(), b.begin(), sameRecord);
    };
    for (const auto& checker : checkers_) {
      if (checker == nullptr || checker.get() == implant) {
        continue;
      }
      auto verified = trial;
      auto input = removals;
      if (!checker->check(cell, x, y, orient, verified, input)) {
        return false;
      }
      if (!unchanged(verified, trial) || !unchanged(input, removals)) {
        std::fputs("[dpl2][drc] rejected: checker modified the finalized filler transaction\n", stdout);
        return false;
      }
    }
    cellChanges.insert(cellChanges.end(), trial.begin(), trial.end());
    return true;
  }
  trial = cellChanges;
  for (const auto& checker : checkers_) {
    if (checker != nullptr
        && !checker->check(cell, x, y, orient, trial, overlayChanges)) {
      return false;
    }
  }
  cellChanges = std::move(trial);
  return true;
}

// ==================== Checker registry ====================

void PlacementDRC::addChecker(const DRCCheckerType type,
                              std::unique_ptr<DRCChecker> checker)
{
  if (checker == nullptr) {
    return;
  }
  const auto found = checker_map_.find(type);
  if (found != checker_map_.end()) {
    for (std::unique_ptr<DRCChecker>& current : checkers_) {
      if (current.get() == found->second) {
        current = std::move(checker);
        found->second = current.get();
        return;
      }
    }
    checker_map_.erase(found);
  }
  checker_map_[type] = checker.get();
  checkers_.push_back(std::move(checker));
}

DRCChecker* PlacementDRC::getChecker(const DRCCheckerType type) const
{
  const auto found = checker_map_.find(type);
  return found != checker_map_.end() ? found->second : nullptr;
}

}  // namespace dpl2
