// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2021-2025, The OpenROAD Authors

#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include "Coordinates.h"
#include "Objects.h"
#include "architecture.h"
#include <dpl2/DePlace.h>
#include <dpl2/PlacementDRC.h>

#include <vector>

namespace dpl2 {

class Network
{
 public:
  std::vector<std::unique_ptr<Node>>& getNodes() { return nodes_; }
  const std::vector<std::unique_ptr<Node>>& getNodes() const { return nodes_; }
  std::vector<std::unique_ptr<Master>>& getMasters() { return masters_; }
  const std::vector<std::unique_ptr<Master>>& getMasters() const
  {
    return masters_;
  }
  // For creating and adding cells.
  void addNode(LeafCellID cellId, const PhysDesMgr* desMgr);
  Node* getNode(LeafCellID cellId);
  const Node* getNode(LeafCellID cellId) const;
  bool updateNode(Node* ndi,
                  const PhysDesMgr* desMgr,
                  const PhysLibCell& physLibCell);

  void setCore(const Rect& core) { core_ = core; }
  const Rect& getCore() const { return core_; }
  Master* getMaster(LibCellID db_master);
  const Master* getMaster(LibCellID db_master) const;
  // For creating masters.
  Master* addMaster(const PhysLibCell& db_master,
                    const Grid* grid,
                    const PlacementDRC* drc_engine);

 private:
  int cells_cnt_ = 0;
  Rect core_;  // Core area of the design.
  std::vector<std::unique_ptr<Master>> masters_;
  std::vector<std::unique_ptr<Node>> nodes_;  // The nodes in the netlist...

  std::unordered_map<LeafCellID, int> inst_to_node_idx_;
  std::unordered_map<LibCellID, int> master_to_idx_;
};

}  // namespace dpl2
