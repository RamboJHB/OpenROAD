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

  // [FRPORT] Registers a checker or replaces the existing checker of the same type
  // during single-threaded setup. Do not call while checks run.
  void addChecker(DRCCheckerType type, std::unique_ptr<DRCChecker> checker);
  // Runs every checker in direct-only mode without changing ccRecords.
  bool checkDRC(const Node* cell, std::vector<CellChangeRecord>& ccRecords) const;
  bool checkDRC(const Node* cell,
                GridX x,
                GridY y,
                const eUTL::PhysOrientation& orient,
                std::vector<CellChangeRecord>& ccRecords) const;

  // Runs every checker through its explicit overlay/repair-capable interface.
  // Repair records are published only if every checker accepts the request.
  bool checkDRC(const Node* cell,
                GridX x,
                GridY y,
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
