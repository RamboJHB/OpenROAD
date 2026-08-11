// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2021-2025, The OpenROAD Authors
#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include <Coordinates.h>
#include <Objects.h>
#include <architecture.h>
#include <dpl2/DePlace.h>

#include <vector>

namespace dpl2 {

class fillerSetting;

class Network
{
public:
  // [FRPORT] [fillerRepair-fix] Non-owning binding to DePlace's active setting.
  // DePlace owns both objects and guarantees the setting outlives Network.
  void setFillerSetting(const fillerSetting* setting) { filler_setting_ = setting; }
  const fillerSetting* getFillerSetting() const { return filler_setting_; }

  std::vector<std::unique_ptr<Node>>& getNodes() { return nodes_; }
  std::vector<std::unique_ptr<Master>>& getMasters() {return masters_;}
  // [FRPORT] Imported Nodes inherit the authoritative Master filler flag.
  // For creating and adding cells.
  bool addNode(LeafCellID cellId, const PhysDesMgr* desMgr);
  Node* getNode(LeafCellID cellId);
  Node* getNode(int id) const {
    return (id >= 0 && id < static_cast<int>(nodes_.size())) ?
      nodes_[id].get() : nullptr;
  }
  Master* getMaster(int id) const {
    return (id >= 0 && id < static_cast<int>(masters_.size())) ?
      masters_[id].get() : nullptr;
  }
  int getMasterId(LibCellID id) const {
    int ret = -1;
    auto it = master_to_idx_.find(id);
    if (it != master_to_idx_.end()) {
      ret = it->second;
    }
    return ret;
  }
  int getNodeId(LeafCellID id) const {
    int ret = -1;
    auto it = inst_to_node_idx_.find(id);
    if (it != inst_to_node_idx_.end()) {
      return it->second;
    }
    return ret;
  }

  // [FRPORT] Test/opto proposals refresh the Node from a registered Master.
  bool updateNode(Node* ndi,
                  const PhysDesMgr* desMgr,
                  const PhysLibCell& physLibCell);

  void setCore(const Rect& core) { core_ = core; }
  const Rect& getCore() const { return core_; }
  Master* getMaster(LibCellID db_master);
  // [FRPORT] Configured fillers must be registered through this edge-aware path.
  // For creating masters.
  Master* addMaster(const PhysLibCell& db_master,
                    const fillerSetting& filler_setting,
                    const Grid* grid,
                    const EdgeTypeTable* edge_types);

  bool addNode(std::unique_ptr<Node> n) {
    if (n == nullptr) {
      return false;
    }
    inst_to_node_idx_[n->getDbInst()] = nodes_.size();
    nodes_.emplace_back(std::move(n));
    cells_cnt_++;
    return true;
  }
  bool addMaster(std::unique_ptr<Master> m) {
    if (m == nullptr) {
      return false;
    }
    master_to_idx_[m->getDbMaster()] = masters_.size();
    masters_.emplace_back(std::move(m));
    return true;
  }
private:
  // [FRPORT] Borrowed configuration used by FillerRepairEngine construction.
  const fillerSetting* filler_setting_ = nullptr;
  int cells_cnt_ = 0;
  Rect core_; // Core area of the design.
  std::vector<std::unique_ptr<Master>> masters_;
  std::vector<std::unique_ptr<Node>> nodes_;  // The nodes in the netlist..

  std::unordered_map<LeafCellID, int> inst_to_node_idx_;
  std::unordered_map<LibCellID, int> master_to_idx_;
};

}  // namespace dpl2
