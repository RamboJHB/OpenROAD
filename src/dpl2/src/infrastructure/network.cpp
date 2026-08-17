// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2021-2025, The OpenROAD Authors

#include <network.h>

#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/architecture.h>
#include <infrastructure/fillerSetting.h>

namespace dpl2 {

namespace {

std::vector<Rect> difference(const Rect& parent_segment,
                             const std::vector<Rect>& segs)
{
  if (segs.empty()) {
    return {parent_segment};
  }
  bool is_horizontal = parent_segment.getYL() == parent_segment.getYH();
  std::vector<Rect> sorted_segs = segs;
  // Sort segments by start coordinate
  std::ranges::sort(
      sorted_segs,

      [is_horizontal](const Rect& a, const Rect& b) {
        return (is_horizontal ? a.getXL() < b.getXL() : a.getYL() < b.getYL());
      });
  // Merge overlapping segments
  auto prev_seg = sorted_segs.begin();
  auto curr_seg = prev_seg;
  for (++curr_seg; curr_seg != sorted_segs.end();) {
    if (curr_seg->intersect(*prev_seg, false)) {
      *prev_seg = prev_seg->expand(*curr_seg);
      curr_seg = sorted_segs.erase(curr_seg);
    } else {
      prev_seg = curr_seg++;
    }
  }
  // Get the difference
  const int start = is_horizontal ? parent_segment.getXL().getStorage() :
    parent_segment.getYL().getStorage();
  const int end = is_horizontal ? parent_segment.getXH().getStorage() :
    parent_segment.getYH().getStorage();
  int current_pos = start;
  std::vector<Rect> result;
  for (const Rect& seg : sorted_segs) {
    int seg_start = is_horizontal ? seg.getXL().getStorage() :
      seg.getYL().getStorage();
    int seg_end = is_horizontal ? seg.getXH().getStorage() :
      seg.getYH().getStorage();
    if (seg_start > current_pos) {
      if (is_horizontal) {
        result.emplace_back(UvDist(current_pos),
                            parent_segment.getYL(),
                            UvDist(seg_start),
                            parent_segment.getYH());
      } else {
        result.emplace_back(parent_segment.getXL(),
                            UvDist(current_pos),
                            parent_segment.getXH(),
                            UvDist(seg_start));
      }
    }
    current_pos = seg_end;
  }
  // Add the remaining end segment if it exists
  if (current_pos < end) {
    if (is_horizontal) {
      result.emplace_back(
          UvDist(current_pos), parent_segment.getYL(), UvDist(end),
          parent_segment.getYH());
    } else {
      result.emplace_back(
          parent_segment.getXL(), UvDist(current_pos), parent_segment.getXH(),
          UvDist(end));
    }
  }

  return result;
}

Rect getBoundarySegment(const Rect& bbox,
                        const eLIB::MacroEdgeDir dir)
{
  Rect segment(bbox);
  switch (dir) {
    case eLIB::MacroEdgeDir::RIGHT:
      segment.setXL(bbox.getXH());
      break;
    case eLIB::MacroEdgeDir::LEFT:
      segment.setXH(bbox.getXL());
      break;
    case eLIB::MacroEdgeDir::TOP:
      segment.setYL(bbox.getYH());
      break;
    case eLIB::MacroEdgeDir::BOTTOM:
      segment.setYH(bbox.getYL());
      break;
    case eLIB::MacroEdgeDir::INVALID:
      break;
  }
  return segment;
}

std::pair<int, int> getMasterPwrs(const eLIB::PhysLibCell& master)
{
  int maxPwr = std::numeric_limits<int>::min();
  int minPwr = std::numeric_limits<int>::max();
  int maxGnd = std::numeric_limits<int>::min();
  int minGnd = std::numeric_limits<int>::max();

  bool isVdd = false;
  bool isGnd = false;

  for (const eLIB::PhysLibPort* port : master.getPorts()) {
    if (port->getUse() == eLIB::SignalTypeE::POWER) {
      isVdd = true;
      for (const eLIB::PhysLibTerm& pin : port->getLibTermIter()) {
        for (const auto& [layerId, shapes] : pin.getShapes()) {
          for (const eLIB::TechShape& shape : shapes) {
            Rect rect;
            if (shape.isRect()) {
              rect = shape.getRect();
            } else if (shape.isPolygon()) {
              rect = shape.getPolygon().getBbox();
            }
            const int y = rect.center().getY().getStorage();
            minPwr = std::min(minPwr, y);
            maxPwr = std::max(maxPwr, y);
          }
        }
      }
    } else if (port->getUse() == eLIB::SignalTypeE::GROUND) {
      isGnd = true;
      for (const eLIB::PhysLibTerm& pin : port->getLibTermIter()) {
        for (const auto& [layerId, shapes] : pin.getShapes()) {
          for (const eLIB::TechShape& shape : shapes) {
            Rect rect;
            if (shape.isRect()) {
              rect = shape.getRect();
            } else if (shape.isPolygon()) {
              rect = shape.getPolygon().getBbox();
            }
            const int y = rect.center().getY().getStorage();
            minGnd = std::min(minGnd, y);
            maxGnd = std::max(maxGnd, y);
          }
        }
      }
    }
  }
  int topPwr = Architecture::Row::Power_UNK;
  int botPwr = Architecture::Row::Power_UNK;
  if (isVdd && isGnd) {
    topPwr = (maxPwr > maxGnd) ? Architecture::Row::Power_VDD
                               : Architecture::Row::Power_VSS;
    botPwr = (minPwr < minGnd) ? Architecture::Row::Power_VDD
                               : Architecture::Row::Power_VSS;
  }
  return {topPwr, botPwr};
}

} // namespace
//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////
/// @brief Create a dpl2 Pin from an PhysPin.
///
/// Extracts the pin's bounding box, offset (relative to cell center), width,
/// height, layer, and mask number from the PhysLibPort's geometry shapes.
/// The mask is taken from each dbBox's getMask() (the MASK attribute from
/// LEF/DEF).  If the shape has no mask (mask == 0), PinMask stays at its
/// default of 0 (no mask coloring).
///
/// The pin is also connected to its owning Node via the dbITerm's getInst().
///
/// @param  term  The odb-level pin (dbITerm) to wrap.
/// @return       Pointer to the newly created dpl2::Pin.
void Network::addPin(const eLIB::PhysLibPort* libport, Master* master)
{
  for (const eLIB::PhysLibTerm* libterm : libport->getLibTerms()) {
    for (auto [layerId, shapes] : libterm->getShapes()) {  //ref
      for (const TechShape& shape : shapes) {
        auto upin = std::make_unique<Pin>();
        Pin* ptr = upin.get();

        if (shape.isRect()) {
          upin->setBbox(shape.getRect());
        } else if (shape.isPolygon()) {
          upin->setBbox(shape.getPolygon().getBbox());
        }

        int foundMask = 0;

        TechLayerID layerId = shape.getLayer();

        eLIB::ShapeMaskID maskId = shape.getMaskId();
        foundMask  = static_cast<int>(maskId);
        upin->setPinMask(foundMask);
        upin->setPinLayer(layerId);
        pins_.emplace_back(std::move(upin));
        connect(ptr, master);
      }
    }
  }
}
void Network::connect(Pin* pin, Master* master)
{
  master->addPin(pin);
}
//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////
Master* Network::getMaster(LibCellID db_master)
{
  auto it = master_to_idx_.find(db_master);
  if (it == master_to_idx_.end()) {
    return nullptr;
  }
  return getMaster(it->second);
}

Master* Network::addMaster(const PhysLibCell& db_master,
                           const fillerSetting& filler_setting,
                           const Grid* grid,
                           const EdgeTypeTable* edge_types)
{
  LibCellID masterId = db_master.getLibCellId();
  const auto it = master_to_idx_.find(masterId);
  if (it != master_to_idx_.end()) {
    Master* master = getMaster(it->second);
    master->setFiller(filler_setting.isFillerCell(masterId));
    return master;
  }
  std::unique_ptr<Master> umaster = std::make_unique<Master>();
  Master* master = umaster.get();
  const int id = next_master_id_++;
  master_to_idx_[masterId] = id;
  master->setId(id);
  masters_.emplace(id, std::move(umaster));
  master->setDbMaster(masterId);
  master->setPhysLibCell(&db_master);
  master->setFiller(filler_setting.isFillerCell(masterId));

  Rect bbox(UvDist(0), UvDist(0), db_master.getWidth(), db_master.getHeight());

  master->setBBox(bbox);
  master->setMultiRow(grid->isMultiHeight(db_master));
  auto master_pwrs = getMasterPwrs(db_master);
  for (auto libport : db_master.getPorts()) {
    if (libport->isPgPort()) {
      continue;
    }
    addPin(libport, master);
  }
  master->setTopPowerType(master_pwrs.first);
  master->setBottomPowerType(master_pwrs.second);
  master->clearEdges();
  if (!edge_types->hasTable()) {
    return master;
  }
  if (master->isFiller()) {  // Skip fillcells
    return master;
  }

  std::map<eLIB::MacroEdgeDir, std::vector<Rect>> typed_segs;
  int num_rows = grid->gridHeight(db_master).v;

  for (const auto& edge : db_master.getEdgeTypeVec()) {
    eLIB::MacroEdgeDir dir = edge.edgeDir;
    Rect edge_rect = getBoundarySegment(bbox, dir);
    if (dir == eLIB::MacroEdgeDir::TOP
        || dir == eLIB::MacroEdgeDir::BOTTOM) {
      if (edge.hasRange()) {
        edge_rect.setXL(edge_rect.getXL() + edge.range.first);
        edge_rect.setXH(edge_rect.getXL() + edge.range.second);
      }
    } else {
      auto dy = edge_rect.dy();
      auto row_height = dy.getStorage() / num_rows;
      auto half_row_height = row_height / 2;
      if (edge.hasCellRow()) {
        edge_rect.setYL(UvDist(edge_rect.getYL().getStorage()
                               + (edge.cellRow - 1) * (row_height)));
        edge_rect.setYH(UvDist(
            std::min(edge_rect.getYH().getStorage(), edge_rect.getYL().getStorage()
            + (row_height))));
      } else if (edge.hasHalfRow()) {
        edge_rect.setYL(UvDist(edge_rect.getYL().getStorage()
                               + (edge.halfRow - 1) * (half_row_height)));
        edge_rect.setYH(UvDist(
            std::min(edge_rect.getYH().getStorage(), edge_rect.getYL().getStorage()
            + (half_row_height))));
      }
    }
    typed_segs[dir].push_back(edge_rect);
    const auto edge_type_idx = edge_types->getEdgeTypeIdx(edge.edgeTypeName.c_str());
    if (edge_type_idx != -1) {
      // consider only edge types defined in the spacing table
      master->addEdge(dpl2::MasterEdge(edge_type_idx, edge_rect));
    }
  }
  const auto default_edge_type_idx = edge_types->getEdgeTypeIdx("DEFAULT");
  if (default_edge_type_idx == -1) {
    return master;
  }
  // Add the remaining DEFAULT un-typed segments
  for (size_t dir_idx = 1; dir_idx <= 4; dir_idx++) {
    const auto dir = (eLIB::MacroEdgeDir) dir_idx;
    const auto parent_seg = getBoundarySegment(bbox, dir);
    const auto default_segs = difference(parent_seg, typed_segs[dir]);
    for (const auto& seg : default_segs) {
      master->addEdge(dpl2::MasterEdge(default_edge_type_idx, seg));
    }
  }
  return master;
}
//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////
Node* Network::getNode(LeafCellID cellId)
{
  auto it = inst_to_node_idx_.find(cellId);
  if (it == inst_to_node_idx_.end()) {
    return nullptr;
  }
  return getNode(it->second);
}

void Network::addNode(LeafCellID cellId, const PhysDesMgr* desMgr)
{
  Node ndi;
  const int id = next_node_id_++;
  const PhysCell& inst = desMgr->getPhysCell(cellId);
  ndi.setId(id);
  ndi.setDbInst(cellId);
  auto master = getMaster(inst.getPhysMaster().getLibCellId());
  ndi.setMaster(master);
  ndi.setType(master->isFiller() ? Node::FILLER : Node::CELL);
  ndi.setFixed(inst.getStatus() == eUNL::PhysObjStatus::LOC_FIXED);
  ndi.setPlaced(inst.getStatus() == eUNL::PhysObjStatus::PLACED);

  ndi.setOrient(inst.getOrient());
  ndi.setHeight(DbuY{inst.getPhysMaster().getHeight().getStorage()});
  ndi.setWidth(DbuX{inst.getPhysMaster().getWidth().getStorage()});
  ndi.setOrigLeft(DbuX{(inst.getOrigin().getX().getStorage() -
      core_.getXL().getStorage())});
  ndi.setOrigBottom(DbuY{(inst.getOrigin().getY().getStorage() -
      core_.getYL().getStorage())});

  ndi.setLeft(ndi.getOrigLeft());
  ndi.setBottom(ndi.getOrigBottom());
  ndi.setBottomPower(master->getBottomPowerType());
  ndi.setTopPower(master->getTopPowerType());
  nodes_.emplace(id, std::make_unique<Node>(ndi));
  inst_to_node_idx_[cellId] = id;
}

void Network::addFillerNode(LeafCellID cellId, const PhysDesMgr* desMgr)
{
  Node ndi;
  const int id = next_node_id_++;
  const PhysCell& inst = desMgr->getPhysCell(cellId);
  ndi.setId(id);
  ndi.setDbInst(cellId);
  ndi.setType(Node::FILLER);
  auto master = getMaster(inst.getPhysMaster().getLibCellId());
  ndi.setMaster(master);
  ndi.setType(master->isFiller() ? Node::FILLER : Node::CELL);
  ndi.setFixed(inst.getStatus() == eUNL::PhysObjStatus::LOC_FIXED);
  ndi.setPlaced(inst.getStatus() == eUNL::PhysObjStatus::PLACED);

  ndi.setOrient(inst.getOrient());
  ndi.setHeight(DbuY{inst.getPhysMaster().getHeight().getStorage()});
  ndi.setWidth(DbuX{inst.getPhysMaster().getWidth().getStorage()});
  ndi.setOrigLeft(DbuX{(inst.getOrigin().getX().getStorage() -
      core_.getXL().getStorage())});
  ndi.setOrigBottom(DbuY{(inst.getOrigin().getY().getStorage() -
      core_.getYL().getStorage())});

  ndi.setLeft(ndi.getOrigLeft());
  ndi.setBottom(ndi.getOrigBottom());
  ndi.setBottomPower(master->getBottomPowerType());
  ndi.setTopPower(master->getTopPowerType());
  nodes_.emplace(id, std::make_unique<Node>(ndi));
  inst_to_node_idx_[cellId] = id;
}

Node* Network::addNode(LibCellID lcId, DbuX x, DbuY y, const eUNL::Design* design)
{
  Node ndi;
  const int id = next_node_id_++;
  const PhysLibCell& pcell = design->getLibAcc().getPhysLibCell(lcId);
  ndi.setId(id);
  // ndi.setDbInst(cellId);
  auto master = getMaster(lcId);
  ndi.setMaster(master);
  ndi.setType(master->isFiller() ? Node::FILLER : Node::CELL);
  ndi.setFixed(false);
  ndi.setPlaced(false);

  ndi.setOrient(PhysOrientationE::R0);
  ndi.setHeight(DbuY{pcell.getHeight().getStorage()});
  ndi.setWidth(DbuX{pcell.getWidth().getStorage()});
  ndi.setOrigLeft(x);
  ndi.setOrigBottom(y);

  ndi.setLeft(ndi.getOrigLeft());
  ndi.setBottom(ndi.getOrigBottom());
  ndi.setBottomPower(master->getBottomPowerType());
  ndi.setTopPower(master->getTopPowerType());
  std::unique_ptr<Node> nodePtr = std::make_unique<Node>(ndi);
  nodes_.emplace(id, std::move(nodePtr));
  Node* placed = nodes_.find(id)->second.get();
  // inst_to_node_idx_[cellId] = id;
  return placed;
}

void Network::deleteNode(Node* cell)
{
  if (cell == nullptr) {
    return;
  }
  // ids are stable (independent of container order), so removing a node only
  // needs to drop it from the lookup containers -- no index remapping of the
  // survivors, no getNodeId()/getId() drift.
  const LeafCellID instId = cell->getDbInst();
  if (instId.isValid()) {
    inst_to_node_idx_.erase(instId);
  }
  auto it = nodes_.find(cell->getId());
  if (it != nodes_.end() && it->second.get() == cell) {
    nodes_.erase(it);
  }
}

bool Network::updateNode(Node* ndi,
                         const PhysDesMgr* desMgr,
                         const PhysLibCell& physLibCell)
{
  LeafCellID cellId = ndi->getDbInst();
  const PhysCell& inst = desMgr->getPhysCell(cellId);
  auto master = getMaster(physLibCell.getLibCellId());
  ndi->setMaster(master);
  ndi->setType(master->isFiller() ? Node::FILLER : Node::CELL);
  ndi->setFixed(inst.getStatus() == eUNL::PhysObjStatus::LOC_FIXED);
  ndi->setPlaced(inst.getStatus() == eUNL::PhysObjStatus::PLACED);

  ndi->setOrient(inst.getOrient());
  ndi->setHeight(DbuY{physLibCell.getHeight().getStorage()});
  ndi->setWidth(DbuX{physLibCell.getWidth().getStorage()});
  ndi->setOrigLeft(DbuX{(inst.getOrigin().getX().getStorage() -
      core_.getXL().getStorage())});
  ndi->setOrigBottom(DbuY{(inst.getOrigin().getY().getStorage() -
      core_.getYL().getStorage())});

  ndi->setLeft(ndi->getOrigLeft());
  ndi->setBottom(ndi->getOrigBottom());
  ndi->setBottomPower(master->getBottomPowerType());
  ndi->setTopPower(master->getTopPowerType());
  return true;
}

}  // namespace dpl2