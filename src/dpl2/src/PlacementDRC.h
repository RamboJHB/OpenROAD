#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include <drc/DRCChecker.h>
#include <dpl2/DePlace.h>

namespace dpl2 {
class Grid;
class Node;

class PlacementDRC
{
 public:
  explicit PlacementDRC(Grid* grid);
  ~PlacementDRC();

  void addChecker(DRCCheckerType type, std::unique_ptr<DRCChecker> checker);
  bool checkDRC(const Node* cell,
                std::vector<CellChangeRecord>& fcRecord) const;
  bool checkDRC(const Node* cell,
                GridX x,
                GridY y,
                const eUTL::PhysOrientation& orient,
                std::vector<CellChangeRecord>& fcRecord) const;

  DRCChecker* getChecker(DRCCheckerType type) const;

 private:
  Grid* grid_{nullptr};
  bool owns_arena_ = false;
  std::vector<std::unique_ptr<DRCChecker>> checkers_;
  std::unordered_map<DRCCheckerType, DRCChecker*> checker_map_;
};

}  // namespace dpl2
