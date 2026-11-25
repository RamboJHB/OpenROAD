// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors

#include <PlacementDRC.h>
#include <dpl2/DePlace.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Padding.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>
#include <tbb/task_arena.h>

#include <cstdio>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>

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
    // Checker construction reads row/site geometry from the initialized Grid.
    initPlacementDRC();
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

bool DePlace::isLegal(LeafCellID instId, LibCellID masterId,
                      std::vector<CellChangeRecord>& changes)
{
  return isLegal(instId, masterId, nullptr, 0, changes);
}

bool DePlace::isLegal(LeafCellID instId, LibCellID masterId,
                      eUNL::VoltageArea* va, int diameter,
                      std::vector<CellChangeRecord>& changes)
{
  constexpr std::pair<int, int> notFound{-1, -1};
  const Node* inst = network_ != nullptr ? network_->getNode(instId) : nullptr;
  return findLegToReplace(inst, va, diameter, masterId, changes,
                          /*tryDirectFirst=*/true) != notFound;
}

std::pair<int, int> DePlace::findLeg(const Node* inst, eUNL::VoltageArea* va,
                                     int diameter, LibCellID masterId,
                                     std::vector<CellChangeRecord>& ccRecords)
{
  return findLegToReplace(inst, va, diameter, masterId, ccRecords,
                          /*tryDirectFirst=*/false);
}

std::pair<int, int> DePlace::findLeg(eUNL::LeafCellID instId,
                                    eUNL::VoltageArea* va,
                                    int diameter,
                                    LibCellID masterId,
                                    std::vector<CellChangeRecord>& ccRecords,
                                    eUTL::PhysOrientation& rtOrient)
{
  Node* cell = this->network_->getNode(instId);
  if (cell == nullptr || cell->isFixed()) {
    return {-1, -1};
  }
  const PhysLibCell& new_pcell_ = design_->getLibAcc().getPhysLibCell(masterId);
  const size_t rollback_size = ccRecords.size();
  std::vector<CellChangeRecord> overlayChange = {};

  // 1) The planned delete: records only, no network mutation.
  overlayChange.push_back(CellChangeRecord{
      OpType::Delete, instId, UvDist{cell->getLeft().v}, UvDist{cell->getBottom().v},
      LibCellID(), LibCellID(), cell->getOrient()});

  // 2) Back-fill the released site so the implant-layer rule stays legal.
  //    The filler-insertion computation is owned by the implant layer's
  //    removal path (not by DePlace): check(nullptr, ...) computes the
  //    back-fill/repair records and appends them to ccRecords.  No
  //    network/grid mutation happens here.
  if (!drc_engine_->getChecker(DRCCheckerType::ImplantLayer)->check(nullptr,
      GridX{0}, GridY{0}, eUTL::PhysOrientationE::R0, ccRecords, overlayChange)) {
    ccRecords.resize(rollback_size);  // implant layer found no legal fill
    return {-1, -1};
  }

  // 2b) Publish the planned deletes (the ECO instance being removed, plus any
  //     filler the implant checker flagged for removal in overlayChange) into
  //     ccRecords BEFORE the site search.  The search (diamondSearch /
  //     checkPixels / the implant overlay DRC) treats every ccRecords Delete
  //     as already-removed via DRCChecker::isRemovedByChanges(): without these
  //     records the deleted std cell would be mistaken for an ordinary
  //     blocking std cell and the search would reject every site it used to
  //     occupy.  The rollback below rescues ccRecords to rollback_size, so the
  //     records published here are undone together with any probe records.
  ccRecords.insert(ccRecords.end(),
                   std::make_move_iterator(overlayChange.begin()),
                   std::make_move_iterator(overlayChange.end()));
  // 3) Target master + search, identical to findLeg().
  const Point2D origin(UvDist{cell->getLeft().v}, UvDist{cell->getBottom().v});
  Master* master = this->network_->addMaster(new_pcell_, *filler_setting_,
      this->grid_.get(), this->edge_type_table_.get());
  Node target;
  initTempNode(target, master, new_pcell_, origin);

  std::pair<int, int> result =
      findLegAtOrigin(origin, &target, va, diameter, ccRecords, rtOrient);
  if (result == std::pair<int, int>{-1, -1}) {
    // Roll back every record appended by this call so the caller's list
    // stays exactly as it was before the failed attempt.
    ccRecords.resize(rollback_size);
    return result;
  }

  return result;
}

std::pair<int, int> DePlace::findLegAtOrigin(const Point2D& origin,
                                            Node* search_cell,
                                            eUNL::VoltageArea* va,
                                            int diameter,
                                            std::vector<CellChangeRecord>& ccRecords,
                                            eUTL::PhysOrientation& rtOrient)
{
  eUTL::Rect rect(eUTL::UvDist(search_cell->getLeft().v - diameter),
                  eUTL::UvDist(search_cell->getBottom().v - diameter),
                  eUTL::UvDist(search_cell->getLeft().v + search_cell->getWidth().v
                    + diameter),
                  eUTL::UvDist(search_cell->getBottom().v +
                    search_cell->getHeight().v + diameter));

  // Value-initialize to the {-1, -1} failure sentinel: with a voltage area
  // whose rect list is empty the loop below never runs, and a default (0, 0)
  // would be indistinguishable from a real site at the core origin.
  std::pair<int, int> Coordinate{-1, -1};
  if (va == nullptr) {
    Coordinate = legalCellInRect(rect, search_cell, ccRecords, rtOrient);
  } else {
    for (Rect va_rect : va->getRects()) {
      Coordinate = legalCellInRect(rect.overlap(va_rect, false), search_cell,
          ccRecords, rtOrient);
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

std::pair<int, int> DePlace::findLegToReplace(
    const Node* old, eUNL::VoltageArea* va, int diameter, LibCellID masterId,
    std::vector<CellChangeRecord>& ccRecords, bool tryDirectFirst)
{
  constexpr std::pair<int, int> notFound{-1, -1};
  if (diameter < 0 || design_ == nullptr || network_ == nullptr
      || grid_ == nullptr || drc_engine_ == nullptr) {
    return notFound;
  }
  Master* const master = network_->getMaster(masterId);
  if (old == nullptr || network_->getNode(old->getDbInst()) != old
      || old->getMaster() == nullptr || old->isFiller()
      || old->isFixed() || old->isTerminal() || old->getGroup() != nullptr
      || master == nullptr || master->isFiller()
      || master->getPhysLibCell() == nullptr) {
    return notFound;
  }
  const PhysLibCell& physical = *master->getPhysLibCell();
  if (physical.getTechSite() == nullptr || physical.getType().isBlock()
      || physical.getType().isPad() || physical.getType().isCover()
      || physical.getWidth().getStorage() <= 0
      || physical.getHeight().getStorage() <= 0) {
    return notFound;
  }

  const GridX oldX = grid_->gridX(old);
  const GridY oldY = grid_->gridSnapDownY(old);
  Node probe;
  const Point2D origin{UvDist(old->getLeft().v), UvDist(old->getBottom().v)};
  initTempNode(probe, master, physical, origin);
  probe.setId(old->getId());
  probe.setDbInst(old->getDbInst());

  const bool sameFootprint
      = old->getWidth().v == physical.getWidth().getStorage()
        && old->getHeight().v == physical.getHeight().getStorage();
  std::vector<CellChangeRecord> trial;
  PhysOrientation rtOrient{PhysOrientationE::R0};
  auto location = notFound;
  // isLegal's same-size swap need not prepare a removal if it fits in place.
  if (tryDirectFirst && sameFootprint) {
    location = findLegOrigin(probe, va, 0, trial, rtOrient);
  }
  if (location == notFound) {
    std::vector<CellChangeRecord> removed{
        {OpType::Delete, old->getDbInst(), UvDist(old->getLeft().v),
         UvDist(old->getBottom().v), old->getMaster()->getDbMaster(),
         old->getMaster()->getDbMaster(), old->getOrient()}};
    // This virtual removal layout is only a seed: the final target can make
    // an otherwise unfillable old footprint tileable.
    drc_engine_->checkDRC(nullptr, oldX, oldY, old->getOrient(), trial, removed);
    probe.setLeft(old->getLeft());
    probe.setBottom(old->getBottom());
    location = findLegOrigin(probe, va, diameter, trial, rtOrient);
  }
  if (location == notFound) {
    return notFound;
  }
  const GridX x{location.first};
  const GridY y{location.second};
  const int xUv = x.v * grid_->getSiteWidth().v;
  const int yUv = grid_->gridYToDbu(y).v;
  trial.push_back({OpType::Replace, old->getDbInst(),
                   UvDist(xUv), UvDist(yUv),
                   old->getMaster()->getDbMaster(), masterId, rtOrient});
  ccRecords.insert(ccRecords.end(), trial.begin(), trial.end());
  return {xUv, yUv};
}

/**
 * Apply a prevalidated Add/Replace/Delete batch. Standard-cell changes update
 * only Network/Grid; the caller owns their UDM transaction. Filler changes
 * also update UDM. Coordinates are core-relative. Callers own DRC and must
 * serialize commits with placement readers. Invalid input is rejected before
 * mutation; this is not an undo journal for allocation/UDM exceptions.
 */
bool DePlace::commit(const std::vector<CellChangeRecord>& ccRecords)
{
  if (ccRecords.empty()) {
    return true;
  }
  if (design_ == nullptr || desMgr_ == nullptr || network_ == nullptr
      || grid_ == nullptr || grid_->getPixelYSize() == 0) {
    return false;
  }

  // The caller creates std instances in UDM before committing their placement.
  // Resolve every requested name in one read-only pass; keep the public wire
  // format unchanged and never allocate a synthetic database identity.
  std::map<std::string, LeafCellID> standardAdds;
  for (const auto& record : ccRecords) {
    const Master* master = network_->getMaster(record.new_lib_cell_);
    const auto* name = std::get_if<std::string>(&record.cell_data_);
    if (record.op_ == OpType::Add && master != nullptr && !master->isFiller()
        && name != nullptr && !name->empty()) {
      standardAdds.emplace(*name, LeafCellID{});
    }
  }
  if (!standardAdds.empty()) {
    if (design_->getHierMgr() == nullptr) {
      return false;
    }
    struct StandardAddVisitor : eUNL::UnlBaseVisitor<eUNL::PhysCell, LeafCellID>
    {
      const eUNL::HierManager* hierarchy;
      std::map<std::string, LeafCellID>& names;
      bool ambiguous = false;

      StandardAddVisitor(const eUNL::HierManager* h, std::map<std::string, LeafCellID>& n)
          : hierarchy(h), names(n) {}
      bool filter(const eUNL::PhysCell& cell, const LeafCellID&) override { return cell.isValid(); }
      eUNL::UnlIterStatus visit(const eUNL::PhysCell&, const LeafCellID& id) override
      {
        const auto found = names.find(hierarchy->getLeafCell(id).getName());
        if (found != names.end()) {
          ambiguous |= found->second.isValid();
          found->second = id;
        }
        return eUNL::UnlIterStatus::CONTINUE;
      }
    } visitor(design_->getHierMgr(), standardAdds);
    tbb::task_arena arena(1);
    eLIB::PhysMacroUsageSet usages;
    desMgr_->iterateAllPhysCells(arena, visitor, usages);
    if (visitor.ambiguous) {
      return false;
    }
  }

  eUNL::NlEditor editor(design_);
  struct Pending
  {
    const CellChangeRecord* record = nullptr;
    bool standard = false;
    Node* old = nullptr;
    Node proposed;
    std::unique_ptr<eUNL::UnlChange_addCell> add;
    std::unique_ptr<eUNL::UnlChange_sizeCell> swap;
    std::unique_ptr<eUNL::UnlChange_placeCell> place;
    std::unique_ptr<eUNL::UnlChange_removeCell> remove;
  };
  std::vector<Pending> pending;
  pending.reserve(ccRecords.size());
  std::set<Node*> removedOccupants;
  std::set<std::string> addedNames;
  for (const auto& record : ccRecords) {
    Pending entry;
    entry.record = &record;
    if (record.op_ != OpType::Add) {
      const auto* id = std::get_if<LeafCellID>(&record.cell_data_);
      entry.old = id != nullptr ? network_->getNode(*id) : nullptr;
      if (entry.old == nullptr || entry.old->getMaster() == nullptr
          || entry.old->isFixed() || entry.old->isTerminal()
          || entry.old->getGroup() != nullptr
          || !removedOccupants.insert(entry.old).second) {
        return false;
      }
      entry.standard = !entry.old->isFiller();
      if (record.orig_lib_cell_.isValid()
          && record.orig_lib_cell_ != entry.old->getMaster()->getDbMaster()) {
        return false;
      }
      // A previous std commit may intentionally differ from UDM. Network is
      // authoritative for the next probe/commit; fillers remain DB-backed.
      if (!entry.standard) {
        const auto physical = desMgr_->getPhysCell(*id);
        if (!physical.isValid()
            || physical.getStatus() == PhysObjStatus::LOC_FIXED
            || physical.getPhysMaster().getLibCellId()
                   != entry.old->getMaster()->getDbMaster()
            || physical.getOrigin().getX() - network_->getCore().getXL()
                   != UvDist(entry.old->getLeft().v)
            || physical.getOrigin().getY() - network_->getCore().getYL()
                   != UvDist(entry.old->getBottom().v)
            || physical.getOrient() != entry.old->getOrient()) {
          return false;
        }
      }
    }
    if (record.op_ == OpType::Delete) {
      if (!entry.standard) {
        entry.remove = std::make_unique<eUNL::UnlChange_removeCell>(
            &editor, *design_, eUNL::UnlChangePhaseE::PRE_CHANGE,
            entry.old->getDbInst());
        if (!entry.remove->feasible()) {
          return false;
        }
      }
      pending.push_back(std::move(entry));
      continue;
    }
    if (record.op_ != OpType::Add && record.op_ != OpType::Replace) {
      return false;
    }
    Master* master = network_->getMaster(record.new_lib_cell_);
    if (master == nullptr || master->getPhysLibCell() == nullptr) {
      return false;
    }
    if (record.op_ == OpType::Replace && entry.old->isFiller() != master->isFiller()) {
      return false;
    }
    entry.standard = !master->isFiller();
    const PhysLibCell& physicalMaster = *master->getPhysLibCell();
    const auto orientation = record.orientation_;
    if (orientation != PhysOrientationE::R0
        && orientation != PhysOrientationE::R180
        && orientation != PhysOrientationE::MX
        && orientation != PhysOrientationE::MY) {
      return false;
    }
    entry.proposed.setMaster(master);
    entry.proposed.setType(master->isFiller() ? Node::FILLER : Node::CELL);
    entry.proposed.setWidth(DbuX{physicalMaster.getWidth().getStorage()});
    entry.proposed.setHeight(DbuY{physicalMaster.getHeight().getStorage()});
    entry.proposed.setLeft(DbuX{record.x_.getStorage()});
    entry.proposed.setBottom(DbuY{record.y_.getStorage()});
    entry.proposed.setOrient(orientation);
    if (record.op_ == OpType::Replace) {
      entry.proposed.setDbInst(entry.old->getDbInst());
    }
    if (physicalMaster.getType().isBlock() || physicalMaster.getType().isPad()
        || physicalMaster.getType().isCover()
        || entry.proposed.getWidth().v <= 0
        || entry.proposed.getHeight().v <= 0) {
      return false;
    }
    if (record.op_ == OpType::Add) {
      const auto* name = std::get_if<std::string>(&record.cell_data_);
      if (name == nullptr || name->empty()
          || !addedNames.insert(*name).second) {
        return false;
      }
      if (entry.standard) {
        const auto found = standardAdds.find(*name);
        if (found == standardAdds.end() || !found->second.isValid()
            || network_->getNode(found->second) != nullptr) {
          return false;
        }
        const auto physical = desMgr_->getPhysCell(found->second);
        if (!physical.isValid() || physical.getStatus() == PhysObjStatus::LOC_FIXED
            || physical.getPhysMaster().getLibCellId() != record.new_lib_cell_) {
          return false;
        }
        entry.proposed.setDbInst(found->second);
      } else {
        entry.add = std::make_unique<eUNL::UnlChange_addCell>(
            &editor,
            *design_,
            eUNL::UnlChangePhaseE::PRE_CHANGE,
            *name,
            design_->getLibAcc().getLibCell(record.new_lib_cell_).getMaster(),
            Point2D(record.x_ + network_->getCore().getXL(),
                    record.y_ + network_->getCore().getYL()),
            orientation);
        if (!entry.add->feasible()) {
          return false;
        }
      }
    } else if (!entry.standard) {
      const auto oldModule
          = design_->getLibAcc()
                .getLibCell(entry.old->getMaster()->getDbMaster())
                .getMaster();
      entry.swap = std::make_unique<eUNL::UnlChange_sizeCell>(
          &editor,
          *design_,
          eUNL::UnlChangePhaseE::PRE_CHANGE,
          entry.old->getDbInst(),
          oldModule,
          design_->getLibAcc().getLibCell(record.new_lib_cell_).getMaster());
      entry.place = std::make_unique<eUNL::UnlChange_placeCell>(
          &editor,
          *design_,
          eUNL::UnlChangePhaseE::PRE_CHANGE,
          entry.old->getDbInst(),
          Point2D(record.x_ + network_->getCore().getXL(),
                  record.y_ + network_->getCore().getYL()),
          orientation);
      if (!entry.swap->feasible() || !entry.place->feasible()) {
        return false;
      }
    }
    pending.push_back(std::move(entry));
  }

  // Check final footprints together, ignoring only the old instances in this
  // batch. Do not let record order authorize overlap or overwrite reservations.
  std::set<Pixel*> claimed;
  for (auto& entry : pending) {
    if (entry.record->op_ == OpType::Delete) {
      continue;
    }
    const int64_t right
        = int64_t{entry.proposed.getLeft().v} + entry.proposed.getWidth().v;
    const int64_t top
        = int64_t{entry.proposed.getBottom().v} + entry.proposed.getHeight().v;
    if (entry.proposed.getLeft().v < 0 || entry.proposed.getBottom().v < 0
        || right > int64_t{network_->getCore().getXH().getStorage()}
                       - network_->getCore().getXL().getStorage()
        || top > int64_t{network_->getCore().getYH().getStorage()}
                     - network_->getCore().getYL().getStorage()) {
      return false;
    }
    const auto box = grid_->gridCovering(&entry.proposed);
    if (box.xlo.v < 0 || box.ylo.v < 0 || box.xhi > grid_->getRowSiteCount()
        || box.yhi > grid_->getRowCount()
        || box.xlo.v * grid_->getSiteWidth().v != entry.proposed.getLeft().v
        || box.xhi.v * grid_->getSiteWidth().v
               != entry.proposed.getLeft().v + entry.proposed.getWidth().v
        || grid_->gridYToDbu(box.ylo) != entry.proposed.getBottom()
        || grid_->gridYToDbu(box.yhi)
               != entry.proposed.getBottom() + entry.proposed.getHeight()) {
      return false;
    }
    const auto& master = *entry.proposed.getMaster()->getPhysLibCell();
    const bool hasPattern = master.hasSitePattern();
    const auto siteOrient = grid_->getPlacementSiteOrientation(
        box.xlo, box.ylo, master,
        hasPattern ? std::make_optional(entry.proposed.getOrient()) : std::nullopt);
    const auto flipsRows = [](PhysOrientation orientation) {
      return orientation == PhysOrientationE::MX || orientation == PhysOrientationE::R180;
    };
    if (!siteOrient || (!hasPattern
                        && flipsRows(*siteOrient) != flipsRows(entry.proposed.getOrient()))) {
      return false;
    }
    const auto padded = grid_->gridCoveringPadded(&entry.proposed);
    for (GridY y = padded.ylo; y < padded.yhi; ++y) {
      for (GridX x = padded.xlo; x < padded.xhi; ++x) {
        Pixel* pixel = grid_->gridPixel(x, y);
        if (pixel == nullptr || !pixel->is_valid
            || (pixel->cell != nullptr
                && removedOccupants.count(pixel->cell) == 0)
            || (pixel->padding_reserved_by != nullptr
                && removedOccupants.count(pixel->padding_reserved_by) == 0)
            || !claimed.insert(pixel).second) {
          return false;
        }
      }
    }
  }

  for (Node* old : removedOccupants) {
    grid_->erasePixel(old);
    old->setPlaced(false);
  }
  for (auto& entry : pending) {
    if (entry.standard) {
      // The batch is already validated. Never refresh a std Node from UDM:
      // its caller-owned physical instance may still have the old placement.
      if (entry.record->op_ == OpType::Delete) {
        network_->deleteNode(entry.old);
        continue;
      }
      Node* node = entry.old;
      if (entry.record->op_ == OpType::Add) {
        auto added = std::make_unique<Node>(entry.proposed);
        added->setId(-1);
        node = added.get();
        network_->addNode(std::move(added));
      } else {
        node->setMaster(entry.proposed.getMaster());
        node->setWidth(entry.proposed.getWidth());
        node->setHeight(entry.proposed.getHeight());
        node->setLeft(entry.proposed.getLeft());
        node->setBottom(entry.proposed.getBottom());
        node->setOrient(entry.proposed.getOrient());
      }
      node->setType(Node::CELL);
      node->setOrigLeft(node->getLeft());
      node->setOrigBottom(node->getBottom());
      node->setBottomPower(node->getMaster()->getBottomPowerType());
      node->setTopPower(node->getMaster()->getTopPowerType());
      node->setPlaced(true);
      node->setHold(false);
      grid_->paintPixel(node);
    } else if (entry.remove != nullptr) {
      entry.remove->commit();
      network_->deleteNode(entry.old);
    } else if (entry.add != nullptr) {
      entry.add->commit();
      network_->addNode(entry.add->getCellId(), desMgr_);
      grid_->paintPixel(network_->getNode(entry.add->getCellId()));
    } else {
      entry.swap->commit();
      entry.place->commit();
      network_->updateNode(
          entry.old, desMgr_, *entry.proposed.getMaster()->getPhysLibCell());
      grid_->paintPixel(entry.old);
    }
  }
  return true;
}

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
  cell.setId(-1);
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

std::pair<int, int> DePlace::findLegOrigin(
    Node& probe, eUNL::VoltageArea* va, int diameter,
    std::vector<CellChangeRecord>& trial, PhysOrientation& rtOrient)
{
  constexpr std::pair<int, int> notFound{-1, -1};
  const Node* replacedCell = network_->getNode(probe.getId());
  if (replacedCell != nullptr
      && (replacedCell->isFiller() || replacedCell->getDbInst() != probe.getDbInst())) {
    replacedCell = nullptr;
  }
  const DbuX anchorX = probe.getLeft();
  const DbuY anchorY = probe.getBottom();
  const UvDist width(probe.getWidth().v);
  const UvDist height(probe.getHeight().v);
  // The probe is core-relative, but the search rectangle and voltage areas
  // are absolute. legalCellInRect converts its rectangle back to the grid.
  if (replacedCell != nullptr) {
    const GridX x = grid_->gridX(replacedCell);
    const GridY y = grid_->gridSnapDownY(replacedCell);
    const UvDist xl = core_.getXL() + UvDist(x.v * grid_->getSiteWidth().v);
    const UvDist yl = core_.getYL() + UvDist(grid_->gridYToDbu(y).v);
    bool inArea = va == nullptr;
    if (va != nullptr) {
      for (const Rect& rect : va->getRects()) {
        if (xl >= rect.getXL() && yl >= rect.getYL()
            && xl + width <= rect.getXH() && yl + height <= rect.getYH()) {
          inArea = true;
          break;
        }
      }
    }
    if (inArea && canBePlaced(&probe, x, y, trial, rtOrient)) {
      return {x.v, y.v};
    }
    if (diameter == 0) {
      return notFound;
    }
  }

  const UvDist xl = core_.getXL() + UvDist(anchorX.v);
  const UvDist yl = core_.getYL() + UvDist(anchorY.v);
  const UvDist extension(diameter);
  const Rect search(xl - extension, yl - extension,
                    xl + width + extension, yl + height + extension);
  if (replacedCell == nullptr) {
    std::ostringstream record;
    record << "[findLeg] target master id=" << probe.getMaster()->getDbMaster().getValue()
           << " w=" << width.getStorage() << " h=" << height.getStorage()
           << " anchor origin=(" << xl.getStorage() << "," << yl.getStorage() << ")\n"
           << "[findLeg] search rect xl=" << search.getXL().getStorage()
           << " xh=" << search.getXH().getStorage()
           << " yl=" << search.getYL().getStorage()
           << " yh=" << search.getYH().getStorage()
           << " diameter=" << diameter << "\n";
    const auto message = record.str();
    std::fwrite(message.data(), 1, message.size(), stdout);
  }
  const auto searchRect = [&](const Rect& rect) {
    return legalCellInRect(rect.overlap(core_, false), &probe, trial, rtOrient);
  };
  if (va == nullptr) {
    return searchRect(search);
  }
  for (const Rect& rect : va->getRects()) {
    const auto position = searchRect(search.overlap(rect, false));
    if (position != notFound) {
      return position;
    }
  }
  return notFound;
}

/**
 * @brief Find legal placement for a new cell at a specific pin's location.
 *
 * Creates a temporary Node for @p masterId at the pin's origin, then searches
 * a @p diameter-extended bounding box for a DRC-legal site.  If a @p va
 * (voltage area) is given, the search is restricted to the intersection
 * of the extended box with each VA rect.
 *
 * The temporary Node is discarded after the search. The returned transaction
 * includes its named Add plus all filler changes; commit it as one batch.
 *
 * @param startLoc  Pin whose physical location anchors the search center.
 * @param va        Optional voltage-area to restrict placement to.
 * @param diameter  Search region extension (in uv units).
 * @param masterId  Target library cell master to place.
 * @param ccRecords [out] Appends the selected target Add, deleted fillers and
 *                  filler repair records; unchanged when no legal site exists.
 * @return Core-relative (left, bottom) DBU coordinates of the found site,
 * or (-1, -1) on failure.
 */
std::pair<int, int> DePlace::findLeg(eUNL::PinID startLoc,
                                     eUNL::VoltageArea* va,
                                     int diameter,
                                     LibCellID masterId,
                                     std::vector<CellChangeRecord>& ccRecords)
{
  if (diameter < 0 || design_ == nullptr || desMgr_ == nullptr
      || network_ == nullptr || grid_ == nullptr || drc_engine_ == nullptr) {
    return {-1, -1};
  }
  Master* const master = network_->getMaster(masterId);
  const PhysLibCell* physical = master != nullptr ? master->getPhysLibCell() : nullptr;
  if (physical == nullptr || physical->getTechSite() == nullptr) {
    return {-1, -1};
  }
  const PhysPin& pin = desMgr_->getPhysPin(eUNL::PhysPinID(startLoc.asFlatPin()));
  const auto& terms = pin.getTermIter();
  if (terms.begin() == terms.end()) {
    return {-1, -1};
  }
  const Point2D origin = (*terms.begin()).getOrigin();

  Node cell;
  initTempNode(cell, master, *physical,
               Point2D{origin.getX() - core_.getXL(), origin.getY() - core_.getYL()});
  std::vector<CellChangeRecord> trial = ccRecords;
  PhysOrientation rtOrient{PhysOrientationE::R0};
  const auto [col, row] = findLegOrigin(cell, va, diameter, trial, rtOrient);
  if (col == -1) {
    return {-1, -1};
  }
  // A new std cell has no DB id yet. Reserve a deterministic request-local name
  // against both live instances and pending Adds, including generated fillers.
  std::set<std::string> names;
  for (const auto& [id, node] : network_->getNodes()) {
    (void) id;
    if (node != nullptr && node->getDbInst().isValid()) {
      names.insert(design_->getHierMgr()->getLeafCell(node->getDbInst()).getName());
    }
  }
  for (const CellChangeRecord& change : trial) {
    if (const auto* name = std::get_if<std::string>(&change.cell_data_)) {
      names.insert(*name);
    }
  }
  const std::string base = "DPL_STD_" + std::to_string(row) + "_"
                           + std::to_string(col) + "_M"
                           + std::to_string(masterId.getIndexValue());
  std::string name = base;
  for (size_t suffix = 1; names.count(name) != 0; ++suffix) {
    name = base + "_" + std::to_string(suffix);
  }
  const int xUv = col * grid_->getSiteWidth().v;
  const int yUv = grid_->gridYToDbu(GridY{row}).v;
  trial.push_back({OpType::Add, name, UvDist(xUv), UvDist(yUv),
                   LibCellID(), masterId, rtOrient});
  ccRecords = std::move(trial);
  return {xUv, yUv};
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
// occupancy independent of the filler/non-filler classification.
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
