// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "RepairInfrastructure.h"

#include <algorithm>
#include <map>

namespace dpl2 {

void RepairInfrastructure::fail(const std::string& message)
{
  diagnostics_.push_back(message);
  ready_ = false;
}

bool RepairInfrastructure::build(
    eUNL::PhysDesMgr* desMgr,
    const std::vector<eUNL::LeafCellID>& leafCells,
    const fillerSetting& fillerSettings,
    const eLIB::PhysLibCell& targetNewMaster,
    Config config)
{
  if (built_) {
    fail("RepairInfrastructure is a one-design snapshot; rebuild a new object");
    return false;
  }
  built_ = true;
  if (desMgr == nullptr) {
    fail("missing PhysDesMgr");
    return false;
  }
  if (fillerSettings.getDesign() == nullptr
      || fillerSettings.getDesign()->getPhysDesMgr() != desMgr) {
    fail("fillerSetting and PhysDesMgr do not describe the same design");
    return false;
  }
  if (fillerSettings.getFillerMasters().empty()) {
    // Without a configured allow list the engine could never offer a swap;
    // fail here instead of building a snapshot that only rejects later.
    fail("fillerSetting::getFillerMasters() is empty");
    return false;
  }
  if (leafCells.empty()) {
    fail("no leaf cells supplied for Network import");
    return false;
  }

  bool haveCore = false;
  eUTL::Rect core;
  for (const eUNL::PhysRow& row : desMgr->getPhysRowIter()) {
    if (row.getSite().getIsPad()) {
      continue;
    }
    core = haveCore ? core.expand(row.getBbox()) : row.getBbox();
    haveCore = true;
  }
  if (!haveCore || core.dx().getStorage() <= 0 || core.dy().getStorage() <= 0) {
    fail("no usable non-pad row core");
    return false;
  }

  padding_->setDesginManager(desMgr);
  grid_.setCore(core);
  grid_.examineRows(desMgr);
  grid_.initGrid(desMgr,
                 padding_,
                 config.maxDisplacementX,
                 config.maxDisplacementY);
  network_.setCore(core);

  // LibCellID order makes Master::getId() deterministic and includes
  // uninstantiated candidate/target masters before checker construction.
  std::map<eLIB::LibCellID, const eLIB::PhysLibCell*> masters;
  std::vector<eUNL::LeafCellID> cells = leafCells;
  std::sort(cells.begin(), cells.end());
  cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
  for (const eUNL::LeafCellID cellId : cells) {
    const eUNL::PhysCell cell = desMgr->getPhysCell(cellId);
    if (!cell.isValid()) {
      fail("leaf cell " + std::to_string(cellId.getIndexValue())
           + " is absent from PhysDesMgr");
      continue;
    }
    const eLIB::PhysLibCell& master = cell.getPhysMaster();
    masters[master.getLibCellId()] = &master;
  }
  for (const eLIB::PhysLibCell* master : fillerSettings.getFillerMasters()) {
    if (master == nullptr) {
      fail("getFillerMasters returned null");
      continue;
    }
    masters[master->getLibCellId()] = master;
  }
  masters[targetNewMaster.getLibCellId()] = &targetNewMaster;

  for (const auto& [id, master] : masters) {
    (void) id;
    network_.addMaster(*master, &grid_);
  }
  if (network_.getMasters().empty()) {
    fail("no physical masters imported");
    return false;
  }
  for (const eUNL::LeafCellID cellId : cells) {
    if (!desMgr->getPhysCell(cellId).isValid()) {
      continue;
    }
    network_.addNode(cellId, desMgr);
  }

  // Populate the real Grid occupancy from the Nodes just imported.  The
  // checker currently uses Grid geometry, while this occupancy keeps the
  // snapshot valid for other infrastructure consumers as well.
  for (const auto& node : network_.getNodes()) {
    if (node != nullptr) {
      grid_.paintPixel(node.get());
    }
  }

  ready_ = diagnostics_.empty();
  return ready_;
}

}  // namespace dpl2
