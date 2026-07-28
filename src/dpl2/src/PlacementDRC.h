#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

#include <dpl2/DRCChecker.h>
#include <dpl2/DePlace.h>

using eUTL::PhysOrientation;

namespace dpl2 {
class Grid;
class Node;

class PlacementDRC
{
  public:
    explicit PlacementDRC(Grid* grid);
    ~PlacementDRC();

    void addChecker(DRCCheckerType type, std::unique_ptr<DRCChecker> checker);
    bool checkDRC(const Node* cell, std::vector<FillerCellRecord>& fcRecord) const;
    bool checkDRC(const Node* cell, GridX x, GridY y,
                  const eUNL::PhysOrientation& orient,
                  std::vector<FillerCellRecord>& fcRecord) const;

    DRCChecker* getChecker(DRCCheckerType type) const;

  private:
    Grid* grid_{nullptr};
    std::vector<std::unique_ptr<DRCChecker>> checkers_;
    std::unordered_map<DRCCheckerType, DRCChecker*> checker_map_;

};

}  // namespace dpl2