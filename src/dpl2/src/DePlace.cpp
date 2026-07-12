// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors

#include "dpl2/DePlace.h"
#include "infrastructure/Grid.h"
#include "infrastructure/network.h"
#include "infrastructure/Padding.h"
#include "drc/PaddingChecker.h"
#include "drc/EdgeSpacingChecker.h"
#include "PlacementDRC.h"

namespace dpl2 {

DePlace::DePlace(PhysDesMgr* desMgr)
    : desMgr_(desMgr),
      network_(std::make_unique<Network>()),
      padding_(std::make_shared<Padding>()),
      grid_(std::make_unique<Grid>()),
      arch_(std::make_unique<Architecture>())
{
  design_ = eUNL::Session::getSession().getCurrentDesign();
  padding_->setDesginManager(desMgr);
}

DePlace::DePlace()
    : network_(std::make_unique<Network>()),
      padding_(std::make_shared<Padding>()),
      grid_(std::make_unique<Grid>()),
      arch_(std::make_unique<Architecture>())
{
  eUNL::Session& sess = eUNL::Session::getSession();
  eUNL::Design* design = sess.getCurrentDesign();
  eUNL::PhysDesMgr* desMgr = design->getPhysDesMgr();

  this->desMgr_ = desMgr;
  this->design_ = design;
  padding_->setDesginManager(desMgr_);
  if (!data_loaded_) {
    importDb();
    initGrid();
    // Paint Fixed&Placed cells.
    setFixedGridCells();
    setPlacedGridCells();
    groupInitPixels();
  }
}

DePlace::~DePlace() = default;

void DePlace::setPaddingGlobal(const int left, const int right)
{
  padding_->setPaddingGlobal(GridX{left}, GridX{right});
}

void DePlace::setPadding(LeafCellID cellId, const int left, const int right)
{
  padding_->setPadding(cellId, GridX{left}, GridX{right});
}

void DePlace::setPadding(PhysLibCell* master, const int left, const int right)
{
  padding_->setPadding(master, GridX{left}, GridX{right});
}

/**
 * @brief Attempts to find a legal placement for a leaf cell after remapping its master in dpl2's internal cache.
 *
 * Given a leaf cell and a target module name, this overload:
 *   1. Resolves @p moduleName to a PhysLibCell via LibObjAccessor,
 *   2. Registers the new master via addMaster() and updates the Node via updateNode(),
 *   3. Searches within a @p diameter-extended bounding box around the cell's original position
 *      for a DRC-legal placement via legalCellInRect().
 *
 * Note: This remaps the master only in dpl2's own Master/Node cache; it does not modify
 *       UDM (no changeLeafCellsMaster call). The caller is responsible for any UDM-side remap
 *       if required.
 *
 * @param cellId     The leaf cell to remap and place.
 * @param diameter   Search region extension (in uv units) around the cell's original bbox.
 * @param moduleName Target module name whose PhysLibCell replaces the cell's current master.
 * @return The (left, bottom) UV coordinates of the placed cell, or (-1, -1) on failure.
 */
std::pair<int, int> DePlace::findLeg(LeafCellID cellId,
                                     int diameter,
                                     std::string moduleName)
{
  eFNL::ModuleID moduleId = design_->getLibAcc().findModule(moduleName);
  auto* libCell = design_->getLibAcc().getLibCell(moduleId);
  if (!libCell) {
    return {-1, -1};
  }

  const PhysLibCell& physLibCell
      = design_->getLibAcc().getPhysLibCell(libCell->getId());
  Node* cell = this->network_->getNode(cellId);
  // Filler-TODO check_drc here?
  unplaceCell(cell);
  this->network_->addMaster(
      physLibCell, this->grid_.get(), this->drc_engine_.get());
  this->network_->updateNode(cell, desMgr_, physLibCell);

  eUTL::Rect rect(
      eUTL::UvDist(cell->getLeft().v - diameter),
      eUTL::UvDist(cell->getBottom().v - diameter),
      eUTL::UvDist(cell->getLeft().v + cell->getWidth().v + diameter),
      eUTL::UvDist(cell->getBottom().v + cell->getHeight().v + diameter));
  legalCellInRect(rect, cell);

  return {cell->getLeft().v, cell->getBottom().v};
}

/**
 * @brief Attempts to find a legal placement for a leaf cell after remapping its master in dpl2's internal cache.
 *
 * This is the core-area overload: it searches the entire core area
 * (this->core_) for a DRC-legal position.
 *
 * Same remap semantics as the diameter overload - only dpl2 Node/Master
 * cache is changed, not UDM.
 *
 * @param cellId     The leaf cell to remap and place.
 * @param moduleName Target module name whose PhysLibCell replaces the cell's current master.
 * @return The (left, bottom) UV coordinates of the placed cell, or (-1, -1) on failure.
 */
std::pair<int, int> DePlace::findLeg(LeafCellID cellId,
                                     std::string moduleName)
{
  Rect rect = this->core_;
  eFNL::ModuleID moduleId = design_->getLibAcc().findModule(moduleName);
  auto* libCell = design_->getLibAcc().getLibCell(moduleId);
  if (!libCell) {
    return {-1, -1};
  }
  const PhysLibCell& physLibCell
      = design_->getLibAcc().getPhysLibCell(libCell->getId());

  Node* cell = this->network_->getNode(cellId);
  unplaceCell(cell);
  this->network_->addMaster(
      physLibCell, this->grid_.get(), this->drc_engine_.get());
  this->network_->updateNode(cell, desMgr_, physLibCell);
  legalCellInRect(rect, cell);

  return {cell->getLeft().v, cell->getBottom().v};
}

/**
 * @brief findLeg - repack a cell to a different cell type at its current
 *        location, then validate the new footprint for DRC legality.
 *
 * Unplaces @p cellId, adds the PhysLibCell for @p moduleName to the grid,
 * updates the node, and checks DRC. The cell is NOT moved; only its master
 * is swapped in place. This overload is designed for ECO scenarios where
 * the caller wants to know "can this cell be swapped to a different standard
 * cell without shifting its (left, bottom) coordinates."
 *
 * @param cellId     LeafCellID of the cell to repack.
 * @param moduleName Name of the target library cell master.
 * @retval true      The repacked cell is DRC-legal at the current location.
 * @retval false     The repacked cell violates DRC (likely insufficient
 *                   space or incompatible master footprint).
 */
bool DePlace::isLegal(LeafCellID cellId, std::string moduleName)
{
  Rect rect = this->core_;
  eFNL::ModuleID moduleId = design_->getLibAcc().findModule(moduleName);
  auto* libCell = design_->getLibAcc().getLibCell(moduleId);
  if (!libCell) {
    return false;
  }
  const PhysLibCell& physLibCell
      = design_->getLibAcc().getPhysLibCell(libCell->getId());

  Node* cell = this->network_->getNode(cellId);
  unplaceCell(cell);
  this->network_->addMaster(
      physLibCell, this->grid_.get(), this->drc_engine_.get());
  this->network_->updateNode(cell, desMgr_, physLibCell);

  bool is_valid = drc_engine_->checkDRC(cell);
  return is_valid;
}

Rect DePlace::getCoreArea()
{
  std::vector<PhysRow> rows;
  DbuRect rect(
      Rect(UvDist(INT_MAX), UvDist(INT_MAX), UvDist(INT_MIN), UvDist(INT_MIN)));
  for (const auto& row : desMgr_->getPhysRowIter()) {
    if (row.getSite().getIsPad() == false) {
      rows.push_back(row);
    }
  }
  if (!rows.empty()) {
    for (PhysRow row : rows) {
      rect.expand(DbuRect(row.getBbox()));
    }
  }
  return rect.getRect();
}

void DePlace::setFixedGridCells()
{
  for (auto& cell : network_->getNodes()) {
    if (cell->getType() == Node::CELL && cell->isFixed()) {
      grid_->visitCellPixels(cell.get(), false, [&](Pixel* pixel, bool padded) {
        if (padded) {
          pixel->padding_reserved_by = cell.get();
        } else {
          setGridCell(cell.get(), pixel);
        }
      });
    }
  }
}

void DePlace::setPlacedGridCells()
{
  for (auto& cell : network_->getNodes()) {
    if (cell->getType() == Node::CELL && cell->isPlaced()) {
      grid_->visitCellPixels(cell.get(), false, [&](Pixel* pixel, bool padded) {
        if (padded) {
          pixel->padding_reserved_by = cell.get();
        } else {
          setGridCell(cell.get(), pixel);
        }
      });
    }
  }
}

void DePlace::setGridCell(Node* cell, Pixel* pixel)
{
  pixel->cell = cell;
  pixel->util = 1.0;
  if (cell->isBlock()) {
    // Try the is_hopeless strategy to get off of a block
    pixel->is_hopeless = true;
  }
}

void DePlace::initGrid()
{
  grid_->initGrid(
      desMgr_, padding_, max_displacement_x_, max_displacement_y_);
  // Get core from grid after initialization
  core_ = grid_->getCore();
}

void DePlace::deleteGrid()
{
  grid_->clear();
}

// ============================================================================
// Stub implementations for Place.cpp - to be implemented later
// ============================================================================

DbuPt DePlace::nearestPt(const Node* cell, const DbuRect& rect) const
{
  // TODO: Implement nearest point calculation
  DbuX nearest_x = cell->getLeft();
  DbuY nearest_y = cell->getBottom();

  if (nearest_x < rect.xl)
    nearest_x = rect.xl;
  else if (nearest_x > rect.xh)
    nearest_x = rect.xh;

  if (nearest_y < rect.yl)
    nearest_y = rect.yl;
  else if (nearest_y > rect.yh)
    nearest_y = rect.yh;

  return DbuPt{nearest_x, nearest_y};
}

void DePlace::groupInitPixels()
{
  for (GridX x{0}; x < grid_->getRowSiteCount(); x++) {
    for (GridY y{0}; y < grid_->getRowCount(); y++) {
      Pixel* pixel = grid_->gridPixel(x, y);
      pixel->util = 0.0;
    }
  }

  for (auto& group : arch_->getRegions()) {
    if (group->getCells().empty()) {
      if (group->getId() != 0) {
        // logger_->warn(
        //     DPL, 42, "No cells found in group {}.", group->getName());
      }
      continue;
    }
    const DbuX site_width = grid_->getSiteWidth();
    for (const DbuRect rect : group->getRects()) {
      const GridRect grid_rect{grid_->gridWithin(rect)};

      for (GridY k{grid_rect.ylo}; k < grid_rect.yhi; k++) {
        for (GridX l{grid_rect.xlo}; l < grid_rect.xhi; l++) {
          Pixel* pixel = grid_->gridPixel(l, k);
          pixel->util += 1.0;
        }
        if (rect.xl % site_width != 0) {
          Pixel* pixel = grid_->gridPixel(grid_rect.xlo, k);
          pixel->util
              -= (rect.xl % site_width).v / static_cast<double>(site_width.v);
        }
        if (rect.xh % site_width != 0) {
          Pixel* pixel = grid_->gridPixel(grid_rect.xhi - 1, k);
          pixel->util -= (site_width - rect.xh % site_width).v
                         / static_cast<double>(site_width.v);
        }
      }
    }
    for (const DbuRect rect : group->getRects()) {
      const GridRect grid_rect{grid_->gridWithin(rect)};

      for (GridY k{grid_rect.ylo}; k < grid_rect.yhi; k++) {
        for (GridX l{grid_rect.xlo}; l < grid_rect.xhi; l++) {
          // Assign group to each pixel.
          Pixel* pixel = grid_->gridPixel(l, k);
          if (pixel->util == 1.0) {
            pixel->group = group;
            pixel->is_valid = true;
            pixel->util = 1.0;
          } else if (pixel->util > 0.0 && pixel->util < 1.0) {
            // pixel->cell = dummy_cell_.get();
            pixel->util = 0.0;
            pixel->is_valid = false;
          }
        }
      }
    }
  }
}

}  // namespace dpl2
