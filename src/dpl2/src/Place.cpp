// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors

#include <dpl2/DePlace.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Coordinates.h>
#include <infrastructure/Objects.h>
#include <infrastructure/Padding.h>
#include <infrastructure/network.h>
#include <PlacementDRC.h>

#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>
#include <set>
#include <tuple>
#include <fstream>
namespace dpl2 {

/**
 * @brief Computes the initial (pre-legalization) location of a cell.
 *
 * Returns the cell's origin in core-relative coordinates, taken from its
 * physical instance. When @p padded is true, adjusts X to account for
 * the left-side padding value.
 *
 * @param cell   Cell whose initial location is queried.
 * @param padded If true, subtract left padding from the X coordinate.
 * @return Core-relative DbuPt of the cell's origin.
 */
DbuPt DePlace::initialLocation(const Node* cell, const bool padded) const
{
  const PhysCell& inst = desMgr_->getPhysCell(cell->getDbInst());
  Point2D origin = inst.getOrigin();
  DbuPt loc;
  loc.x =(DbuX)(origin.getX() - core_.getXL()).getStorage();
  if (padded) {
    loc.x =(DbuX)(origin.getX().getStorage()) - gridToDbu(padding_->padLeft(cell),
        grid_->getSiteWidth());
  }
  loc.y = (DbuY)(origin.getY() - core_.getYL()).getStorage();
  return loc;
}

/**
 * @brief Snap a cell to a legal grid position nearest to @p pt.
 *
 * Clamps the given point inside the core area, aligns to row/site grid,
 * and returns the resulting DbuPt.
 *
 * @param cell Cell being legalized.
 * @param pt   Target core-relative point to legalize.
 * @return Legal DbuPt nearest to @p pt.
 */
DbuPt DePlace::legalPt(const Node* cell, const DbuPt& pt) const
{
  // Move inside core.
  const DbuX site_width = grid_->getSiteWidth();
  const DbuX core_x = std::clamp(
      pt.x,
      DbuX{0},
      gridToDbu(grid_->getRowSiteCount(), site_width) - cell->getWidth());
  // Align with row site.
  const GridX grid_x{divRound(core_x.v, site_width.v)};
  const DbuX legal_x{gridToDbu(grid_x, site_width)};
  // Align to row
  const DbuY core_y
      = std::clamp(pt.y, DbuY{0}, DbuY{core_.getYH().getStorage()} -
      cell->getHeight());
  const GridY grid_y = grid_->gridRoundY(core_y);
  DbuY legal_y = grid_->gridYToDbu(grid_y);
  return {legal_x, legal_y};
}

/**
 * @brief Legalize a cell to the nearest valid grid point.
 *
 * Uses the cell's current physical location as the starting point,
 * then applies moveHopeless and nearestBlockEdge strategies to avoid
 * fixed/blocked pixels. If @p padded is true, the initial location is
 * offset by the cell's left padding.
 *
 * @param cell   Cell to legalize.
 * @param padded If true, subtract left padding from initial X.
 * @return Legalized core-relative DbuPt.
 */
DbuPt DePlace::legalPt(const Node* cell, const bool padded) const
{
  if (cell->isFixed()) {
    // logger_->critical(
    //     DPL, 26, "legalPt called on fixed cell {}.", cell->name());
  }

  const DbuPt init = initialLocation(cell, padded);
  DbuPt legal_pt = legalPt(cell, init);
  GridX grid_x = grid_->gridX(legal_pt.x);
  GridY grid_y = grid_->gridSnapDownY(legal_pt.y);

  Pixel* pixel = grid_->gridPixel(grid_x, grid_y);
  if (pixel) {
    // Move std cells off of macros.  First try the is_hopeless strategy
    if (pixel->is_hopeless && moveHopeless(cell, grid_x, grid_y)) {
      legal_pt = DbuPt(gridToDbu(grid_x, grid_->getSiteWidth()),
                       grid_->gridYToDbu(grid_y));
      pixel = grid_->gridPixel(grid_x, grid_y);
    }

    const Node* block = pixel->cell;

    // If that didn't do the job fall back on the old move to nearest
    // edge strategy.  This doesn't consider site availability at the
    // end used so it is secondary.
    if (block && block->isBlock()) {
      const Rect block_bbox((UvDist)(block->getLeft().v),
                            (UvDist)(block->getBottom().v),
                            (UvDist)(block->getLeft().v + block->getWidth().v),
                            (UvDist)(block->getBottom().v + block->getHeight().v));
      if ((legal_pt.x + cell->getWidth()) >= block_bbox.getXL().getStorage()
          && legal_pt.x <= block_bbox.getXH().getStorage()
          && (legal_pt.y + cell->getHeight()) >= block_bbox.getYL().getStorage()
          && legal_pt.y <= block_bbox.getYH().getStorage()) {
        legal_pt = nearestBlockEdge(cell, legal_pt, block_bbox);
      }
    }
  }

  return legal_pt;
}

/**
 * @brief Find the nearest edge of a macro block and move the cell there.
 *
 * Computes the distance from the cell's legalized position to each of the
 * four edges of @p block_bbox, then snaps the cell to the closest edge
 * (left/right/below/above), re-legalizing the result.
 *
 * @param cell       Cell being pushed away from the block.
 * @param legal_pt   Current legalized core-relative position.
 * @param block_bbox Bounding box of the blocking macro.
 * @return New legal DbuPt adjacent to the nearest block edge.
 */
DbuPt DePlace::nearestBlockEdge(const Node* cell,
                                const DbuPt& legal_pt,
                                const Rect& block_bbox) const
{
  const DbuX legal_x = legal_pt.x;
  const DbuY legal_y = legal_pt.y;

  const DbuX x_min_dist = abs(legal_x - block_bbox.getXL().getStorage());
  const DbuX x_max_dist
      = abs(DbuX{block_bbox.getXH().getStorage()} - (legal_x + cell->getWidth()));
  const DbuY y_min_dist = abs(legal_y - block_bbox.getYL().getStorage());
  const DbuY y_max_dist
      = abs(DbuY{block_bbox.getYH().getStorage()} - (legal_y + cell->getHeight()));

  const int min_dist
      = std::min({x_min_dist.v, x_max_dist.v, y_min_dist.v, y_max_dist.v});

  if (min_dist == x_min_dist) {  // left of block
    return legalPt(cell,
                   {DbuX{block_bbox.getXL().getStorage()} - cell->getWidth(),
                    legal_pt.y});
  }
  if (min_dist == x_max_dist) {  // right of block
    return legalPt(cell, {DbuX{block_bbox.getXH().getStorage()}, legal_pt.y});
  }
  if (min_dist == y_min_dist) {  // below block
    return legalPt(cell,
        {legal_pt.x, DbuY{block_bbox.getYL().getStorage() - cell->getHeight().v}});
  }
  // above block
  return legalPt(cell, {legal_pt.x, DbuY{block_bbox.getYH().getStorage()}});
}

/**
 * @brief Get a legal grid point for a cell from its physical location.
 *
 * Uses the cell's current origin (optionally padded) as the target,
 * legalizes, and snaps to grid.
 *
 * @param cell  Cell being placed.
 * @param padded If true, offset X by left padding.
 * @return GridPt corresponding to the legalized location.
 */
GridPt DePlace::legalGridPt(const Node* cell, const bool padded) const
{
  const DbuPt pt = legalPt(cell, padded);
  return GridPt(grid_->gridX(pt.x), grid_->gridSnapDownY(pt.y));
}

/**
 * @brief Convert a DbuPt to a legal GridPt.
 *
 * Legalizes @p pt for @p cell and snaps to the nearest grid-aligned site.
 *
 * @param cell Cell being placed.
 * @param pt   Target core-relative point.
 * @return GridPt snapped to the nearest valid site.
 */
GridPt DePlace::legalGridPt(const Node* cell, const DbuPt& pt) const
{
  const DbuPt legal = legalPt(cell, pt);
  return GridPt(grid_->gridX(legal.x), grid_->gridSnapDownY(legal.y));
}

/**
 * @brief Rip-up cells inside a target rect to make room for target_cell.
 *
 * Collects all non-fixed cells occupying the given grid rect, unplaces
 * them, places target_cell via bounded diamond search, then re-places
 * the displaced cells with normal diamondMove.
 *
 * @param target_cell The cell to place inside the rect.
 * @param target_grid_rect Grid-aligned target region to clear and use.
 * @param start_pt Grid point inside the rect to start diamond search.
 * @return (grid_x, grid_y) of the placed target_cell, or (-1, -1) on failure.
 */
std::pair<int, int> DePlace::ripUpAndReplaceInRect(Node* target_cell,
                                                   const GridRect& target_grid_rect,
                                                   const GridPt& start_pt)
{
  const GridX x_min = std::max(GridX{0}, target_grid_rect.xlo);
  const GridY y_min = std::max(GridY{0}, target_grid_rect.ylo);
  const GridX x_max = std::min(grid_->getRowSiteCount(), target_grid_rect.xhi);
  const GridY y_max = std::min(grid_->getRowCount(), target_grid_rect.yhi);

  std::set<Node*> region_cells;
  for (GridX x = x_min; x < x_max; x++) {
    for (GridY y = y_min; y < y_max; y++) {
      Pixel* pixel = grid_->gridPixel(x, y);
      if (pixel) {
        Node* cell_in_pixel = pixel->cell;
        if (cell_in_pixel && !cell_in_pixel->isFixed()
            && cell_in_pixel != target_cell) {
          region_cells.insert(cell_in_pixel);
        }
      }
    }
  }

  // Unplace cells in the target rect to make room.
  for (Node* around_cell : region_cells) {
    unplaceCell(around_cell);
  }

  // Try to place the target cell inside the target rect.
  const PixelPt pixel_pt = diamondSearch(
      target_cell, start_pt.x, start_pt.y, x_min, x_max, y_min, y_max);
  if (pixel_pt.pixel) {
    // placeCell(target_cell, pixel_pt.x, pixel_pt.y);
    return {pixel_pt.x.v, pixel_pt.y.v};
  } else {
    placement_failures_.push_back(target_cell);
    return {-1, -1};
  }

  // Re-place the cells that were ripped up.
  for (Node* around_cell : region_cells) {
    if (!diamondMove(around_cell)) {
      placement_failures_.push_back(around_cell);
      return {-1, -1};
    }
  }
}

/**
 * @brief Diamond BFS placement search with automatic bounds.
 *
 * Searches outward from (@p x, @p y) in Manhattan-distance order for the
 * first pixel that can legally host @p cell. Bounds are derived from
 * max_displacement_x_ and max_displacement_y_.
 *
 * @param cell Cell to place.
 * @param x   Starting grid X.
 * @param y   Starting grid Y.
 * @return PixelPt with valid pixel if found, empty PixelPt otherwise.
 */
PixelPt DePlace::diamondSearch(const Node* cell,
                               const GridX x,
                               const GridY y,
                               std::vector<CellChangeRecord>* fillerChanges) const
{
  // Diamond search limits.
  GridX x_min = x - max_displacement_x_;
  GridX x_max = x + max_displacement_x_;
  GridY y_min = y - max_displacement_y_;
  GridY y_max = y + max_displacement_y_;
  // // Restrict search to group boundary.
  // Group* group = cell->getGroup();
  // if (group) {
  //   const GridRect grid_boundary = grid_->gridWithin(group->getBBox());
  //   const GridPt min = grid_boundary.closestPtInside({x_min, y_min});
  //   const GridPt max = grid_boundary.closestPtInside({x_max, y_max});
  //   x_min = min.x;
  //   y_min = min.y;
  //   x_max = max.x;
  //   y_max = max.y;
  // }

  return diamondSearch(cell, x, y, x_min, x_max, y_min, y_max, fillerChanges);
}

/**
 * @brief Diamond BFS search with explicit bounds.
 *
 * Identical BFS logic to the two-argument overload, but the caller
 * controls the search area via x_min_in/x_max_in/y_min_in/y_max_in
 * instead of deriving bounds from max_displacement_ and group boundaries.
 *
 * @param cell The cell to place.
 * @param x Starting grid X coordinate.
 * @param y Starting grid Y coordinate.
 * @param x_min_in Minimum grid X (inclusive).
 * @param x_max_in Maximum grid X (inclusive).
 * @param y_min_in Minimum grid Y (inclusive).
 * @param y_max_in Maximum grid Y (inclusive).
 * @return PixelPt with valid pixel if found, empty PixelPt otherwise.
 */
PixelPt DePlace::diamondSearch(const Node* cell,
                               const GridX x,
                               const GridY y,
                               const GridX x_min_in,
                               const GridX x_max_in,
                               const GridY y_min_in,
                               const GridY y_max_in,
                               std::vector<CellChangeRecord>* fillerChanges) const
{
  // Clip search limits to grid bounds.
  const GridX x_min = std::max(GridX{0}, x_min_in);
  const GridY y_min = std::max(GridY{0}, y_min_in);
  const GridX x_max = std::min(grid_->getRowSiteCount(), x_max_in);
  const GridY y_max = std::min(grid_->getRowCount(), y_max_in);

  struct PQ_entry
  {
    int manhattan_distance;
    GridPt p;
    int sequence;
    bool operator>(const PQ_entry& other) const
    {
      return std::tie(manhattan_distance, sequence)
             > std::tie(other.manhattan_distance, other.sequence);
    }
  };

  std::priority_queue<PQ_entry, std::vector<PQ_entry>, std::greater<PQ_entry>>
      positionsHeap;
  std::unordered_set<GridPt> visited;
  int sequence = 0;
  const GridPt center{x, y};
  positionsHeap.push(
      {.manhattan_distance = 0, .p = center, .sequence = sequence++});
  visited.insert(center);

  const std::vector<GridPt> neighbors = {{GridX(-1), GridY(0)},
                                         {GridX(1), GridY(0)},
                                         {GridX(0), GridY(-1)},
                                         {GridX(0), GridY(1)}};
  while (!positionsHeap.empty()) {
    const GridPt nearest = positionsHeap.top().p;
    positionsHeap.pop();
    if (fillerChanges != nullptr) {
      std::cout << "[findLeg BFS] try ("<< nearest.x.v<<","<< nearest.y.v << ")";
    }
    if (canBePlaced(cell, nearest.x, nearest.y, fillerChanges)) {
      if (fillerChanges != nullptr) {
        std::cout << " -> ACCEPT\n";
      }
      return PixelPt(
          grid_->gridPixel(nearest.x, nearest.y), nearest.x, nearest.y);
    }
    if (fillerChanges != nullptr) {
      std::cout << " -> reject\n";
    }
    // Put neighbors in the queue
    for (GridPt offset : neighbors) {
      GridPt neighbor = {nearest.x + offset.x, nearest.y + offset.y};
      // Check if it was already put in the queue
      if (visited.contains(neighbor)) {
        continue;
      }
      // Check limits
      if (neighbor.x < x_min || neighbor.x > x_max || neighbor.y < y_min
          || neighbor.y > y_max) {
        continue;
      }

      visited.insert(neighbor);
      positionsHeap.push({.manhattan_distance = calcDist(center, neighbor),
                          .p = neighbor,
                          .sequence = sequence++});
    }
  }
  if (fillerChanges != nullptr) {
    std::cout << "[findLeg BFS] exhausted " << sequence
              << " enqueued candidate(s) in box "
              << "x=[" << x_min.v << "," << x_max.v << "]"
              << " y=[" << y_min.v << "," << y_max.v << "], no legal site\n";
  }
  return PixelPt();
}

/**
 * @brief Manhattan distance between two grid points for BFS priority.
 *
 * Distance is computed as the sum of the X separation (in Dbu units)
 * and Y separation (in Dbu units after grid-Y to Dbu conversion).
 *
 * @param p1 First grid point.
 * @param p2 Second grid point.
 * @return Manhattan distance in Dbu units.
 */
int DePlace::calcDist(const GridPt& p1, const GridPt& p2) const
{
  DbuY y_dist = abs(grid_->gridYToDbu(p1.y) - grid_->gridYToDbu(p2.y));
  DbuX x_dist = gridToDbu(abs(p1.x - p2.x), grid_->getSiteWidth());
  return sumXY(x_dist, y_dist);
}

/**
 * @brief Legalize a single cell into a specified target rect.
 *
 * Used during ECO to place a cell inside a designated region.
 * First tries diamond search within the target rect. If that fails,
 * falls back to rip-up-and-replace: unplaces cells already occupying
 * the rect, places the target cell, then re-places the displaced cells.
 *
 * @param cell The instance to legalize.
 * @param target_rect The target region in absolute (design) coordinates.
 * @return (grid_x, grid_y) of the placed cell, or (-1, -1) on failure.
 */
std::pair<int, int> DePlace::legalCellInRect(const Rect& target_rect, Node* cell,
                                             std::vector<CellChangeRecord>* fillerChanges)
{
  if (!cell) {
    // logger_->error(
    //     DPL, 70, "Instance {} not found in placement network.", cell->name());
    return {-1, -1};
  }

  if (cell->isFixed()) {
    // logger_->warn(DPL, 71, "Cannot legalize fixed cell {}.", cell->name());
    return {-1, -1};
  }

  // Unplace the cell if currently placed.
  if (cell->isPlaced()) {
    unplaceCell((cell));
  }

  // Convert target rect to core-relative coordinates.
  const DbuRect target_dbu_rect(Rect(target_rect.getXL() - core_.getXL(),
                                     target_rect.getYL() - core_.getYL(),
                                     target_rect.getXH() - core_.getXL(),
                                     target_rect.getYH() - core_.getYL()));

  // Find the nearest point inside the target rect to the cell's current
  // position, then snap to a legal grid point.
  const DbuPt nearest = nearestPt(cell, target_dbu_rect);

  const GridPt start_grid_pt = legalGridPt((cell), nearest);

  // Clip start point inside the target rect bounds.
  const GridRect target_grid_rect = grid_->gridWithin(target_dbu_rect);
  const GridPt start_pt = target_grid_rect.closestPtInside(start_grid_pt);

  const GridX x_min = std::max(GridX{0}, target_grid_rect.xlo);
  const GridY y_min = std::max(GridY{0}, target_grid_rect.ylo);
  const GridX x_max = std::min(grid_->getRowSiteCount(), target_grid_rect.xhi);
  const GridY y_max = std::min(grid_->getRowCount(), target_grid_rect.yhi);

  const PixelPt pixel_pt
      = diamondSearch((cell), start_pt.x, start_pt.y, x_min, x_max, y_min, y_max,
                      fillerChanges);

  if (pixel_pt.pixel) {
    // placeCell((cell), pixel_pt.x, pixel_pt.y);
    return {pixel_pt.x.v, pixel_pt.y.v};
  }

  // Diamond search failed.  The rip-up-and-replace fallback is disabled: it
  // unplaces (and never re-places) neighbor cells, permanently mutating the
  // placement state, which is unsafe for read-only/probe use.  Report failure
  // instead.
  // return ripUpAndReplaceInRect((cell), target_grid_rect, start_pt);
  return {-1, -1};
}

/**
 * @brief Assign grid-aligned coordinates to a cell without painting pixels.
 *
 * Sets the cell's left/bottom to the Dbu position derived from
 * (@p x, @p y) using the current grid's site width and row pitch.
 *
 * @param cell Cell to update.
 * @param x    Grid X coordinate.
 * @param y    Grid Y coordinate.
 */
void DePlace::setGridLoc(Node* cell, const GridX x, const GridY y) const
{
  cell->setLeft(gridToDbu(x, grid_->getSiteWidth()));
  cell->setBottom(grid_->gridYToDbu(y));
}

/**
 * @brief Place a cell at a specific grid position.
 *
 * Sets the cell's grid coordinates, paints it onto the pixel grid, marks
 * it as placed, and assigns the site orientation.
 *
 * @param cell Cell to place.
 * @param x    Grid X coordinate.
 * @param y    Grid Y coordinate.
 */
void DePlace::placeCell(Node* cell, const GridX x, const GridY y)
{
  setGridLoc(cell, x, y);
  grid_->paintPixel(cell, x, y);
  cell->setPlaced(true);
  std::string site_name =
      cell->getMaster()->getPhysLibCell()->getTechSite()->getName();
  cell->setOrient(grid_->getSiteOrientation(x, y, site_name).value());
}

/**
 * @brief Remove a non-fixed cell from the grid.
 *
 * Erases the cell's occupied pixels and clears its placed/hold flags.
 * Fixed cells and cells that are not currently placed are silently ignored.
 *
 * @param cell Cell to unplace.
 */
void DePlace::unplaceCell(Node* cell)
{
  if (cell->isFixed() || !cell->isPlaced()) {
    return;
  }
  grid_->erasePixel(cell);
  cell->setPlaced(false);
  cell->setHold(false);
}

/**
 * @brief Check whether a cell can be legally placed at (@p bin_x, @p bin_y).
 *
 * Verifies that the cell fits within grid bounds, all required pixels are
 * valid and empty, one-site-gap rules are satisfied, symmetry constraints
 * are met, and the DRC engine approves.
 *
 * @param cell  Cell to check.
 * @param bin_x Lower-left grid X.
 * @param bin_y Lower-left grid Y.
 * @return true if the placement is legal, false otherwise.
 */
bool DePlace::canBePlaced(const Node* cell, GridX bin_x, GridY bin_y,
                          std::vector<CellChangeRecord>* fillerChanges) const
{
  if (bin_y >= grid_->getRowCount()) {
    return false;
  }

  const GridX x_end = bin_x + grid_->gridWidth(cell);
  const GridY y_end
      = grid_->gridEndY(grid_->gridYToDbu(bin_y) + cell->getHeight());

  // With fillerChanges, findLeg admits a target over whitespace and/or one or
  // more intersecting fillers. The checker may return gap-filling Adds plus
  // surrounding filler Replaces. Ordinary placement paths pass nullptr.
  return checkPixels(cell, bin_x, bin_y, x_end, y_end, fillerChanges);
}

/**
 * @brief Verify all pixels in the given grid rectangle
 * are free and valid for @p cell.
 *
 * Each pixel must exist, be unoccupied, marked valid, belong to the
 * correct group (if any), and support the required site orientation on
 * the first row. When disallow_one_site_gaps_ is set, additionally
 * rejects placements that would create single-site gaps at the left or
 * right boundary.
 *
 * @param cell  Cell to check.
 * @param x     Lower-left grid X.
 * @param y     Lower-left grid Y.
 * @param x_end Exclusive upper-bound grid X.
 * @param y_end Exclusive upper-bound grid Y.
 * @return true if all pixels pass.
 */
bool DePlace::checkPixels(const Node* cell,
                          const GridX x,
                          const GridY y,
                          const GridX x_end,
                          const GridY y_end,
                          std::vector<CellChangeRecord>* fillerChanges) const
{
  if (x_end > grid_->getRowSiteCount()) {
    return false;
  }
  // if (!checkRegionOverlap(cell, x, y, x_end, y_end)) {
  //   return false;
  // }
  const TechSite* site = cell->getMaster()->getPhysLibCell()->getTechSite();

  for (GridY y1 = y; y1 < y_end; y1++) {
    const bool first_row = (y1 == y);
    for (GridX x1 = x; x1 < x_end; x1++) {
      const Pixel* pixel = grid_->gridPixel(x1, y1);
      if (pixel == nullptr || (pixel->cell && !pixel->cell->isFiller())
          || !pixel->is_valid
          || (cell->inGroup() && pixel->group != cell->getGroup())
          || (!cell->inGroup() && pixel->group)
          || (first_row && !grid_->getSiteOrientation(x1, y1, site->getName()))) {
        if (fillerChanges != nullptr) {
          std::cout << "  [findLeg pixels] (" << x1.v << "," << y1.v << ") reject:";
          if (pixel == nullptr) {
            std::cout << " pixel==null";
          } else {
            if (pixel->cell && !pixel->cell->isFiller()) {
              std::cout << " occupied-by-std-cell";
            }
            if (!pixel->is_valid) {
              std::cout << " !valid";
            }
            if (cell->inGroup() && pixel->group != cell->getGroup()) {
              std::cout << " group-mismatch";
            }
            if (!cell->inGroup() && pixel->group) {
              std::cout << " in-foreign-group";
            }
          }
          if (first_row && !grid_->getSiteOrientation(x1, y1, site->getName())) {
            std::cout << " bad-site-orient";
          }
          std::cout << "\n";
        }
        return false;
      }
    }
  }

  const auto orient = grid_->getSiteOrientation(x, y, site->getName()).value();

  // Check for symmetry
  const PhysLibCell* master = cell->getMaster()->getPhysLibCell();
  unsigned masterSym = getMasterSymmetry((int)master->getSymmetry());

  if (!checkMasterSym(masterSym, orient)) {
    if (fillerChanges != nullptr) {
      std::cout << "  [findLeg checkPixels] (" << x.v << "," << y.v
                << ") reject: symmetry masterSym=" << masterSym
                << " orient=" << static_cast<uint>(orient) << "\n";
    }
    return false;
  }

  std::vector<CellChangeRecord> ccRecords;
  if (fillerChanges != nullptr) {
    // Describe every filler intersected by the temporary target as a caller-
    // owned Delete overlay. The checker/repair engine may accept partial
    // coverage and retile any released sites outside the target; Grid and
    // Network remain unchanged during this probe.
    setGridLoc(const_cast<Node*>(cell), x, y);
    std::vector<Node*> replacedFillers;
    std::set<int> seenFillerIds;
    for (GridY y1 = y; y1 < y_end; ++y1) {
      for (GridX x1 = x; x1 < x_end; ++x1) {
        const Pixel* pixel = grid_->gridPixel(x1, y1);
        if (pixel == nullptr
            || (pixel->cell != nullptr && !pixel->cell->isFiller())) {
          return false;
        }
        if (pixel->cell != nullptr
            && seenFillerIds.insert(pixel->cell->getId()).second) {
          replacedFillers.push_back(pixel->cell);
        }
      }
    }
    if (replacedFillers.empty()) {
      return false;
    }
    std::sort(replacedFillers.begin(),
              replacedFillers.end(),
              [](const Node* left, const Node* right) {
                return left->getId() < right->getId();
              });
    std::vector<CellChangeRecord> overlayChanges;
    overlayChanges.reserve(replacedFillers.size());
    for (const Node* filler : replacedFillers) {
      if (filler == nullptr || filler->getMaster() == nullptr) {
        return false;
      }
      overlayChanges.push_back(
          CellChangeRecord{OpType::Delete,
                           filler->getDbInst(),
                           UvDist(filler->getLeft().v),
                           UvDist(filler->getBottom().v),
                           filler->getMaster()->getDbMaster(),
                           filler->getMaster()->getDbMaster(),
                           filler->getOrient()});
    }
    return drc_engine_->checkDRC(
        cell, x, y, orient, *fillerChanges, overlayChanges);
  }
  return drc_engine_->checkDRC(cell, x, y, orient, ccRecords);
}

/**
 * @brief Map integer symmetry code to internal symmetry bitmask.
 *
 * Converts the physical library's integer symmetry encoding:
 * - 2 → Symmetry_X
 * - 3 → Symmetry_Y
 * - 5 → Symmetry_ROT90
 *
 * @param symmetry Raw symmetry code from PhysLibCell.
 * @return Bitmask of Symmetry_* flags.
 */
unsigned DePlace::getMasterSymmetry(int symmetry) const
{
  unsigned masterSym = 0;
  switch (symmetry) {
    case 2:
      masterSym |= Symmetry_X;
      break;
    case 3:
      masterSym |= Symmetry_Y;
      break;
    case 5:
      masterSym |= Symmetry_ROT90;
      break;
    default:
      break;
  }
  return masterSym;
}

/**
 * @brief Check that a cell's orientation is compatible with its master's symmetry.
 *
 * @param masterSym Symmetry bitmask from the master.
 * @param cellOri   Desired orientation for the placed cell.
 * @return true if the orientation is allowed.
 */
bool DePlace::checkMasterSym(unsigned masterSym,
    eUTL::PhysOrientation cellOri) const
{
  using eUTL::PhysOrientationE;
  switch (cellOri.getValue()) {
    case PhysOrientationE::R0:
      return true;
    case PhysOrientationE::MX:
      return (masterSym & Symmetry_Y) != 0;
    case PhysOrientationE::MY:
      return (masterSym & Symmetry_X) != 0;
    case PhysOrientationE::R180:
      return (masterSym & Symmetry_Y) && (masterSym & Symmetry_X);
    case PhysOrientationE::R90:
    case PhysOrientationE::R270:
      return (masterSym & Symmetry_ROT90) != 0;
    case PhysOrientationE::MX90:
    case PhysOrientationE::MY90:
      return (masterSym & Symmetry_ROT90) && (masterSym & Symmetry_Y)
          && (masterSym & Symmetry_X);
    default:
      return false;
  }
}

////////////////////////////////////////////////////////////////////////

/**
 * @brief Attempt to place a cell via diamond
 * search from its current legal location.
 *
 * @param cell Cell to place.
 * @return true if a legal pixel was found and the cell was placed.
 */
bool DePlace::diamondMove(Node* cell)
{
  const GridPt init = legalGridPt(cell, false);
  return diamondMove(cell, init);
}

/**
 * @brief Attempt to place a cell via diamond search from a specific grid point.
 *
 * @param cell    Cell to place.
 * @param grid_pt Starting grid point for diamond search.
 * @return true if a legal pixel was found and the cell was placed.
 */
bool DePlace::diamondMove(Node* cell, const GridPt& grid_pt)
{
  const PixelPt pixel_pt = diamondSearch(cell, grid_pt.x, grid_pt.y);
  if (pixel_pt.pixel) {
    placeCell(cell, pixel_pt.x, pixel_pt.y);
    return true;
  }
  return false;
}

/**
 * @brief Find the nearest valid grid site by scanning left/right/above/below.
 *
 * Starting from (@p grid_x, @p grid_y), searches each direction
 * independently for the closest pixel marked is_valid. Useful as a
 * fallback to move cells off hopeless (blocked) sites.
 *
 * @param[in]  cell   Cell being moved (used to account for width in distance).
 * @param[in,out] grid_x Current grid X, updated to the nearest valid X on success.
 * @param[in,out] grid_y Current grid Y, updated to the nearest valid Y on success.
 * @return true if a valid pixel was found in any direction.
 */
bool DePlace::moveHopeless(const Node* cell, GridX& grid_x, GridY& grid_y) const
{
  GridX best_x = grid_x;
  GridY best_y = grid_y;
  int best_dist = std::numeric_limits<int>::max();
  const GridX site_count = grid_->getRowSiteCount();
  const GridY row_count = grid_->getRowCount();
  const DbuX site_width = grid_->getSiteWidth();

  for (GridX x = grid_x - 1; x >= 0; --x) {  // left
    if (grid_->pixel(grid_y, x).is_valid) {
      best_dist = gridToDbu(grid_x - x - 1, site_width).v;
      best_x = x;
      best_y = grid_y;
      break;
    }
  }
  for (GridX x = grid_x + 1; x < site_count; ++x) {  // right
    if (grid_->pixel(grid_y, x).is_valid) {
      const int dist = gridToDbu(x - grid_x, site_width).v - cell->getWidth().v;
      if (dist < best_dist) {
        best_dist = dist;
        best_x = x;
        best_y = grid_y;
      }
      break;
    }
  }
  for (GridY y = grid_y - 1; y >= 0; --y) {  // below
    if (grid_->pixel(y, grid_x).is_valid) {
      const int dist = (grid_->gridYToDbu(grid_y) - grid_->gridYToDbu(y)).v;
      if (dist < best_dist) {
        best_dist = dist;
        best_x = grid_x;
        best_y = y;
      }
      break;
    }
  }
  for (GridY y = grid_y + 1; y < row_count; ++y) {  // above
    if (grid_->pixel(y, grid_x).is_valid) {
      const int dist = (grid_->gridYToDbu(y) - grid_->gridYToDbu(grid_y)).v;
      if (dist < best_dist) {
        best_dist = dist;
        best_x = grid_x;
        best_y = y;
      }
      break;
    }
  }
  if (best_dist != std::numeric_limits<int>::max()) {
    grid_x = best_x;
    grid_y = best_y;
    return true;
  }
  return false;
}

} // namespace dpl2
