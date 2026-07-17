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
  // [fillerRepair-fix] was `id` (undeclared) instead of `idx`.
  Node* getNode(int idx) const {
     return (idx >= 0 && idx < static_cast<int>(nodes_.size())) ? nodes_[idx].get() : nullptr;
  }
  Master* getMaster(int idx) const {
     return (idx >= 0 && idx< static_cast<int>(masters_.size())) ? masters_[idx].get() : nullptr; 
  }
  int getNodeId(LeafCellID cellId) const {
    int ret = -1;
    auto it = inst_to_node_idx_.find(cellId);
    if (it != inst_to_node_idx_.end()) {
      return it->second;
    }
    return ret;
  }
  int getMasterId(LibCellID db_master) const {
    int ret = -1;
    auto it = master_to_idx_.find(db_master);
    if (it != master_to_idx_.end()) {
      return it->second;
    }
    return ret;
  }

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
  // [fillerRepair-fix] was unique_ptr<Node*> / inst_to_node_idx__ (typos).
  void addNode(std::unique_ptr<Node> n) {
    inst_to_node_idx_[n->getDbInst()] = nodes_.size();
    nodes_.emplace_back(std::move(n));
    cells_cnt_++;
  }

  void addMaster(std::unique_ptr<Master> m) {
    master_to_idx_[m->getDbMaster()] = masters_.size();
    masters_.emplace_back(std::move(m));
  }

 private:
  int cells_cnt_ = 0;
  Rect core_;  // Core area of the design.
  std::vector<std::unique_ptr<Master>> masters_;
  std::vector<std::unique_ptr<Node>> nodes_;  // The nodes in the netlist...

  std::unordered_map<LeafCellID, int> inst_to_node_idx_;
  std::unordered_map<LibCellID, int> master_to_idx_;
};

}  // namespace dpl2
