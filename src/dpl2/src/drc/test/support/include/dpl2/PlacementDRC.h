// Fake dpl2/PlacementDRC.h (tier-1): the real header is not delivered yet.
// Interface names match the call sites in infrastructure/network.cpp.
#pragma once
#include <string>
namespace dpl2 {
class PlacementDRC
{
 public:
  bool hasCellEdgeSpacingTable() const { return has_table_; }
  int getEdgeTypeIdx(const char* name) const
  {
    return std::string(name) == "DEFAULT" ? 0 : -1;
  }
  bool has_table_ = false;
};
}  // namespace dpl2
