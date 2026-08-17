// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors

#include <dpl2/DePlace.h>
#include <infrastructure/Grid.h>
#include <infrastructure/network.h>
#include <infrastructure/Padding.h>
#include <infrastructure/fillerSetting.h>
#include <drc/ImplantLayerChecker.h>
#include <fillerRepair/FillerRepairEngine.h>
#include <PlacementDRC.h>

#include <array>
#include <climits>
#include <cstdlib>
#include <iterator>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>

namespace dpl2 {

namespace {

bool fillerRepairInitFailure(std::string_view stage, std::string_view reason)
{
  std::cerr << "\n[fr][deplace] Filler repair initialization failed\n"
            << "  stage  : " << stage << "\n"
            << "  reason : " << reason << "\n";
  return false;
}

}  // namespace

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
  return registerFillerRepairMasters({});
}

bool DePlace::registerFillerRepairMasters(
    const std::vector<const PhysLibCell*>& targetMasters)
{
  if (filler_setting_ == nullptr || network_ == nullptr || grid_ == nullptr
      || edge_type_table_ == nullptr) {
    return fillerRepairInitFailure(
        "master registration",
        "DePlace requires fillerSetting, Network, Grid, and EdgeTypeTable");
  }

  // Once published, the engine and checker read an immutable catalog. Do not
  // mutate Network while worker checks may be running.
  if (filler_repair_engine_ != nullptr) {
    if (network_->getMasters().size() != filler_repair_master_count_
        || filler_setting_->getFillerCells() != filler_repair_filler_ids_) {
      return fillerRepairInitFailure(
          "revision validation",
          "master catalog or filler configuration changed after publication");
    }
    for (const PhysLibCell* master : targetMasters) {
      if (master == nullptr
          || network_->getMaster(master->getLibCellId()) == nullptr) {
        return fillerRepairInitFailure(
            "revision validation",
            "a target master was not registered before engine construction");
      }
    }
    return true;
  }

  network_->setFillerSetting(filler_setting_.get());
  const std::vector<const PhysLibCell*> masters
      = filler_setting_->getFillerPhysCells();
  if (masters.empty()
      || std::any_of(masters.begin(), masters.end(),
                     [](const PhysLibCell* master) {
                       return master == nullptr;
                     })) {
    return fillerRepairInitFailure(
        "filler registration",
        "configured filler-master list is empty or contains null");
  }

  for (std::size_t index = 0; index < masters.size(); ++index) {
    const PhysLibCell* master = masters[index];
    if (network_->addMaster(*master,
                            *filler_setting_,
                            grid_.get(),
                            edge_type_table_.get()) == nullptr) {
      return fillerRepairInitFailure(
          "filler registration",
          "Network::addMaster rejected configured filler index "
              + std::to_string(index));
    }
  }

  for (std::size_t index = 0; index < targetMasters.size(); ++index) {
    const PhysLibCell* master = targetMasters[index];
    if (master == nullptr) {
      return fillerRepairInitFailure(
          "target registration",
          "target-master list contains null at index "
              + std::to_string(index));
    }
    if (filler_setting_->isFillerCell(master->getLibCellId())) {
      return fillerRepairInitFailure(
          "target registration",
          "target master is also configured as filler at index "
              + std::to_string(index));
    }
    if (network_->addMaster(*master,
                            *filler_setting_,
                            grid_.get(),
                            edge_type_table_.get()) == nullptr) {
      return fillerRepairInitFailure(
          "target registration",
          "Network::addMaster rejected target index "
              + std::to_string(index));
    }
  }

  // addMaster refreshes an existing Master's classification, but Nodes keep
  // the type captured when they were imported. set_filler_option commonly
  // runs after that import, so synchronize placed instances as part of the
  // same infrastructure-owned registration step.
  for (const auto& [nid, node] : network_->getNodes()) {
    (void) nid;
    if (node != nullptr && node->getMaster() != nullptr
        && filler_setting_->isFillerCell(
            node->getMaster()->getDbMaster())) {
      node->setType(Node::FILLER);
    }
  }
  return true;
}

bool DePlace::initializeFillerRepair(
    const std::vector<const PhysLibCell*>& targetMasters)
{
  std::lock_guard<std::mutex> lock(filler_repair_init_mutex_);
  if (filler_repair_engine_ != nullptr) {
    return registerFillerRepairMasters(targetMasters)
           && filler_repair_engine_->isReady();
  }
  if (!registerFillerRepairMasters(targetMasters)) {
    return false;
  }
  if (drc_engine_ == nullptr) {
    initPlacementDRC();
  }
  if (drc_engine_ == nullptr) {
    return fillerRepairInitFailure(
        "PlacementDRC construction",
        "DePlace::initPlacementDRC did not create PlacementDRC");
  }

  auto checker = std::make_unique<ipl::ImplantLayerChecker>(
      grid_.get(), design_, network_.get());
  auto engine
      = std::make_unique<fillerRepair::FillerRepairEngine>(*checker);
  if (!engine->isReady()) {
    fillerRepairInitFailure(
        "FillerRepairEngine construction",
        "immutable placement/checker snapshot is not ready");
    for (const ipl::Diagnostic& diagnostic : engine->getInitDiagnostics()) {
      std::cerr << "  diagnostic\n"
                << "    status  : " << diagnostic.status << "\n"
                << "    message : " << diagnostic.message << "\n";
    }
    return false;
  }
  if (!checker->setFillerRepairEngine(engine.get())) {
    return fillerRepairInitFailure(
        "checker publication",
        "ImplantLayerChecker rejected the ready repair engine");
  }

  implant_layer_checker_ = checker.get();
  filler_repair_engine_ = std::move(engine);
  drc_engine_->addChecker(DRCCheckerType::ImplantLayer, std::move(checker));
  filler_repair_filler_ids_ = filler_setting_->getFillerCells();
  filler_repair_master_count_ = network_->getMasters().size();
  return true;
}

bool DePlace::isFillerRepairReady() const
{
  return implant_layer_checker_ != nullptr
         && filler_repair_engine_ != nullptr
         && filler_repair_engine_->isReady();
}

bool DePlace::repairFillers(
    const CellChangeRecord& targetChange,
    std::vector<CellChangeRecord>& fillerChanges) const
{
  return isFillerRepairReady()
         && implant_layer_checker_->repair(targetChange, fillerChanges);
}

// [FRPORT] Read-only Add search. Each candidate is evaluated through the
// published checker -> engine entry; only a complete filler transaction is
// appended, and no Grid/Network/UDM object is mutated.
bool DePlace::findLegal(
    CellChangeRecord& targetAdd,
    int diameter,
    std::vector<CellChangeRecord>& fillerChanges) const
{
  const std::string* const targetName
      = std::get_if<std::string>(&targetAdd.cell_data_);
  if (!isFillerRepairReady() || targetAdd.op_ != OpType::Add
      || targetName == nullptr || targetName->empty()
      || !targetAdd.new_lib_cell_.isValid() || targetAdd.orig_lib_cell_.isValid()
      || grid_ == nullptr || network_ == nullptr || diameter < -1) {
    return false;
  }
  const Master* const registered
      = network_->getMaster(targetAdd.new_lib_cell_);
  if (registered == nullptr || registered->getPhysLibCell() == nullptr
      || registered->isFiller()) {
    return false;
  }

  const Rect core = grid_->getCore();
  const int64_t xRelative
      = targetAdd.x_.getStorage() - core.getXL().getStorage();
  const int64_t yRelative
      = targetAdd.y_.getStorage() - core.getYL().getStorage();
  if (xRelative < std::numeric_limits<int>::min()
      || xRelative > std::numeric_limits<int>::max()
      || yRelative < std::numeric_limits<int>::min()
      || yRelative > std::numeric_limits<int>::max()) {
    return false;
  }

  const GridX preferredX
      = grid_->gridX(DbuX{static_cast<int>(xRelative)});
  const GridY preferredY
      = grid_->gridSnapDownY(DbuY{static_cast<int>(yRelative)});
  GridX xMin{0};
  GridX xMax = grid_->getRowSiteCount() - GridX{1};
  GridY yMin{0};
  GridY yMax = grid_->getRowCount() - GridY{1};
  if (diameter >= 0) {
    const int64_t radius = diameter;
    const int64_t xLimit
        = static_cast<int64_t>(grid_->getRowSiteCount().v)
          * grid_->getSiteWidth().v;
    const int64_t yLimit = grid_->gridYToDbu(grid_->getRowCount()).v;
    const int xLo = static_cast<int>(
        std::clamp<int64_t>(xRelative - radius, 0, INT_MAX));
    const int xHi = static_cast<int>(std::clamp<int64_t>(
        xRelative + radius, 0, std::min<int64_t>(xLimit, INT_MAX)));
    const int yLo = static_cast<int>(
        std::clamp<int64_t>(yRelative - radius, 0, INT_MAX));
    const int yHi = static_cast<int>(std::clamp<int64_t>(
        yRelative + radius, 0, std::min<int64_t>(yLimit, INT_MAX)));
    xMin = grid_->gridX(DbuX{xLo});
    xMax = grid_->gridX(DbuX{xHi});
    yMin = grid_->gridSnapDownY(DbuY{yLo});
    yMax = grid_->gridSnapDownY(DbuY{yHi});
  }

  std::vector<CellChangeRecord> trial;
  const std::pair<int, int> location
      = findLegalAdd(*targetName,
                     *registered->getPhysLibCell(),
                     preferredX,
                     preferredY,
                     xMin,
                     xMax,
                     yMin,
                     yMax,
                     trial);
  if (location.first < 0 || location.second < 0) {
    return false;
  }
  const GridX col = grid_->gridX(DbuX{location.first});
  const GridY row = grid_->gridSnapDownY(DbuY{location.second});
  const std::optional<PhysOrientation> orientation
      = grid_->getSiteOrientation(
          col, row, registered->getPhysLibCell()->getTechSite()->getName());
  if (!orientation.has_value()) {
    return false;
  }
  targetAdd.x_ = UvDist{core.getXL().getStorage() + location.first};
  targetAdd.y_ = UvDist{core.getYL().getStorage() + location.second};
  targetAdd.orientation_ = *orientation;
  fillerChanges.insert(fillerChanges.end(),
                       std::make_move_iterator(trial.begin()),
                       std::make_move_iterator(trial.end()));
  return true;
}

std::pair<int, int> DePlace::findLegalAdd(
    const std::string& targetName,
    const PhysLibCell& master,
    GridX preferredX,
    GridY preferredY,
    GridX xMin,
    GridX xMax,
    GridY yMin,
    GridY yMax,
    std::vector<CellChangeRecord>& fillerChanges) const
{
  if (!isFillerRepairReady() || grid_ == nullptr
      || master.getTechSite() == nullptr || grid_->getSiteWidth().v <= 0) {
    return {-1, -1};
  }
  const int widthSites = std::max(
      1,
      (master.getWidth().getStorage() + grid_->getSiteWidth().v - 1)
          / grid_->getSiteWidth().v);
  const int heightRows = std::max(1, grid_->gridHeight(master).v);
  xMin.v = std::max(0, xMin.v);
  yMin.v = std::max(0, yMin.v);
  xMax.v = std::min(xMax.v, grid_->getRowSiteCount().v - widthSites);
  yMax.v = std::min(yMax.v, grid_->getRowCount().v - heightRows);
  if (xMin > xMax || yMin > yMax) {
    return {-1, -1};
  }
  preferredX.v = std::clamp(preferredX.v, xMin.v, xMax.v);
  preferredY.v = std::clamp(preferredY.v, yMin.v, yMax.v);

  const Rect core = grid_->getCore();
  const std::string& siteName = master.getTechSite()->getName();
  const int64_t maxRadius
      = static_cast<int64_t>(xMax.v) - xMin.v
        + static_cast<int64_t>(yMax.v) - yMin.v;
  static constexpr int kMaxCheckerCandidates = 4096;
  static constexpr int kMaxExaminedSites = 65536;
  int checkedCandidates = 0;
  int examinedSites = 0;
  for (int64_t radius = 0; radius <= maxRadius; ++radius) {
    for (int64_t dy = -radius; dy <= radius; ++dy) {
      const int64_t dx = radius - std::abs(dy);
      const int64_t row = static_cast<int64_t>(preferredY.v) + dy;
      const std::array<int64_t, 2> columns{
          static_cast<int64_t>(preferredX.v) - dx,
          static_cast<int64_t>(preferredX.v) + dx};
      for (int side = 0; side < (dx == 0 ? 1 : 2); ++side) {
        const int64_t col = columns[side];
        if (row < yMin.v || row > yMax.v || col < xMin.v || col > xMax.v) {
          continue;
        }
        if (examinedSites++ >= kMaxExaminedSites) {
          return {-1, -1};
        }
        const std::optional<PhysOrientation> orientation
            = grid_->getSiteOrientation(GridX{static_cast<int>(col)},
                                        GridY{static_cast<int>(row)},
                                        siteName);
        if (!orientation.has_value()) {
          continue;
        }
        if (checkedCandidates++ >= kMaxCheckerCandidates) {
          return {-1, -1};
        }
        const int64_t x
            = core.getXL().getStorage()
              + col * grid_->getSiteWidth().v;
        const int64_t y
            = core.getYL().getStorage()
              + grid_->gridYToDbu(GridY{static_cast<int>(row)}).v;
        const CellChangeRecord target{OpType::Add,
                                      CellData{targetName},
                                      UvDist{x},
                                      UvDist{y},
                                      LibCellID{},
                                      master.getLibCellId(),
                                      *orientation};
        std::vector<CellChangeRecord> trial;
        if (!repairFillers(target, trial)) {
          continue;
        }
        fillerChanges.insert(fillerChanges.end(),
                             std::make_move_iterator(trial.begin()),
                             std::make_move_iterator(trial.end()));
        return {static_cast<int>(col) * grid_->getSiteWidth().v,
                grid_->gridYToDbu(GridY{static_cast<int>(row)}).v};
      }
    }
  }
  return {-1, -1};
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
 * node plus an overlay of the cells the new footprint displaces (the target
 * std-cell being swapped in place, and any fillers the new footprint covers)
 * to the single-entry DRC check, and returns the verdict.  No in-memory
 * placement state is mutated.
 *
 * @param  masterId    Target library cell master.
 * @param  target      The std-cell currently occupying the probe site.
 * @param  cellChanges [in/out] Caller's filler change list, carried through for
 *                     the ImplantLayer checker (reserved).
 * @retval true        The swap is DRC-legal at the current location.
 * @retval false       The swap violates DRC.
 */
bool DePlace::isLegalProbe(LibCellID masterId, const Node* target,
                           std::vector<CellChangeRecord>& cellChanges)
{
  if (!isFillerRepairReady() || target == nullptr || drc_engine_ == nullptr) {
    return false;
  }
  const PhysLibCell& new_pcell = design_->getLibAcc().getPhysLibCell(masterId);

  // Target masters are registered before the immutable engine is published.
  Master* master = this->network_->getMaster(masterId);
  if (master == nullptr) {
    return false;
  }

  // Throw-away node anchored at the target's current coordinates.
  Node probe;
  Point2D origin(UvDist{target->getLeft().v}, UvDist{target->getBottom().v});
  initTempNode(probe, master, new_pcell, origin);
  probe.setId(target->getId());
  probe.setDbInst(target->getDbInst());

  const GridX x = grid_->gridX(probe.getLeft());
  const GridY y = grid_->gridSnapDownY(probe.getBottom());
  const GridX x_end = x + grid_->gridWidth(&probe);
  const GridY y_end
      = grid_->gridEndY(grid_->gridYToDbu(y) + probe.getHeight());

  // Overlay = cells the new footprint displaces: the target std-cell being
  // swapped in place plus any fillers covered by the (possibly larger) new
  // footprint.  The neighbour-reading checkers treat these as already
  // removed/replaced, without touching the grid.
  std::vector<CellChangeRecord> overlayChanges;
  std::set<Node*> collected;
  overlayChanges.push_back(CellChangeRecord{
      OpType::Delete, target->getDbInst(), UvDist(0), UvDist(0),
      LibCellID(), LibCellID(), PhysOrientationE::R0});
  collected.insert(const_cast<Node*>(target));
  for (GridY y1 = y; y1 < y_end; ++y1) {
    for (GridX x1 = x; x1 < x_end; ++x1) {
      const Pixel* pixel = grid_->gridPixel(x1, y1);
      if (pixel == nullptr || pixel->cell == nullptr) {
        continue;
      }
      Node* occupant = pixel->cell;
      if (!occupant->isFiller() || collected.count(occupant)) {
        continue;
      }
      collected.insert(occupant);
      overlayChanges.push_back(CellChangeRecord{
          OpType::Delete, occupant->getDbInst(), UvDist(0), UvDist(0),
          LibCellID(), LibCellID(), PhysOrientationE::R0});
    }
  }

  const auto orient
      = grid_->getSiteOrientation(x, y, new_pcell.getTechSite()->getName());
  return orient.has_value()
         && drc_engine_->checkDRC(
             &probe, x, y, *orient, cellChanges, overlayChanges);
}

/**
 * @brief Check DRC legality of swapping a cell to a given master at its current
 *        location.
 *
 * Builds a request-local Node with @p masterId at the existing origin and runs
 * the read-only checker/repair chain. Grid, Network, UDM, and the original
 * Node are never changed.
 *
 * @param  instId    LeafCellID of the cell to test.
 * @param  masterId  Target library cell master to test-swap to.
 * @param  ccRecords  [out] Filler-change records accumulated during DRC check
 * (reserved).
 * @retval true      The swap is DRC-legal at the current location.
 * @retval false     The swap violates DRC (insufficient space or
 * incompatible footprint).
 */
bool DePlace::isLegal(LeafCellID instId, LibCellID masterId,
    std::vector<CellChangeRecord>& ccRecords)
{
  if (!isFillerRepairReady()) {
    return false;
  }
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
 *    - OpType::Delete / Add: reserved for future use.
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
 * @param ccRecords  [out] Filler-change records accumulated during DRC check
 * (reserved).
 * @return The (left, bottom) UV coordinates of the found site,
 * or (-1, -1) on failure.
 */
std::pair<int, int> DePlace::findLeg(eUNL::PinID startLoc,
                                     eUNL::VoltageArea* va,
                                     int diameter,
                                     LibCellID masterId,
                                     std::vector<CellChangeRecord>& ccRecords)
{
  if (!isFillerRepairReady() || design_ == nullptr || desMgr_ == nullptr
      || grid_ == nullptr || network_ == nullptr || drc_engine_ == nullptr) {
    return {-1, -1};
  }
  if (diameter < 0) {
    return {-1, -1};
  }
  const PhysLibCell& new_pcell_ = design_->getLibAcc().getPhysLibCell(masterId);
  const PhysPin& pin = desMgr_->getPhysPin(eUNL::PhysPinID(startLoc.asFlatPin()));
  if (pin.getTermIter().empty()) {
    return {-1, -1};
  }
  Point2D origin = (*pin.getTermIter().begin()).getOrigin();

  // The immutable repair revision requires every opto target master up front.
  Master* master = this->network_->getMaster(masterId);
  if (master == nullptr) {
    return {-1, -1};
  }

  // Build a throw-away Node (not inserted into the network, never painted).
  Node cell;
  initTempNode(cell, master, new_pcell_, origin);
  cell.setId(-1);
  cell.setDbInst(LeafCellID{});

  // Search box anchored at the cell origin (same as the original overload).
  eUTL::Rect rect(eUTL::UvDist(cell.getLeft().v - diameter), \
                  eUTL::UvDist(cell.getBottom().v - diameter), \
                  eUTL::UvDist(cell.getLeft().v + cell.getWidth().v + diameter), \
                  eUTL::UvDist(cell.getBottom().v + cell.getHeight().v + diameter));

  // legalCellInRect() performs the search.  The temporary node is not placed
  // / painted (setPlaced(false) above), so its unplaceCell() is a no-op and
  // the node is never inserted into the grid or network; the rip-up fallback
  // inside legalCellInRect() is disabled.
  Node* search_cell = &cell;
  std::pair<int, int> Coordinate;
  // ccRecords carries the caller-supplied filler change list (for ImplantLayer).
  // The std-cell overlay (fillers a candidate site displaces) is built
  // internally by checkPixels() per candidate site, so only the filler list
  // needs to travel down the search chain.
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
    (void) nid;
    if (!cell->isTerminal() && cell->isFixed()) {
      paintGridCell(cell.get());
    }
  }
}

void DePlace::setPlacedGridCells()
{
  for (auto& [nid, cell] : network_->getNodes()) {
    (void) nid;
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
