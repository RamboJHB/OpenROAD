#pragma once

#include <array>
#include <string>
#include <vector>

#include <dpl2/DePlace.h>
#include <Coordinates.h>
#include <Objects.h>

namespace dpl2 {
class Grid;
class Node;
struct CellChangeRecord;

enum class DRCCheckerType
{
  EdgeSpacing,
  BlockedLayers,
  Padding,
  OneSiteGap,
  ImplantLayer,
  FixedMask,
  Count
};

constexpr std::array<const char*, static_cast<size_t>(DRCCheckerType::Count)>
    drc_checker_type_name{
        "edge_spacing",
        "blocked_layers",
        "padding",
        "one_site_gap",
        "implant_layer",
        "fixed_mask",
    };

inline std::string toString(DRCCheckerType type)
{
  return std::string(
      drc_checker_type_name[static_cast<size_t>(type)]);
}

// Abstract base for a DRC rule checker.
// Subclasses encapsulate their own dependencies and check logic.
class DRCChecker
{
 public:
  virtual ~DRCChecker() = default;

  // Core checking interface — pure virtual, overridden by each rule.
  virtual bool check(const Node* cell,
                     GridX x,
                     GridY y,
                     const eUTL::PhysOrientation& orient) const = 0;

  // List-carrying variant.  Every checker receives two change lists:
  //    - cellChanges   : the caller's planned cell-change list (filler
  //                      modifications, consumed by ImplantLayer code
  //                      maintained by others);
  //    - overlayChanges: the std-cell/filler cells displaced by the candidate
  //                      footprint, treated as already removed by the
  //                      neighbour-reading checkers (EdgeSpacing/Padding).
  // A checker ignores whichever list is not part of its rule.
  virtual bool check(const Node* cell,
                     GridX x,
                     GridY y,
                     const eUTL::PhysOrientation& orient,
                     std::vector<CellChangeRecord>& cellChanges,
                     std::vector<CellChangeRecord>& overlayChanges) const = 0;

  // Returns the instance name of a cell via the design's LibObjAccessor.
  std::string cellName(const Node* cell) const
  {
    return design_->getHierMgr()->getLeafCell(cell->getDbInst()).getName();
  }

protected:
  Grid* grid_;
  eUNL::Design* design_;
  explicit DRCChecker(Grid* grid, eUNL::Design* design)
      : grid_(grid), design_(design) {}
};

}  // namespace dpl2