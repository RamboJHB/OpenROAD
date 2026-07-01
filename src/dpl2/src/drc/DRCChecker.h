#pragma once

#include <array>
#include <string>
#include <dpl2/DePlace.h>

namespace dpl2 {
class Grid;
class Node;

enum class DRCCheckerType
{
  EdgeSpacing,
  BlockedLayers,
  Padding,
  OneSiteGap,
  Count
};

constexpr std::array<const char*, static_cast<size_t>(DRCCheckerType::Count)>
    drc_checker_type_name{
        "edge_spacing",
        "blocked_layers",
        "padding",
        "one_site_gap",
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

  // Core checking interface -- pure virtual, overridden by each rule.
  virtual bool check(const Node* cell,
                     GridX x,
                     GridY y,
                     const eUTL::PhysOrientation& orient) const = 0;

 protected:
  Grid* grid_;
  explicit DRCChecker(Grid* grid) : grid_(grid) {}
};

}  // namespace dpl2
