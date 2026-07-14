// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "UdmIdBridge.h"

#include <algorithm>

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

namespace {

// Mirror of initFromUDM step 5a's "has implant shape" test, using the
// checker's layer table instead of techLayerToCheckerId_ (which is private).
// A tech layer is an implant layer for the checker iff its NAME appears in
// checker.layers(). [VERIFY-UDM] layer identity by name; if two tech layers
// share a name this needs the relative-id route instead.
bool isImplantLayerName(const ipl::ImplantLayerChecker& checker,
                        const std::string& name)
{
  for (const ipl::ImplantLayer& layer : checker.layers()) {
    if (layer.name == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

UdmIdBridge::UdmIdBridge(const eUNL::PhysDesMgr& desMgr,
                         const ipl::ImplantLayerChecker& checker)
    : checker_(checker)
{
  const eLIB::TechLib& tech = desMgr.getTopTech();

  // --- rows: mirror of initFromUDM step 3 + step 6's row bounds ------------
  // RowId = iteration index; capture per-row span and origin alignment.
  std::vector<eUTL::UvDist> rowOriginsX;
  std::vector<std::pair<eUTL::UvDist, eUTL::UvDist>> rowYBounds;
  for (const eUNL::PhysRow& row : desMgr.getPhysRowIter()) {
    const bool isPad = row.getSite().getIsPad();
    row_is_pad_.push_back(isPad);
    rowOriginsX.push_back(row.getOrigin().getX());
    const eUTL::UvDist yLo = row.getOrigin().getY();
    rowYBounds.emplace_back(yLo, yLo + row.getSite().getHeight());
    if (!isPad) {
      const ipl::Dbu w = row.getSite().getWidth().getStorage();
      if (site_width_ == 0 || w < site_width_) {
        site_width_ = w;
      }
      const ipl::Dbu h = row.getSite().getHeight().getStorage();
      if (row_height_ == 0 || h < row_height_) {
        row_height_ = h;
      }
    }
    // Row legal span in the checker frame: [0, bbox width). [VERIFY-UDM]
    // PhysRow bbox accessor -- DePlace::getCoreArea uses row.getBbox(); the
    // width in DBU is xh-xl of that box.
    const eUTL::Rect bbox = row.getBbox();
    const ipl::Dbu width = static_cast<ipl::Dbu>(
        (bbox.getXH() - bbox.getXL()).getStorage());
    row_spans_.emplace_back(0, width);
  }
  for (size_t i = 1; i < rowOriginsX.size(); ++i) {
    if (!(rowOriginsX[i] == rowOriginsX[0])) {
      row_origins_aligned_ = false;
      break;
    }
  }

  // --- masters: mirror of initFromUDM steps 5a/5b ---------------------------
  // Same container types and iteration so the sequential MasterId assignment
  // reproduces the checker's. KEEP IN LOCKSTEP with initFromUDM.
  std::map<const eLIB::PhysLibCell*, bool> mastersWithImplant;
  const eUNL::HierManager& hierMgr = desMgr.getHierMgr();
  for (eUNL::LeafCellID lcId : hierMgr.getAllLeafCellIter()) {
    eUNL::PhysCell physCell = desMgr.getPhysCell(lcId);
    if (!physCell.isValid()) {
      continue;
    }
    const eLIB::PhysLibCell& master = physCell.getPhysMaster();
    if (!master.getType().isCore()) {
      continue;
    }
    if (mastersWithImplant.find(&master) != mastersWithImplant.end()) {
      continue;
    }
    bool hasImplant = false;
    const auto& obsVec = master.getObstruction();
    for (const auto& obs : obsVec) {
      const auto& shapes = obs.getShapes(eUTL::PhysOrientationE::R0);
      for (const auto& [layerRelId, shapeVec] : shapes) {
        // initFromUDM asks findLayerId(layerRelId) >= 0; we resolve through
        // the tech layer's name against checker.layers(). [VERIFY-UDM]
        const eLIB::TechLayer& techLayer = tech.getTechLayer(layerRelId);
        if (isImplantLayerName(checker_, techLayer.getName())) {
          hasImplant = true;
          break;
        }
      }
      if (hasImplant) {
        break;
      }
    }
    mastersWithImplant[&master] = hasImplant;
  }

  ipl::MasterId nextMasterId = 0;
  for (const auto& [masterPtr, hasImplant] : mastersWithImplant) {
    if (!hasImplant) {
      continue;
    }
    // initFromUDM additionally skips masters whose extracted shape list ends
    // up empty (non-RECT only). Mirror that filter. [VERIFY-UDM]
    bool hasRect = false;
    const auto& obsVec = masterPtr->getObstruction();
    for (const auto& obs : obsVec) {
      const auto& shapes = obs.getShapes(eUTL::PhysOrientationE::R0);
      for (const auto& [layerRelId, shapeVec] : shapes) {
        const eLIB::TechLayer& techLayer = tech.getTechLayer(layerRelId);
        if (!isImplantLayerName(checker_, techLayer.getName())) {
          continue;
        }
        for (const auto& techShape : shapeVec) {
          if (techShape.getType() == eLIB::TechShape::RECT) {
            hasRect = true;
            break;
          }
        }
        if (hasRect) {
          break;
        }
      }
      if (hasRect) {
        break;
      }
    }
    if (!hasRect) {
      continue;
    }
    master_by_cell_[masterPtr] = nextMasterId;
    master_by_libcell_[static_cast<int>(
        masterPtr->getLibCellId().getIndexValue())] = nextMasterId;
    cell_by_master_[nextMasterId] = masterPtr;
    ++nextMasterId;
  }

  // --- placed instances: mirror of initFromUDM step 6 -----------------------
  ipl::InstanceId nextInstId = 0;
  for (eUNL::LeafCellID lcId : hierMgr.getAllLeafCellIter()) {
    eUNL::PhysCell physCell = desMgr.getPhysCell(lcId);
    if (!physCell.isValid()) {
      continue;
    }
    const eLIB::PhysLibCell& master = physCell.getPhysMaster();
    const auto mIt = master_by_cell_.find(&master);
    const bool checkerModeled = mIt != master_by_cell_.end();
    if (!checkerModeled && !master.getType().isCore()) {
      // Non-core (pad/macro) cells are outside the row coverage domain; row
      // legal spans must already exclude their cutouts. [VERIFY-UDM]
      continue;
    }
    const eUNL::PhysObjStatus status = physCell.getStatus();
    if (status != eUNL::PhysObjStatus::PLACED
        && status != eUNL::PhysObjStatus::LOC_FIXED) {
      continue;
    }
    const eUTL::Point2D origin = physCell.getOrigin();
    const eUTL::PhysOrientation orient = physCell.getOrient();
    if (orient != eUTL::PhysOrientationE::R0
        && orient != eUTL::PhysOrientationE::MX
        && orient != eUTL::PhysOrientationE::MY
        && orient != eUTL::PhysOrientationE::R180) {
      continue;
    }
    const eUTL::UvDist y = origin.getY();
    ipl::RowId rowId = -1;
    for (size_t ri = 0; ri < rowYBounds.size(); ++ri) {
      if (y >= rowYBounds[ri].first && y < rowYBounds[ri].second) {
        rowId = static_cast<ipl::RowId>(ri);
        break;
      }
    }
    if (rowId < 0) {
      continue;
    }
    if (site_width_ <= 0) {
      continue;
    }
    const ipl::Dbu xOffset =
        (origin.getX() - rowOriginsX[rowId]).getStorage();
    if (xOffset < 0) {
      continue;
    }
    if (!checkerModeled) {
      // Core cell whose master carries no implant shapes: invisible to the
      // checker (no InstanceId) but it occupies sites, so the 100%-utility
      // precheck and window boundaries must account for it.
      coverage_extras_.push_back(CoverageExtra{
          rowId,
          xOffset,
          static_cast<ipl::Dbu>(master.getWidth().getStorage()),
          static_cast<ipl::Dbu>(master.getHeight().getStorage())});
      continue;
    }
    // NOTE: initFromUDM keeps a non-site-aligned instance (diagnostic only),
    // so the id is still assigned. Mirror that: no alignment filter here.
    const ipl::InstanceId instId = nextInstId++;
    inst_by_leaf_[static_cast<int>(lcId.getIndexValue())] = instId;
    leaf_by_inst_[instId] = lcId;
  }
}

ipl::InstanceId UdmIdBridge::instanceIdOf(eUNL::LeafCellID cellId) const
{
  const auto it = inst_by_leaf_.find(static_cast<int>(cellId.getIndexValue()));
  return it != inst_by_leaf_.end() ? it->second : -1;
}

eUNL::LeafCellID UdmIdBridge::leafCellOf(ipl::InstanceId instanceId) const
{
  const auto it = leaf_by_inst_.find(instanceId);
  return it != leaf_by_inst_.end() ? it->second : eUNL::LeafCellID();
}

ipl::MasterId UdmIdBridge::masterIdOf(const eLIB::PhysLibCell& master) const
{
  const auto it = master_by_cell_.find(&master);
  return it != master_by_cell_.end() ? it->second : -1;
}

ipl::MasterId UdmIdBridge::masterIdOf(eLIB::LibCellID libCellId) const
{
  const auto it =
      master_by_libcell_.find(static_cast<int>(libCellId.getIndexValue()));
  return it != master_by_libcell_.end() ? it->second : -1;
}

const eLIB::PhysLibCell* UdmIdBridge::physLibCellOf(
    ipl::MasterId masterId) const
{
  const auto it = cell_by_master_.find(masterId);
  return it != cell_by_master_.end() ? it->second : nullptr;
}

std::pair<ipl::Dbu, ipl::Dbu> UdmIdBridge::rowSpan(ipl::RowId rowId) const
{
  if (rowId < 0 || rowId >= static_cast<ipl::RowId>(row_spans_.size())) {
    return {0, 0};
  }
  return row_spans_[static_cast<size_t>(rowId)];
}

bool UdmIdBridge::validate(std::vector<std::string>& problems) const
{
  const auto& masters = checker_.masters();
  if (masters.size() != cell_by_master_.size()) {
    problems.push_back("master count mismatch: checker "
                       + std::to_string(masters.size()) + " vs bridge "
                       + std::to_string(cell_by_master_.size()));
  }
  for (const ipl::MasterInput& mi : masters) {
    const eLIB::PhysLibCell* cell = physLibCellOf(mi.masterId);
    if (cell == nullptr) {
      problems.push_back("bridge missing master "
                         + std::to_string(mi.masterId));
      continue;
    }
    if (static_cast<ipl::Dbu>(cell->getWidth().getStorage()) != mi.width
        || static_cast<ipl::Dbu>(cell->getHeight().getStorage())
               != mi.height) {
      problems.push_back("master " + std::to_string(mi.masterId)
                         + " width/height mismatch vs UDM");
    }
    const bool isFiller = cell->getType().isCoreFiller()
                          || cell->getType().isPadFiller();
    if (isFiller != mi.isFiller) {
      problems.push_back("master " + std::to_string(mi.masterId)
                         + " isFiller mismatch vs UDM");
    }
  }
  if (checker_.placedInsts().size() != leaf_by_inst_.size()) {
    problems.push_back("instance count mismatch: checker "
                       + std::to_string(checker_.placedInsts().size())
                       + " vs bridge "
                       + std::to_string(leaf_by_inst_.size()));
  }
  if (!row_origins_aligned_) {
    problems.push_back(
        "row origin X differs across rows: the checker frame is per-row "
        "relative and the planner assumes one shared x frame [VERIFY-UDM]");
  }
  return problems.empty();
}

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
