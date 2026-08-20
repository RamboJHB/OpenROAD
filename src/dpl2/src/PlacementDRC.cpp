#include <PlacementDRC.h>

#include <drc/util.h>
#include <infrastructure/Grid.h>
#include <infrastructure/network.h>

namespace dpl2 {

PlacementDRC::PlacementDRC(Grid* grid) : grid_(grid)
{
  if (!DrcUtil::getArena()) {
    // todo: set global thread count
    // int threadCount = uv3d::Uv3d::uv3d()->getThreadCount();
    int threadCount = 32;
    std::cout << "available thread number: " << threadCount << std::endl;
    DrcUtil::setArena(new tbb::task_arena(threadCount));
  }
}

PlacementDRC::~PlacementDRC()
{
  if (DrcUtil::getArena()) {
    delete DrcUtil::getArena();
    DrcUtil::setArena(nullptr);
  }
}

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
                            std::vector<CellChangeRecord>& ccRecords) const
{
  std::vector<CellChangeRecord> overlayChanges;
  return checkDRC(cell, x, y, orient, ccRecords, overlayChanges);
}

bool PlacementDRC::checkDRC(const Node* cell,
                            const GridX x,
                            const GridY y,
                            const eUTL::PhysOrientation& orient,
                            std::vector<CellChangeRecord>& cellChanges,
                            std::vector<CellChangeRecord>& overlayChanges) const
{
  if (cell == nullptr || grid_ == nullptr) {
    return false;
  }

  std::vector<CellChangeRecord> trial = cellChanges;
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
