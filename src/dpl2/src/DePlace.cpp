// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors

#include <dpl2/DePlace.h>
#include <infrastructure/Grid.h>
#include <infrastructure/network.h>
#include <infrastructure/Padding.h>
#include <infrastructure/fillerSetting.h>
#include <drc/ImplantLayerChecker.h>
#include <PlacementDRC.h>

namespace dpl2 {

DePlace::DePlace(PhysDesMgr* desMgr)
    : desMgr_(desMgr),
      arch_(std::make_unique<Architecture>()),
      network_(std::make_unique<Network>()),
      padding_(std::make_shared<Padding>()),
      grid_(std::make_unique<Grid>())
{
  design_ = eUNL::Session::getSession().getCurrentDesign();
  padding_->setDesginManager(desMgr);
  filler_setting_ = std::make_unique<fillerSetting>(design_);
  network_->setFillerSetting(filler_setting_.get());
}

DePlace::DePlace()
    : arch_(std::make_unique<Architecture>()),
      network_(std::make_unique<Network>()),
      padding_(std::make_shared<Padding>()),
      grid_(std::make_unique<Grid>())
{
  eUNL::Session& sess = eUNL::Session::getSession();
  eUNL::Design* design = sess.getCurrentDesign();
  eUNL::PhysDesMgr* desMgr = design->getPhysDesMgr();

  this->desMgr_ = desMgr;
  this->design_ = design;
  padding_->setDesginManager(desMgr_);
  filler_setting_ = std::make_unique<fillerSetting>(design);
  network_->setFillerSetting(filler_setting_.get());
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

bool DePlace::registerFillerRepairMasters()
{
  if (filler_setting_ == nullptr || network_ == nullptr || grid_ == nullptr
      || edge_type_table_ == nullptr) {
    return false;
  }

  const std::vector<const PhysLibCell*> masters
      = filler_setting_->getFillerPhysCells();
  if (masters.empty()
      || std::any_of(masters.begin(), masters.end(),
                     [](const PhysLibCell* master) {
                       return master == nullptr;
                     })) {
    return false;
  }

  for (const PhysLibCell* master : masters) {
    if (network_->addMaster(*master,
                            *filler_setting_,
                            grid_.get(),
                            edge_type_table_.get()) == nullptr) {
      return false;
    }
  }

  // addMaster refreshes an existing Master's classification, but Nodes keep
  // the type captured when they were imported. set_filler_option commonly
  // runs after that import, so synchronize placed instances as part of the
  // same infrastructure-owned registration step.
  for (const auto& [nid, node] : network_->getNodes()) {
    if (node != nullptr && node->getMaster() != nullptr
        && filler_setting_->isFillerCell(
            node->getMaster()->getDbMaster())) {
      node->setType(Node::FILLER);
    }
  }
  return true;
}

bool DePlace::prepareFillerRepair(const PhysLibCell& targetMaster)
{
  std::lock_guard<std::mutex> lock(filler_repair_init_mutex_);
  if (network_ == nullptr || grid_ == nullptr || filler_setting_ == nullptr
      || edge_type_table_ == nullptr) {
    return false;
  }

  if (drc_engine_ != nullptr) {
    DRCChecker* const existing
        = drc_engine_->getChecker(DRCCheckerType::ImplantLayer);
    if (existing != nullptr) {
      const auto* checker = dynamic_cast<ipl::ImplantLayerChecker*>(existing);
      const int masterId = network_->getMasterId(targetMaster.getLibCellId());
      return checker != nullptr && masterId >= 0
             && checker->getMasterItems().contains(masterId);
    }
  }

  if (network_->getMaster(targetMaster.getLibCellId()) == nullptr
      && network_->addMaster(targetMaster,
                             *filler_setting_,
                             grid_.get(),
                             edge_type_table_.get()) == nullptr) {
    return false;
  }
  if (!registerFillerRepairMasters()) {
    return false;
  }
  if (drc_engine_ == nullptr) {
    initPlacementDRC();
  }
  if (drc_engine_ == nullptr) {
    return false;
  }
  drc_engine_->addChecker(
      DRCCheckerType::ImplantLayer,
      std::make_unique<ipl::ImplantLayerChecker>(
          grid_.get(), design_, network_.get()));
  return true;
}

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
 * @brief Read-only overlay DRC of swapping @p target to master @p masterId at
 *        its current location.
 *
 * Builds a throw-away node with the target master at the target's current
 * coordinates (never inserted into the network, never painted), feeds the
 * node plus the one old Network std cell as the replacement overlay, and
 * returns the verdict. No in-memory placement state is mutated.
 *
 * @param  masterId    Target library cell master.
 * @param  target      The std-cell currently occupying the probe site.
 * @param  cellChanges [out] Atomic surrounding-filler Replace records when
 *                     repair succeeds; unchanged when the check fails.
 * @retval true        The swap is DRC-legal at the current location.
 * @retval false       The swap violates DRC.
 */
bool DePlace::isLegalProbe(LibCellID masterId, const Node* target,
                           std::vector<CellChangeRecord>& cellChanges)
{
  if (target == nullptr) {
    return false;
  }
  const PhysLibCell& new_pcell = design_->getLibAcc().getPhysLibCell(masterId);

  if (!prepareFillerRepair(new_pcell)) {
    return false;
  }
  Master* const master = this->network_->getMaster(masterId);
  if (master == nullptr || target->getMaster() == nullptr
      || target->getWidth().v != new_pcell.getWidth().getStorage()
      || target->getHeight().v != new_pcell.getHeight().getStorage()) {
    return false;
  }

  // Throw-away node anchored at the target's current coordinates.
  Node probe;
  Point2D origin(UvDist{target->getLeft().v}, UvDist{target->getBottom().v});
  initTempNode(probe, master, new_pcell, origin);

  const GridX x = grid_->gridX(probe.getLeft());
  const GridY y = grid_->gridSnapDownY(probe.getBottom());
  // isLegal is a one-to-one in-place std-cell replacement. The old Network
  // node is the complete input overlay; filler repair may only return changes
  // for surrounding fillers.
  std::vector<CellChangeRecord> overlayChanges;
  overlayChanges.push_back(CellChangeRecord{
      OpType::Delete, target->getDbInst(), UvDist(0), UvDist(0),
      LibCellID(), LibCellID(), PhysOrientationE::R0});

  const auto orient
      = grid_->getSiteOrientation(x, y, new_pcell.getTechSite()->getName()).value();
  return drc_engine_->checkDRC(&probe, x, y, orient, cellChanges, overlayChanges);
}

/**
 * @brief Check DRC legality of swapping a cell to a given master at its current
 *        location.
 *
 * Builds a temporary same-footprint node and checks it against an overlay of
 * the committed cell. The cell is not moved or mutated.
 *
 * @param  instId    LeafCellID of the cell to test.
 * @param  masterId  Target library cell master to test-swap to.
 * @param  ccRecords [out] Atomic surrounding-filler Replace records when
 *                    repair succeeds; unchanged on failure.
 * @retval true      The swap is DRC-legal at the current location.
 * @retval false     The swap violates DRC (insufficient space or
 * incompatible footprint).
 */
bool DePlace::isLegal(LeafCellID instId, LibCellID masterId,
    std::vector<CellChangeRecord>& ccRecords)
{
  Node* cell = this->network_->getNode(instId);
  if (cell == nullptr) {
    return false;
  }
  return isLegalProbe(masterId, cell, ccRecords);
}

/**
 * @brief Apply a batch of CellChangeRecord to the design.
 *
 * Iterates over @p ccRecords and performs each operation:
 *    - OpType::Replace: swaps the cell's master via NlEditor sizeCell change,
 *      then updates the in-memory Node.
 *    - OpType::Delete: removes an existing cell.
 *    - OpType::Add: currently ignored by this generic commit path.
 *
 * @param ccRecords  Vector of CellChangeRecord describing the changes.
 * @return true on success.
 */
bool DePlace::commit(const std::vector<CellChangeRecord>& ccRecords)
{
  // for increasing coverage
  eUNL::NlEditor editor(design_);
  // eUNL::HierID hID = design_->getCurHier().getId();
  for (const CellChangeRecord& cellChange : ccRecords) {
    switch (cellChange.op_)
    {
    case OpType::Replace:
    {
      const eLIB::LibCell& oldLibCell = design_->getLibAcc().getLibCell(
          cellChange.orig_lib_cell_);
      eFNL::ModuleID oldModId = oldLibCell.getMaster();

      const eLIB::LibCell& newLibCell = design_->getLibAcc().getLibCell(
          cellChange.new_lib_cell_);
      eFNL::ModuleID newModId = newLibCell.getMaster();
      eUNL::UnlChange_sizeCell sizeCell(&editor, *design_,
          eUNL::UnlChangePhaseE::PRE_CHANGE,
          std::get<LeafCellID>(cellChange.cell_data_), oldModId, newModId);
      assert(sizeCell.feasible());
      sizeCell.commit();
      // update in-memory data.
      Node* cell = this->network_->getNode(std::get<LeafCellID>(
          cellChange.cell_data_));
      network_->updateNode(
          cell, desMgr_,
          design_->getLibAcc().getPhysLibCell(cellChange.new_lib_cell_));
      break;
    }

    case OpType::Delete:
    {
      eUNL::UnlChange_removeCell removeCell(&editor,
          *design_, eUNL::UnlChangePhaseE::PRE_CHANGE,
          std::get<LeafCellID>(cellChange.cell_data_));
      assert(removeCell.feasible());
      removeCell.commit();
      // update in-memory data.
      Node* cell = this->network_->getNode(std::get<LeafCellID>(
          cellChange.cell_data_));
      network_->deleteNode(cell);
      break;
    }

    case OpType::Add:
    {
    }

    default:
      break;
    }

  }
  return true;
}

/**
 * @brief Compute the bounding box of an operable region.
 *
 * Currently a stub — returns an empty/default Rect.
 * TODO: Implement actual bounding-box computation over @p operableRect.
 *
 * @param operableRect  Input region to compute the bounding box for.
 * @return The bounding box as a Rect.
 */
Rect DePlace::getBoundingBox(const Rect& operableRect)
{
  return grid_->getBoundingBox(operableRect);
}

void DePlace::initTempNode(Node& cell, Master* master,
                           const PhysLibCell& pcell,
                           const Point2D& origin) const
{
  // Throw-away Node, deliberately NOT inserted into the Network and never
  // painted onto the grid, so no in-memory placement state is mutated.
  cell.setMaster(master);
  cell.setType(master->isFiller() ? Node::FILLER : Node::CELL);
  cell.setWidth(DbuX{pcell.getWidth().getStorage()});
  cell.setHeight(DbuY{pcell.getHeight().getStorage()});
  cell.setOrient(PhysOrientationE::R0);
  cell.setFixed(false);
  cell.setPlaced(false);
  cell.setLeft(DbuX{origin.getX().getStorage()});
  cell.setBottom(DbuY{origin.getY().getStorage()});
  // Bind the throw-away node to the instance occupying the origin site (if
  // any), so checkers that resolve names via getDbInst() have a valid id.
  const GridX origin_gx = grid_->gridX(cell.getLeft());
  const GridY origin_gy = grid_->gridSnapDownY(cell.getBottom());
  const Pixel* origin_pixel = grid_->gridPixel(origin_gx, origin_gy);
  if (origin_pixel != nullptr && origin_pixel->cell != nullptr) {
    cell.setDbInst(origin_pixel->cell->getDbInst());
  }
}

/**
 * @brief Find legal placement for a new cell at a specific pin's location.
 *
 * Creates a temporary Node for @p masterId at the pin's origin, then searches
 * a @p diameter-extended bounding box for a DRC-legal site.  If a @p va
 * (voltage area) is given, the search is restricted to the intersection
 * of the extended box with each VA rect.
 *
 * The temporary Node is discarded after the search; the returned coordinate
 * can be used by the caller to place the cell permanently.
 *
 * @param startLoc  Pin whose physical location anchors the search center.
 * @param va        Optional voltage-area to restrict placement to.
 * @param diameter  Search region extension (in uv units).
 * @param masterId  Target library cell master to place.
 * @param ccRecords [out] Atomic surrounding-filler Replace records for the
 *                  selected candidate; unchanged when no legal site exists.
 * @return The (left, bottom) UV coordinates of the found site,
 * or (-1, -1) on failure.
 */
std::pair<int, int> DePlace::findLeg(eUNL::PinID startLoc,
                                     eUNL::VoltageArea* va,
                                     int diameter,
                                     LibCellID masterId,
                                     std::vector<CellChangeRecord>& ccRecords)
{
  const PhysLibCell& new_pcell_ = design_->getLibAcc().getPhysLibCell(masterId);
  const PhysPin& pin = desMgr_->getPhysPin(eUNL::PhysPinID(startLoc.asFlatPin()));
  Point2D origin = (*pin.getTermIter().begin()).getOrigin();

  // Reuse the registered master if present; otherwise register it (same
  // semantics as the existing findLeg overloads).  The master itself must be
  // reachable from the node because isFiller()/checkers read Master fields.
  if (!prepareFillerRepair(new_pcell_)) {
    return {-1, -1};
  }
  Master* const master = this->network_->getMaster(masterId);
  if (master == nullptr) {
    return {-1, -1};
  }

  // Build a throw-away Node (not inserted into the network, never painted).
  Node cell;
  initTempNode(cell, master, new_pcell_, origin);

  // Search box anchored at the cell origin (same as the original overload).
  eUTL::Rect rect(eUTL::UvDist(cell.getLeft().v - diameter), \
                  eUTL::UvDist(cell.getBottom().v - diameter), \
                  eUTL::UvDist(cell.getLeft().v + cell.getWidth().v + diameter), \
                  eUTL::UvDist(cell.getBottom().v + cell.getHeight().v + diameter));

  std::cout << "[findLeg] target master id=" << masterId.getValue()
        // << " name=" << master->getMasterName()
        << " w=" << new_pcell_.getWidth().getStorage()
        << " h=" << new_pcell_.getHeight().getStorage()
        << " anchor origin=(" << origin.getX().getStorage()
        << "," << origin.getY().getStorage() << ")\n"
        << "[findLeg] search rect xl=" << rect.getXL().getStorage()
        << " xh=" << rect.getXH().getStorage()
        << " yl=" << rect.getYL().getStorage()
        << " yh=" << rect.getYH().getStorage()
        << " diameter=" << diameter << "\n";

  // legalCellInRect() performs the search.  The temporary node is not placed
  // / painted (setPlaced(false) above), so its unplaceCell() is a no-op and
  // the node is never inserted into the grid or network; the rip-up fallback
  // inside legalCellInRect() is disabled.
  Node* search_cell = &cell;
  std::pair<int, int> Coordinate;
  // checkPixels() accepts only an exact-cover filler at each candidate and
  // builds that one filler Delete overlay internally. ccRecords receives only
  // surrounding filler Replace records from the accepted candidate.
  if (va == nullptr) {
    Coordinate = legalCellInRect(rect, search_cell, &ccRecords);
  } else {
    for (Rect va_rect : va->getRects()) {
      Coordinate = legalCellInRect(rect.overlap(va_rect, false),
          search_cell, &ccRecords);
      if (Coordinate != std::pair<int, int>{-1, -1}) {
        break;
      }
    }
  }

  // No post-filter here: the overlay DRC runs inside checkPixels() at the end
  // of every candidate-site test during the search, so a non-{-1,-1}
  // coordinate is already DRC-clean against the real neighbours.  ccRecords is
  // the caller-supplied filler change list passed through unchanged.
  if (Coordinate != std::pair<int, int>{-1, -1}) {
    Coordinate = std::make_pair(Coordinate.first * grid_->getSiteWidth().v,
                                grid_->gridYToDbu(GridY{Coordinate.second}).v);
  }
  return Coordinate;
}

Rect DePlace::getCoreArea()
{
  std::vector<PhysRow> rows;
  DbuRect rect(Rect(UvDist(INT_MAX), UvDist(INT_MAX),
      UvDist(INT_MIN), UvDist(INT_MIN)));
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

// Asking isTerminal() rather than listing CELL and FILLER also keeps grid
// occupancy independent of the filler/non-filler classification, which
// Network::updateNode does not refresh after a master swap.
void DePlace::paintGridCell(Node* cell)
{
  grid_->visitCellPixels(cell, false, [&](Pixel* pixel, bool padded) {
    if (padded) {
      pixel->padding_reserved_by = cell;
    } else {
      setGridCell(cell, pixel);
    }
  });
}

void DePlace::setFixedGridCells()
{
  for (auto& [nid, cell] : network_->getNodes()) {
    if (!cell->isTerminal() && cell->isFixed()) {
      paintGridCell(cell.get());
    }
  }
}

void DePlace::setPlacedGridCells()
{
  for (auto& [nid, cell] : network_->getNodes()) {
    if (!cell->isTerminal() && cell->isPlaced()) {
      paintGridCell(cell.get());
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

// =============================================================================
// Stub implementations for Place.cpp - to be implemented later
// =============================================================================

DbuPt DePlace::nearestPt(const Node* cell, const DbuRect& rect) const
{
  // TODO: Implement nearest point calculation
  DbuX nearest_x = cell->getLeft();
  DbuY nearest_y = cell->getBottom();

  if (nearest_x < rect.xl) nearest_x = rect.xl;
  else if (nearest_x > rect.xh) nearest_x = rect.xh;

  if (nearest_y < rect.yl) nearest_y = rect.yl;
  else if (nearest_y > rect.yh) nearest_y = rect.yh;

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
        //     DPL, 42, "No cells found in group {}. ", group->getName());
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
          pixel->util -= ((site_width - rect.xh) % site_width).v
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

} // namespace dpl2
