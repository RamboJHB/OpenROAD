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
  bool checkDRC(const Node* cell, std::vector<CellChangeRecord>& ccRecords) const;
  bool checkDRC(const Node* cell, GridX x, GridY y,
                const eUTL::PhysOrientation& orient,
                std::vector<CellChangeRecord>& ccRecords) const;
  // Read-only overlay variant: runs all registered checkers.  Every checker
  // consumes @p overlayChanges (the std-cell/filler cells the candidate
  // footprint displaces) as the read-only overlay, without mutating the
  // in-memory grid/network.  @p cellChanges carries the caller's planned filler
  // cell-change list; it is passed through for the ImplantLayer code maintained
  // by others.
  bool checkDRC(const Node* cell, GridX x, GridY y,
              const eUTL::PhysOrientation& orient,
              std::vector<CellChangeRecord>& cellChanges,
              std::vector<CellChangeRecord>& overlayChanges) const;

  DRCChecker* getChecker(DRCCheckerType type) const;

 private:
  Grid* grid_{nullptr};
  std::vector<std::unique_ptr<DRCChecker>> checkers_;
  std::unordered_map<DRCCheckerType, DRCChecker*> checker_map_;

};

}  // namespace dpl2
