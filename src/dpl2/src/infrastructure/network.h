1 // SPDX-License-Identifier: BSD-3-Clause
2 // Copyright (c) 2021-2025, The OpenROAD Authors
3 
4 #pragma once
5 #include <string>
6 #include <unordered_map>
7 
8 #include <Coordinates.h>
9 #include <Objects.h>
10 #include <architecture.h>
11 #include <dpl2/DePlace.h>
12 
13 #include <vector>
14 
15 namespace dpl2 {
16 
17 class Network
18 {
19 public:
20   std::vector<std::unique_ptr<Node>>& getNodes() { return nodes_; }
21   std::vector<std::unique_ptr<Master>>& getMasters() {return masters_;}
22   // For creating and adding cells.
23   void addNode(LeafCellID cellId, const PhysDesMgr* desMgr);
24   Node* getNode(LeafCellID cellId);
25   Node* getNode(int id) const {
26     return (id >= 0 && id < static_cast<int>(nodes_.size())) ?
27       nodes_[id].get() : nullptr;
28   }
29   Master* getMaster(int id) const {
30     return (id >= 0 && id < static_cast<int>(masters_.size())) ?
31       masters_[id].get() : nullptr;
32   }
33   int getMasterId(LibCellID id) const {
34     int ret = -1;
35     auto it = master_to_idx_.find(id);
36     if (it != master_to_idx_.end()) {
37       ret = it->second;
38     }
39     return ret;
40   }
41   int getNodeId(LeafCellID id) const {
42     int ret = -1;
43     auto it = inst_to_node_idx_.find(id);
44     if (it != inst_to_node_idx_.end()) {
45       return it->second;
46     }
47     return ret;
48   }
49 
50   bool updateNode(Node* ndi,
51                   const PhysDesMgr* desMgr,
52                   const PhysLibCell& physLibCell);
53 
54   void setCore(const Rect& core) { core_ = core; }
55   const Rect& getCore() const { return core_; }
56   Master* getMaster(LibCellID db_master);
57   // For creating masters.
58   Master* addMaster(const PhysLibCell& db_master,
59                     const Grid* grid,
60                     const EdgeTypeTable* edge_types);
61 
62   void addNode(std::unique_ptr<Node> n) {
63     inst_to_node_idx_[n->getDbInst()] = nodes_.size();
64     nodes_.emplace_back(std::move(n));
65     cells_cnt_++;
66   }
67   void addMaster(std::unique_ptr<Master> m) {
68     master_to_idx_[m->getDbMaster()] = masters_.size();
69     masters_.emplace_back(std::move(m));
70   }
71 private:
72   int cells_cnt_ = 0;
73   Rect core_; // Core area of the design.
74   std::vector<std::unique_ptr<Master>> masters_;
75   std::vector<std::unique_ptr<Node>> nodes_;  // The nodes in the netlist..
76 
77   std::unordered_map<LeafCellID, int> inst_to_node_idx_;
78   std::unordered_map<LibCellID, int> master_to_idx_;
79 };
80 
81 }  // namespace dpl2