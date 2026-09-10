// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2021-2025, The OpenROAD Authors

#pragma once
#include <Coordinates.h>
#include <Objects.h>
#include <architecture.h>
#include <dpl2/DePlace.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace dpl2 {

class fillerSetting;

class Network
{
 public:
  std::map<int, std::unique_ptr<Node>>& getNodes() { return nodes_; }
  const std::map<int, std::unique_ptr<Node>>& getNodes() const { return nodes_; }
  std::map<int, std::unique_ptr<Master>>& getMasters() { return masters_; }
  const std::map<int, std::unique_ptr<Master>>& getMasters() const
  { return masters_; }
  // For creating and adding cells.
  void addNode(LeafCellID cellId, const PhysDesMgr* desMgr);
  void addFillerNode(LeafCellID cellId, const PhysDesMgr* desMgr);
  Node* addNode(LibCellID lcId, DbuX x, DbuY y, const eUNL::Design* design);
  void deleteNode(Node* cell);
  Node* getNode(LeafCellID cellId);
  Node* getNode(int id) const {
    auto it = nodes_ .find(id);
    return it != nodes_.end() ? it->second.get() : nullptr;
  }
  Master* getMaster(int id) const {
    auto it = masters_ .find(id);
    return it != masters_.end() ? it->second.get() : nullptr;
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

  bool updateNode(Node* ndi,
                  const PhysDesMgr* desMgr,
                  const PhysLibCell& physLibCell);

  void setCore(const Rect& core) { core_ = core; }
  const Rect& getCore() const { return core_; }
  Master* getMaster(LibCellID db_master);
  // For creating masters.
  Master* addMaster(const PhysLibCell& db_master,
                    const fillerSetting& filler_setting,
                    const Grid* grid,
                    const EdgeTypeTable* edge_types);

  // [FRPORT] Setup-only classification barrier. Call after the Network and
  // final fillerSetting are complete, but before constructing DRC checkers.
  void updateFillerClassification(const fillerSetting& filler_setting);

  void addPin(const eLIB::PhysLibPort* libport, Master* master);

  void addNode(std::unique_ptr<Node> n) {
    // Respect an id already assigned by the caller (used by tests); otherwise
    // fall back to a fresh id from the monotonic counter.
    const int id = n->getId() >= 0 ? n->getId() : next_node_id_++;
    n->setId(id);
    next_node_id_ = std::max(next_node_id_, id + 1);
    const LeafCellID instId = n->getDbInst();
    if (instId.isValid()) {
      inst_to_node_idx_[instId] = id;
    }
    nodes_.emplace(id, std::move(n));
  }
  void addMaster(std::unique_ptr<Master> m) {
    const int id = m->getId() >= 0 ? m->getId() : next_master_id_++;
    master_to_idx_[m->getDbMaster()] = id;
    masters_.emplace(id, std::move(m));
  }
  void setFillerSetting(const fillerSetting* setting) {
    filler_setting_ = setting;
  }
  const fillerSetting* getFillerSetting() const { return filler_setting_;}
 private:

  const fillerSetting* filler_setting_ = nullptr;
  void connect(Pin* pin, Master* master);
  Rect core_;  // Core area of the design;
  // Masters/nodes are keyed by their stable id (ascending allocation order), so
  // iteration preserves insertion order and lookup/removal by id is O(log n).
  std::map<int, std::unique_ptr<Master>> masters_;
  std::map<int, std::unique_ptr<Node>> nodes_;  // The nodes in the netlist...
  std::vector<std::unique_ptr<Pin>> pins_;     // The pins in the network...

  // Ids are allocated from a monotonic counter and never reused or renumbered,
  // so an id is a stable identifier independent of container order, even after
  // Network::deleteNode().
  int next_node_id_ = 0;
  int next_master_id_ = 0;

  // Maps the db identifier (LeafCellID/LibCellID) to the stable id.
  std::unordered_map<LeafCellID, int> inst_to_node_idx_;
  std::unordered_map<LibCellID, int> master_to_idx_;
};

}  // namespace dpl2
