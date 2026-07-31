// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2024-2025, The OpenROAD Authors

#pragma once
#include <string>
#include <variant>

#include <Coordinates.h>
#include <dpl2/DePlace.h>

// UDM
#include <phys/fpManager.hh>
#include <phys/physDesMgr.hh>
#include <phys/physHier.hh>
#include <util/iter.hh>

using eLIB::PhysLibCell;
using eLIB::TechSite;
using eUNL::LeafCellID;
using eUNL::LibCellID;
using eUNL::PhysPin;
using eUTL::Rect;
using eUTL::UvDist;
using eUTL::Point2D;
using eUTL::PhysOrientationE;
using eUTL::PhysOrientation;

namespace dpl2 {

class MasterEdge
{
 public:
  MasterEdge(unsigned int type, const Rect& box);
  unsigned int getEdgeType() const;
  const Rect& getBBox() const;

 private:
  unsigned int edge_type_idx_{0};
  Rect bbox_;
};

class Master
{
 public:
  bool isMultiRow() const;
  // Master classification is assigned from fillerSetting::isFillerCell() when
  // infrastructure imports or refreshes the configured filler core list.
  bool isFiller() const;
  void setFiller(bool filler) { is_filler_ = filler; }
  const std::vector<MasterEdge>& getEdges() const;
  void setMultiRow(bool in);
  void addEdge(const MasterEdge& edge);
  void setBBox(const Rect& box);
  void clearEdges();
  ADD_SETTER_GETTER_PP(int, Id, id_);
  ADD_SETTER_GETTER_PP(LibCellID, DbMaster, db_master_);
  ADD_SETTER_GETTER_PP(int, BottomPowerType, bottom_pwr_);
  ADD_SETTER_GETTER_PP(int, TopPowerType, top_pwr_);
  void setPhysLibCell(const PhysLibCell* phys_lib_cell) { phys_lib_cell_ = phys_lib_cell; }
  const PhysLibCell* getPhysLibCell() const { return phys_lib_cell_; }

 private:
  int id_{0};
  LibCellID db_master_;
  const PhysLibCell* phys_lib_cell_{nullptr};
  Rect boundary_box_;
  bool is_multi_row_{false};
  bool is_filler_{false};
  std::vector<MasterEdge> edges_;
  int bottom_pwr_{0};
  int top_pwr_{0};
};

class Pin;
class Group;

class Node
{
 public:
  enum Type
  {
    UNKNOWN,
    CELL,
    TERMINAL,
    MACROCELL,
    FILLER
  };
  ~Node();
  ADD_SETTER_GETTER_PP(int, Id, id_);
  ADD_SETTER_GETTER_PP(DbuX, Left, left_);
  ADD_SETTER_GETTER_PP(DbuY, Bottom, bottom_);
  ADD_SETTER_GETTER_PP(PhysOrientation, Orient, orient_);
  ADD_SETTER_GETTER_PP(DbuX, OrigLeft, orig_left_);
  ADD_SETTER_GETTER_PP(DbuY, OrigBottom, orig_bottom_);
  ADD_SETTER_GETTER_PP(DbuX, Width, width_);
  ADD_SETTER_GETTER_PP(DbuY, Height, height_);
  ADD_SETTER_GETTER_PP(Type, Type, type_);
  ADD_SETTER_GETTER_PP(bool, Fixed, fixed_);
  ADD_SETTER_GETTER_PP(bool, Placed, placed_);
  ADD_SETTER_GETTER_PP(bool, Hold, hold_);
  ADD_SETTER_GETTER_PP(int, TopPower, powerTop_);
  ADD_SETTER_GETTER_PP(int, BottomPower, powerBot_);
  ADD_SETTER_GETTER_PP(int, GroupId, group_id_);
  ADD_SETTER_GETTER_PP(uint8_t, UsedLayer, used_layers_);
  ADD_SETTER_GETTER_PTR_PP(Master, Master, master_);
  ADD_SETTER_GETTER_PTR_PP(Group, Group, group_);
  ADD_SETTER_GETTER_PTR_PP(const Rect, Region, region_);

  LeafCellID getDbInst() const;
  DbuX getRight() const;
  DbuY getTop() const;
  DbuX getCenterX() const;
  DbuY getCenterY() const;
  bool isFixed() const;
  bool isPlaced() const;
  bool isHold() const;
  const TechSite* getSite() const;
  DbuX siteWidth() const;
  int64_t area() const;

  bool isTerminal() const;
  bool isFiller() const;
  bool isStdCell() const;
  bool isBlock() const;
  bool inGroup() const;
  Rect getBBox() const;

  void setDbInst(LeafCellID cellId);
  void addUsedLayer(int layer);
  bool adjustCurrOrient(const PhysOrientation& newOrient);

 protected:
  int id_{0};
  LeafCellID db_owner_;
  // Current position; bottom corner.
  DbuX left_{0};
  DbuY bottom_{0};
  PhysOrientation orient_;
  // Original position.
  DbuX orig_left_{0};
  DbuY orig_bottom_{0};
  // Width and height.
  DbuX width_{0};
  DbuY height_{0};
  // Type.
  Type type_{UNKNOWN};
  // Fixed or not fixed.
  bool fixed_{false};
  bool placed_{false};
  bool hold_{false};
  // For power.
  int powerTop_{0};
  int powerBot_{0};
  // Master and edges
  Master* master_{nullptr};
  Group* group_{nullptr};
  const Rect* region_{nullptr};  // group rect
  // // Regions.
  int group_id_{-1};
  // used layers
  uint8_t used_layers_{0};
};

class Group
{
 public:
  const std::vector<Rect>& getRects() const;
  std::vector<Node*> getCells() const;
  const Rect& getBBox() const;

  void addRect(const Rect& in);
  void addCell(Node* cell);

  ADD_SETTER_GETTER_PP(int, Id, id_);
  ADD_SETTER_GETTER_PP(std::string, Name, name_);
  ADD_SETTER_GETTER_PP(Rect, Boundary, boundary_);
  ADD_SETTER_GETTER_PP(double, Util, util_);

 private:
  int id_{0};
  std::string name_;
  std::vector<Rect> region_boundaries_;
  std::vector<Node*> cells_;
  Rect boundary_;
  double util_{0.0};
};

class Edge
{
 public:
  ADD_SETTER_GETTER_PP(int, Id, id_);

  int getNumPins() const;
  const std::vector<Pin*>& getPins() const;
  void addPin(Pin* pin);
  void removePin(Pin* pin);
  uint64_t hpwl() const;

 private:
  int id_{0};
  std::vector<Pin*> pins_;
};

class Pin
{
 public:
  enum Direction
  {
    Dir_IN,
    Dir_OUT,
    Dir_INOUT,
    Dir_UNKNOWN
  };

  Pin();
  ADD_SETTER_GETTER_PP(int, Direction, dir_);
  ADD_SETTER_GETTER_PTR_PP(Node, Node, node_);
  ADD_SETTER_GETTER_PTR_PP(Edge, Edge, edge_);
  ADD_SETTER_GETTER_PP(DbuX, OffsetX, offsetX_);
  ADD_SETTER_GETTER_PP(DbuY, OffsetY, offsetY_);
  ADD_SETTER_GETTER_PP(int, PinLayer, pinLayer_);
  ADD_SETTER_GETTER_PP(DbuX, PinWidth, pinWidth_);
  ADD_SETTER_GETTER_PP(DbuY, PinHeight, pinHeight_);

 private:
  friend class Node;
  // Pin width and height.
  DbuX pinWidth_{0};
  DbuY pinHeight_{0};
  // Direction.
  int dir_{Dir_INOUT};
  // Layer.
  int pinLayer_{0};
  // Node and edge for pin.
  Node* node_{nullptr};
  Edge* edge_{nullptr};
  // Offsets from cell center.
  DbuX offsetX_{0};
  DbuY offsetY_{0};
};

enum class OpType : uint8_t {
    Replace = 0,
    Delete = 1,
    Add    = 2,
};

using CellData = std::variant<std::string, LeafCellID>;

struct CellChangeRecord {
    OpType    op_;
    CellData    cell_data_;
    UvDist    origin_x_;
    UvDist    origin_y_;
    LibCellID    orig_lib_cell_;
    LibCellID    new_lib_cell_;
    PhysOrientation orientation_;
  };

/**
 * Lightweight lookup table that maps edge type name strings to integer
 * indices.
 */
class EdgeTypeTable
{
 public:
  template <typename InputIt>
  void addNames(InputIt first, InputIt last)
  {
    for (auto it = first; it != last; ++it) {
      edge_types_indices_.try_emplace(*it, edge_types_indices_.size());
    }
  }

  bool hasTable() const { return !edge_types_indices_.empty(); }
  int getEdgeTypeIdx(const std::string& edge_type) const;
  std::size_t size() const { return edge_types_indices_.size(); }

  using const_iterator = std::unordered_map<std::string, int>::const_iterator;
  const_iterator begin() const { return edge_types_indices_.begin(); }
  const_iterator end() const { return edge_types_indices_.end(); }

 private:
  std::unordered_map<std::string, int> edge_types_indices_;
};
} // namespace dpl2
