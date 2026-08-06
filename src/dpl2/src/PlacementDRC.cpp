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

bool PlacementDRC::checkDRC(const Node* cell, std::vector<CellChangeRecord>&
    fcRecord) const
{
  return checkDRC(
      cell, grid_->gridX(cell), grid_->gridRoundY(cell),
      cell->getOrient(), fcRecord);
}

bool PlacementDRC::checkDRC(const Node* cell,
                            const GridX x,
                            const GridY y,
                            const eUTL::PhysOrientation& orient,
                            std::vector<CellChangeRecord>& fcRecord) const
{
  for (auto& checker : checkers_) {
    if (!checker->check(cell, x, y, orient, fcRecord)) {
      return false;
    }
  }
  return true;
}

// =================== Checker registry ===================

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
