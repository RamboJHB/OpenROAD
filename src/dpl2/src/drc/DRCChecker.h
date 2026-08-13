#pragma once
#include <array>
#include <string>
#include <vector>
#include <Coordinates.h>
#include <dpl2/DePlace.h>

namespace dpl2 {
class Grid;
class Node;

enum class DRCCheckerType {
  EdgeSpacing,
  BlockedLayers,
  Padding,
  OneSiteGap,
  ImplantLayer,
  Count
};

constexpr std::array<const char*, static_cast<size_t>(DRCCheckerType::Count)> drc_checker_type_name{
    "edge_spacing",
    "blocked_layers",
    "padding",
    "one_site_gap",
    "implant_layer",
};

inline std::string toString(DRCCheckerType type)
{
  return std::string(drc_checker_type_name[static_cast<size_t>(type)]);
}

// Abstract base for a DRC rule checker.
// Subclasses encapsulate their own dependencies and check logic.
class DRCChecker
{
 public:
  virtual ~DRCChecker() = default;

  // Core checking interface -- pure virtual, overridden by each rule.
  virtual bool check(const Node* cell, GridX x, GridY y, const eUTL::PhysOrientation& orient) const = 0;

  // [FRPORT] [fillerRepair-fix] Repair-aware form: a checker that can fix the
  // candidate by editing fillers appends its records to `fcRecord` and
  // returns true. Declared here so ImplantLayerChecker's `override` is valid
  // and so callers holding a DRCChecker* reach the repair path. The default
  // ignores the record vector, so a checker without repair needs no change.
  virtual bool check(const Node* cell, GridX x, GridY y, const eUTL::PhysOrientation& orient,
                     std::vector<CellChangeRecord>& fcRecord) const
  {
    (void) fcRecord;
    return check(cell, x, y, orient);
  }

 protected:
  Grid* grid_;
  explicit DRCChecker(Grid* grid) : grid_(grid) {}
};

}  // namespace dpl2
