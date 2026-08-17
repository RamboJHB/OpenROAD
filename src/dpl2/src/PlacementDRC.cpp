#include <PlacementDRC.h>

#include <drc/util.h>
#include <infrastructure/Grid.h>
#include <infrastructure/network.h>

#include <uv3d/Uv3d.hh>

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
  std::cout << "[PlacementDRC::checkDRC] entry (1-arg overload)\n"
            << "  cell=" << cell
            << ", left=" << grid_->gridX(cell).v
            << ", bottom=" << grid_->gridRoundY(cell).v
            << ", orient=" << static_cast<uint>(cell->getOrient())
            << std::endl;
  return checkDRC(
      cell, grid_->gridX(cell), grid_->gridRoundY(cell),
      cell->getOrient(), ccRecords);
}

bool PlacementDRC::checkDRC(const Node* cell,
                            const GridX x,
                            const GridY y,
                            const eUNL::PhysOrientation& orient,
                            std::vector<CellChangeRecord>& ccRecords) const
{
  std::cout << "[PlacementDRC::checkDRC] entry (4-arg overload)\n"
            << "  cell=" << cell
            << ", x=" << x.v << ", y=" << y.v
            << ", orient=" << static_cast<uint>(orient)
            << " (" << checkers_.size() << " checker(s) registered)\n"
            << std::endl;
  // Full-checker path: run every registered checker with no separate overlay
  // list (empty overlayChanges passed for the two-list entry).
  std::vector<CellChangeRecord> overlayChanges;
  return checkDRC(cell, x, y, orient, ccRecords, overlayChanges);
}

bool PlacementDRC::checkDRC(const Node* cell,
                            const GridX x,
                            const GridY y,
                            const eUNL::PhysOrientation& orient,
                            std::vector<CellChangeRecord>& cellChanges,
                            std::vector<CellChangeRecord>& overlayChanges) const
{
  std::cout << "[PlacementDRC::checkDRC] entry (overlay overload, "
            << checkers_.size() << " checker(s) registered)\n"
            << "  cell=" << cell
            << ", x=" << x.v << ", y=" << y.v
            << ", orient=" << static_cast<uint>(orient)
            << std::endl;
  bool is_false = false;
  for (const auto& checker : checkers_) {
    // Every checker receives the two change lists and picks the one its rule
    // consumes: EdgeSpacing/Padding read overlayChanges (the std-cell/filler
    // cells the candidate footprint displaces) as the read-only overlay;
    // ImplantLayer (maintained by others) reads cellChanges.
    if (checker != nullptr
        && !checker->check(cell, x, y, orient, cellChanges, overlayChanges)) {
      is_false = true;
    }
    std::cout << "--- cellChanges (" << cellChanges.size() << " records) ---\n";
  }
  return !is_false;
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