// Functional definitions for the Grid member functions the repair chain
// links against, computed over the Helper/test-populated row maps.
// Grid.cpp itself is excluded from the tier-1 build (it needs tbb and the
// PhysNet visitor surface, none of which the repair chain touches).
#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"

namespace dpl2 {

GridY Grid::gridSnapDownY(DbuY y) const
{
  auto it = row_y_dbu_to_index_.upper_bound(y);
  if (it == row_y_dbu_to_index_.begin()) {
    return GridY{0};
  }
  --it;
  return it->second;
}

GridX Grid::gridX(DbuX x) const
{
  return GridX{site_width_.v > 0 ? static_cast<int>(x.v / site_width_.v) : 0};
}

GridY Grid::gridSnapDownY(const Node* cell) const
{
  return gridSnapDownY(cell->getBottom());
}

GridX Grid::gridX(const Node* cell) const
{
  return gridX(cell->getLeft());
}

GridY Grid::gridHeight(const PhysLibCell& master) const
{
  const int h = master.getHeight().getStorage();
  const int rowH = uniform_row_height_ ? uniform_row_height_->v : 0;
  return GridY{rowH > 0 ? (h + rowH - 1) / rowH : 1};
}

bool Grid::isMultiHeight(const PhysLibCell& master) const
{
  return gridHeight(master).v > 1;
}

}  // namespace dpl2
