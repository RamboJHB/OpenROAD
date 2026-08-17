// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors
#include <dpl2/DePlace.h>
#include <infrastructure/Grid.h>
#include <infrastructure/network.h>
#include <infrastructure/Padding.h>
#include <infrastructure/fillerSetting.h>
#include <PlacementDRC.h>
#include <drc/ImplantLayerChecker.h>
#include <fillerRepair/FillerRepairEngine.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>

namespace dpl2 {

namespace {

bool fillerRepairVerbose()
{
  const char* const value = std::getenv("FR_VERBOSE");
  return value == nullptr || std::string_view(value) != "0";
}

bool fillerRepairInitFailure(std::string_view stage, std::string_view reason)
{
  if (fillerRepairVerbose()) {
    std::cerr << "\n[fr][deplace] Filler repair initialization failed\n"
              << "  stage  : " << stage << "\n"
              << "  reason : " << reason << "\n";
  }
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
  // [FRPORT] [fillerRepair-fix] Network is filler repair's single infrastructure
  // seam; checker no longer needs a DePlace-specific provider.
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
  // [FRPORT] [fillerRepair-fix] Network borrows the setting; DePlace owns both.
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

// [FRPORT] Register configured filler masters and the std-cell master universe
// that opto may propose through DePlace, so all receive the same real edge-table
// data before the immutable checker/engine pair is constructed.
bool DePlace::registerFillerRepairMasters(
    const std::vector<const PhysLibCell*>& target_masters)
{
  if (filler_setting_ == nullptr || network_ == nullptr || grid_ == nullptr
      || edge_type_table_ == nullptr) {
    return fillerRepairInitFailure(
        "master registration",
        "DePlace requires fillerSetting, Network, Grid, and EdgeTypeTable");
  }

  // Once published, the engine's catalog and placement snapshot are
  // immutable. Never mutate Network behind active checker workers.
  if (filler_repair_engine_ != nullptr) {
    if (network_->getMasters().size() != filler_repair_master_count_
        || filler_setting_->getFillerCells() != filler_repair_filler_ids_) {
      return fillerRepairInitFailure(
          "revision validation",
          "Network masters or configured filler IDs changed after publication");
    }
    for (const PhysLibCell* master : target_masters) {
      if (master == nullptr
          || network_->getMaster(master->getLibCellId()) == nullptr) {
        return fillerRepairInitFailure(
            "revision validation",
            "a requested target master is null or was not registered before "
            "engine construction");
      }
    }
    return true;
  }

  // [fillerRepair-fix] Keep the binding current if the owner replaced its
  // setting before rebuilding configured filler masters.
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
        "the configured filler-master list is empty or contains null");
  }

  for (const PhysLibCell* master : masters) {
    if (network_->addMaster(*master,
                            *filler_setting_,
                            grid_.get(),
                            edge_type_table_.get()) == nullptr) {
      return fillerRepairInitFailure(
          "filler registration",
          std::string("Network::addMaster rejected configured filler master \"")
              + master->getLibCell().getName() + "\"");
    }
  }

  for (const PhysLibCell* master : target_masters) {
    if (master == nullptr) {
      return fillerRepairInitFailure("target registration",
                                     "the target-master list contains null");
    }
    if (filler_setting_->isFillerCell(master->getLibCellId())) {
      return fillerRepairInitFailure(
          "target registration",
          std::string("target master \"")
              + master->getLibCell().getName()
              + "\" is configured as a filler");
    }
    if (network_->addMaster(*master,
                            *filler_setting_,
                            grid_.get(),
                            edge_type_table_.get()) == nullptr) {
      return fillerRepairInitFailure(
          "target registration",
          std::string("Network::addMaster rejected target master \"")
              + master->getLibCell().getName() + "\"");
    }
  }

  for (const auto& node : network_->getNodes()) {
    if (node != nullptr && node->getMaster() != nullptr
        && filler_setting_->isFillerCell(
            node->getMaster()->getDbMaster())) {
      node->setType(Node::FILLER);
    }
  }
  return true;
}

// [FRPORT] Build the one revision-scoped checker/engine chain owned by
// DePlace. Publication is complete before any pointer becomes visible to
// isLegal/findLeg workers.
bool DePlace::initializeFillerRepair(
    const std::vector<const PhysLibCell*>& target_masters)
{
  std::lock_guard<std::mutex> lock(filler_repair_init_mutex_);
  if (filler_repair_engine_ != nullptr) {
    return registerFillerRepairMasters(target_masters)
           && filler_repair_engine_->isReady();
  }
  if (!registerFillerRepairMasters(target_masters)) {
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
        "the immutable placement/checker snapshot is not ready");
    if (fillerRepairVerbose()) {
      for (const ipl::Diagnostic& diagnostic : engine->getInitDiagnostics()) {
        std::cerr << "  diagnostic\n"
                  << "    status  : " << diagnostic.status << "\n"
                  << "    message : " << diagnostic.message << "\n";
      }
    }
    return false;
  }
  if (!checker->setFillerRepairEngine(engine.get())) {
    return fillerRepairInitFailure(
        "checker binding",
        "ImplantLayerChecker rejected the ready engine");
  }

  ipl::ImplantLayerChecker* const published_checker = checker.get();
  filler_repair_engine_ = std::move(engine);
  drc_engine_->addChecker(DRCCheckerType::ImplantLayer, std::move(checker));
  implant_layer_checker_ = published_checker;
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
    const CellChangeRecord& target_change,
    std::vector<CellChangeRecord>& fcRecord) const
{
  return isFillerRepairReady()
         && implant_layer_checker_->repair(target_change, fcRecord);
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
 * @brief Finds a repairable placement without changing shared placement data.
 *
 * Given a leaf cell and a target module name, this overload:
 *    1. Resolves @p moduleName to a PhysLibCell via LibObjAccessor,
 *    2. Requires the master in the immutable repair revision,
 *    3. Evaluates a fixed-origin Replace for an existing cell, or searches a
 *       diameter-limited Add for a new buffer not present in Network.
 *
 * No Network, Grid, or UDM state is changed. Use the repair-aware overload to
 * receive changes; the legacy overload fails when changes would be required.
 *
 * @param cellId     The leaf cell to remap and place.
 * @param diameter   Search region extension (in uv units)
 * around the cell's original bbox.
 * @param moduleName Target module name whose
 * PhysLibCell replaces the cell's current master.
 * @return The (left, bottom) UV coordinates
 * of the placed cell, or (-1, -1) on failure.
 */
std::pair<int, int> DePlace::findLeg(LeafCellID cellId, int diameter,
    std::string moduleName)
{
  std::vector<CellChangeRecord> ignored;
  const std::pair<int, int> result
      = findLeg(cellId, diameter, std::move(moduleName), ignored);
  return ignored.empty() ? result : std::pair<int, int>{-1, -1};
}
/**
 * @brief Whole-core form of the non-mutating repair-aware search.
 *
 * This is the core-area overload: it searches the entire core area
 * (this->core_) for a DRC-legal position.
 *
 * Same transaction semantics as the diameter overload; it never writes the
 * dpl2 cache or UDM.
 *
 * @param cellId     The leaf cell to remap and place.
 * @param moduleName Target module name whose PhysLibCell
 * replaces the cell's current master.
 * @return The (left, bottom) UV coordinates of the
 * placed cell, or (-1, -1) on failure.
 */
std::pair<int, int> DePlace::findLeg(LeafCellID cellId, std::string moduleName)
{
  std::vector<CellChangeRecord> ignored;
  const std::pair<int, int> result
      = findLeg(cellId, std::move(moduleName), ignored);
  return ignored.empty() ? result : std::pair<int, int>{-1, -1};
}

std::pair<int, int> DePlace::findLeg(
    LeafCellID cellId,
    int diameter,
    std::string moduleName,
    std::vector<CellChangeRecord>& fcRecord)
{
  return findLegImpl(cellId, diameter, moduleName, fcRecord);
}

std::pair<int, int> DePlace::findLeg(
    LeafCellID cellId,
    std::string moduleName,
    std::vector<CellChangeRecord>& fcRecord)
{
  return findLegImpl(cellId, -1, moduleName, fcRecord);
}

std::pair<int, int> DePlace::findLegImpl(
    LeafCellID cellId,
    int diameter,
    const std::string& moduleName,
    std::vector<CellChangeRecord>& fcRecord)
{
  if (!isFillerRepairReady() || design_ == nullptr || desMgr_ == nullptr
      || grid_ == nullptr || network_ == nullptr || diameter < -1) {
    return {-1, -1};
  }
  const eFNL::ModuleID moduleId = design_->getLibAcc().findModule(moduleName);
  const auto* libCell = design_->getLibAcc().getLibCell(moduleId);
  if (libCell == nullptr) {
    return {-1, -1};
  }
  const PhysLibCell& master
      = design_->getLibAcc().getPhysLibCell(libCell->getId());
  if (network_->getMaster(master.getLibCellId()) == nullptr) {
    return {-1, -1};
  }

  // Existing std cells retain their immutable origin. Filler Delete/Add is
  // not used to hide a std-cell move or footprint change.
  if (const Node* cell = network_->getNode(cellId); cell != nullptr) {
    std::vector<CellChangeRecord> trial;
    if (!isLegal(cellId, master.getLibCellId(), trial)) {
      return {-1, -1};
    }
    fcRecord.insert(fcRecord.end(),
                    std::make_move_iterator(trial.begin()),
                    std::make_move_iterator(trial.end()));
    return {cell->getLeft().v, cell->getBottom().v};
  }

  const eUNL::PhysCell physical = desMgr_->getPhysCell(cellId);
  if (!physical.isValid()) {
    return {-1, -1};
  }
  const Rect core = grid_->getCore();
  CellChangeRecord target_add{
      OpType::Add,
      CellData{"findLeg_buffer"},
      physical.getOrigin().getX(),
      physical.getOrigin().getY(),
      LibCellID{},
      master.getLibCellId(),
      physical.getOrient()};
  std::vector<CellChangeRecord> trial;
  if (!findLegal(target_add, diameter, trial)) {
    return {-1, -1};
  }
  fcRecord.insert(fcRecord.end(),
                  std::make_move_iterator(trial.begin()),
                  std::make_move_iterator(trial.end()));
  return {static_cast<int>(target_add.x_.getStorage()
                           - core.getXL().getStorage()),
          static_cast<int>(target_add.y_.getStorage()
                           - core.getYL().getStorage())};
}

bool DePlace::findLegal(CellChangeRecord& target_add,
                        int diameter,
                        std::vector<CellChangeRecord>& fcRecord) const
{
  const std::string* const target_name
      = std::get_if<std::string>(&target_add.cell_data_);
  if (!isFillerRepairReady() || target_add.op_ != OpType::Add
      || target_name == nullptr || target_name->empty()
      || !target_add.new_lib_cell_.isValid() || grid_ == nullptr
      || network_ == nullptr || target_add.orig_lib_cell_.isValid()) {
    return false;
  }
  const int master_id = network_->getMasterId(target_add.new_lib_cell_);
  const Master* const registered = network_->getMaster(master_id);
  if (registered == nullptr || registered->getPhysLibCell() == nullptr
      || registered->isFiller()) {
    return false;
  }

  const Rect core = grid_->getCore();
  const int64_t x_relative
      = target_add.x_.getStorage() - core.getXL().getStorage();
  const int64_t y_relative
      = target_add.y_.getStorage() - core.getYL().getStorage();
  if (x_relative < std::numeric_limits<int>::min()
      || x_relative > std::numeric_limits<int>::max()
      || y_relative < std::numeric_limits<int>::min()
      || y_relative > std::numeric_limits<int>::max()) {
    return false;
  }
  const GridX preferred_x
      = grid_->gridX(DbuX{static_cast<int>(x_relative)});
  const GridY preferred_y
      = grid_->gridSnapDownY(DbuY{static_cast<int>(y_relative)});
  GridX x_min{0};
  GridX x_max = grid_->getRowSiteCount() - GridX{1};
  GridY y_min{0};
  GridY y_max = grid_->getRowCount() - GridY{1};
  if (diameter >= 0) {
    const int64_t search_radius = diameter;
    const int64_t x_limit = static_cast<int64_t>(
                                grid_->getRowSiteCount().v)
                            * grid_->getSiteWidth().v;
    const int64_t y_limit
        = grid_->gridYToDbu(grid_->getRowCount()).v;
    const int x_lo = static_cast<int>(
        std::clamp<int64_t>(x_relative - search_radius, 0, INT_MAX));
    const int x_hi = static_cast<int>(std::clamp<int64_t>(
        x_relative + search_radius, 0, std::min<int64_t>(x_limit, INT_MAX)));
    const int y_lo = static_cast<int>(
        std::clamp<int64_t>(y_relative - search_radius, 0, INT_MAX));
    const int y_hi = static_cast<int>(std::clamp<int64_t>(
        y_relative + search_radius, 0, std::min<int64_t>(y_limit, INT_MAX)));
    x_min = grid_->gridX(DbuX{x_lo});
    x_max = grid_->gridX(DbuX{x_hi});
    y_min = grid_->gridSnapDownY(DbuY{y_lo});
    y_max = grid_->gridSnapDownY(DbuY{y_hi});
  }

  std::vector<CellChangeRecord> trial;
  const std::pair<int, int> location
      = findLegalAdd(*target_name,
                     *registered->getPhysLibCell(),
                     preferred_x,
                     preferred_y,
                     x_min,
                     x_max,
                     y_min,
                     y_max,
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
  target_add.x_ = UvDist{core.getXL().getStorage() + location.first};
  target_add.y_ = UvDist{core.getYL().getStorage() + location.second};
  target_add.orientation_ = *orientation;
  fcRecord.insert(fcRecord.end(),
                  std::make_move_iterator(trial.begin()),
                  std::make_move_iterator(trial.end()));
  return true;
}

/**
 * @brief isLegal - repack a cell to a different cell type at itscurrent
 * location, then validate the new footprint for DRC legality
 *
 * Unplaces @p cellId, adds the PhysLibCell for @p lcId to the grid,
 * updates the node, and checks DRC. The cell is NOT moved; only its master
 * is swapped in place. This overload is designed for ECO scenarios where
 * the caller wants to know "can this cell be swapped to a differrent standard
 * cell without shifting its (left, bottom) coordinates."
 *
 * @param cellId     LeafCellID of the cell to repack.
 * @param lcId       LibCellID of the target library cell master.
 * @param fcRecord   The Filler Cell modify record in checkDRC.
 * @retval true      The repacked cell is DRC-legal at the current location.
 * @retval false     The repacked cell violates DRC
 * (likely insufficient space or incompatible master footprint).
 */
bool DePlace::isLegal(LeafCellID cellId, LibCellID lcId,
    std::vector<CellChangeRecord>& fcRecord) const
{
  if (!isFillerRepairReady() || drc_engine_ == nullptr || network_ == nullptr
      || grid_ == nullptr) {
    return false;
  }
  const Node* const cell = network_->getNode(cellId);
  Master* const master = network_->getMaster(lcId);
  if (cell == nullptr || !cell->isStdCell() || master == nullptr
      || master->getPhysLibCell() == nullptr || master->isFiller()) {
    return false;
  }

  // Request-local overlay only: no unplace, Network master swap, Grid paint,
  // UDM edit, or restore window. This makes parallel opto queries share one
  // immutable DePlace/checker/engine revision safely.
  Node candidate = *cell;
  candidate.setMaster(master);
  candidate.setType(Node::CELL);
  candidate.setWidth(
      DbuX{master->getPhysLibCell()->getWidth().getStorage()});
  candidate.setHeight(
      DbuY{master->getPhysLibCell()->getHeight().getStorage()});
  candidate.setBottomPower(master->getBottomPowerType());
  candidate.setTopPower(master->getTopPowerType());

  std::vector<CellChangeRecord> trial;
  if (!drc_engine_->checkDRC(&candidate,
                             grid_->gridX(cell),
                             grid_->gridRoundY(cell),
                             cell->getOrient(),
                             trial)) {
    return false;
  }
  fcRecord.insert(fcRecord.end(),
                  std::make_move_iterator(trial.begin()),
                  std::make_move_iterator(trial.end()));
  return true;
}

std::pair<int, int> DePlace::findLegalAdd(
    const std::string& target_name,
    const PhysLibCell& master,
    GridX preferred_x,
    GridY preferred_y,
    GridX x_min,
    GridX x_max,
    GridY y_min,
    GridY y_max,
    std::vector<CellChangeRecord>& fcRecord) const
{
  if (!isFillerRepairReady() || grid_ == nullptr
      || master.getTechSite() == nullptr || grid_->getSiteWidth().v <= 0) {
    return {-1, -1};
  }
  const int width_sites = std::max(
      1,
      (master.getWidth().getStorage() + grid_->getSiteWidth().v - 1)
          / grid_->getSiteWidth().v);
  const int height_rows = std::max(1, grid_->gridHeight(master).v);
  x_min.v = std::max(0, x_min.v);
  y_min.v = std::max(0, y_min.v);
  x_max.v = std::min(x_max.v, grid_->getRowSiteCount().v - width_sites);
  y_max.v = std::min(y_max.v, grid_->getRowCount().v - height_rows);
  if (x_min > x_max || y_min > y_max) {
    return {-1, -1};
  }
  preferred_x.v = std::clamp(preferred_x.v, x_min.v, x_max.v);
  preferred_y.v = std::clamp(preferred_y.v, y_min.v, y_max.v);

  const Rect core = grid_->getCore();
  const std::string& site_name = master.getTechSite()->getName();
  const int64_t max_radius
      = static_cast<int64_t>(x_max.v) - x_min.v
        + static_cast<int64_t>(y_max.v) - y_min.v;
  static constexpr int kMaxCheckerCandidates = 4096;
  static constexpr int kMaxExaminedSites = 65536;
  int checked_candidates = 0;
  int examined_sites = 0;
  for (int64_t radius = 0; radius <= max_radius; ++radius) {
    for (int64_t dy = -radius; dy <= radius; ++dy) {
      const int64_t dx = radius - std::abs(dy);
      const int64_t row = static_cast<int64_t>(preferred_y.v) + dy;
      const std::array<int64_t, 2> columns{
          static_cast<int64_t>(preferred_x.v) - dx,
          static_cast<int64_t>(preferred_x.v) + dx};
      for (int side = 0; side < (dx == 0 ? 1 : 2); ++side) {
        const int64_t col = columns[side];
        if (row < y_min.v || row > y_max.v || col < x_min.v
            || col > x_max.v) {
          continue;
        }
        if (examined_sites++ >= kMaxExaminedSites) {
          return {-1, -1};
        }
        const std::optional<PhysOrientation> orientation
            = grid_->getSiteOrientation(
                GridX{static_cast<int>(col)},
                GridY{static_cast<int>(row)},
                site_name);
        if (!orientation.has_value()) {
          continue;
        }
        if (checked_candidates++ >= kMaxCheckerCandidates) {
          return {-1, -1};
        }
        const int64_t x = core.getXL().getStorage()
                          + static_cast<int64_t>(col)
                                * grid_->getSiteWidth().v;
        const int64_t y = core.getYL().getStorage()
                          + grid_->gridYToDbu(
                                     GridY{static_cast<int>(row)})
                                .v;
        const CellChangeRecord target{
            OpType::Add,
            CellData{target_name},
            UvDist{x},
            UvDist{y},
            LibCellID{},
            master.getLibCellId(),
            *orientation};
        std::vector<CellChangeRecord> trial;
        if (!repairFillers(target, trial)) {
          continue;
        }
        fcRecord.insert(fcRecord.end(),
                        std::make_move_iterator(trial.begin()),
                        std::make_move_iterator(trial.end()));
        return {static_cast<int>(col) * grid_->getSiteWidth().v,
                grid_->gridYToDbu(GridY{static_cast<int>(row)}).v};
      }
    }
  }
  return {-1, -1};
}

Rect DePlace::getBoundingBox(const Rect& region, int rings) const
{
  return grid_->getBoundingBox(region, rings);
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

// [FRPORT] [fillerRepair-fix] Paints one node's footprint into the grid.
//
// The predicate is "does this node stand on sites", NOT "is it a standard
// cell". Both callers used to test `getType() == Node::CELL`, which drops
// every Node::FILLER -- Network::addNode types core fillers that way -- so on
// a fully filled design the sites the fillers occupy stayed empty in the grid
// and Grid::isFullUtil() reported false. Fillers are what makes a design
// full; they have to be painted like anything else standing on a row.
//
// Asking isTerminal() rather than listing CELL and FILLER also keeps grid
// occupancy independent of filler/non-filler type refreshes.
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
  for (auto& cell : network_->getNodes()) {
    if (!cell->isTerminal() && cell->isFixed()) {
      paintGridCell(cell.get());
    }
  }
}

void DePlace::setPlacedGridCells()
{
  for (auto& cell : network_->getNodes()) {
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
