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
  // Full-checker path: run every registered checker with no separate overlay
  // list (empty overlayChanges passed for the two-list entry).
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
  if (cell == nullptr) {
    return false;
  }
  std::vector<CellChangeRecord> trial = cellChanges;
  for (const auto& checker : checkers_) {
    // Every checker receives the two change lists and picks the one its rule
    // consumes: EdgeSpacing/Padding read overlayChanges (the std-cell/filler
    // cells the candidate footprint displaces) as the read-only overlay;
    // ImplantLayer (maintained by others) reads cellChanges.
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
  checker_map_[type] = checker.get();
  checkers_.push_back(std::move(checker));
}

DRCChecker* PlacementDRC::getChecker(const DRCCheckerType type) const
{
  return checker_map_.at(type);
}

}  // namespace dpl2
