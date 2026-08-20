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
  bool checkDRC(const Node* cell, std::vector<CellChangeRecord>& ccRecords) const;
  bool checkDRC(const Node* cell,
                GridX x,
                GridY y,
                const eUTL::PhysOrientation& orient,
                std::vector<CellChangeRecord>& ccRecords) const;

  // Checks one temporary node against one caller-supplied replacement overlay.
  // Repair records are published only if every registered checker accepts the
  // same request; a failed checker therefore cannot leak a partial repair.
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
