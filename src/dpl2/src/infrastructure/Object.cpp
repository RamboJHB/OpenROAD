// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2024-2025, The OpenROAD Authors
// [FRPORT] Implements the shared Master/Node filler classification contract.

#include <Objects.h>


namespace dpl2 {
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
MasterEdge::MasterEdge(unsigned int type, const eUTL::Rect& box)
    : edge_type_idx_(type), bbox_(box)
{
}

unsigned int MasterEdge::getEdgeType() const
{
  return edge_type_idx_;
}
const eUTL::Rect& MasterEdge::getBBox() const
{
  return bbox_;
}
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
bool Master::isMultiRow() const
{
  return is_multi_row_;
}
const std::vector<MasterEdge>& Master::getEdges() const
{
  return edges_;
}
void Master::setMultiRow(const bool in)
{
  is_multi_row_ = in;
}
void Master::addEdge(const MasterEdge& edge)
{
  edges_.emplace_back(edge);
}
void Master::clearEdges()
{
  edges_.clear();
}
void Master::setBBox(const Rect& box)
{
  boundary_box_ = box;
}
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

Node::~Node() = default;

DbuX Node::getRight() const
{
  return left_ + width_;
}
DbuY Node::getTop() const
{
  return bottom_ + height_;
}
DbuX Node::getCenterX() const
{
  return left_ + width_ / DbuX{2};
}
DbuY Node::getCenterY() const
{
  return bottom_ + height_ / DbuY{2};
}
LeafCellID Node::getDbInst() const
{
  return db_owner_;
}
bool Node::isFixed() const
{
  return fixed_;
};
bool Node::isPlaced() const
{
  return placed_;
}
bool Node::isHold() const
{
  return hold_;
}
const TechSite* Node::getSite() const
{
  if (master_ && master_->getPhysLibCell()) {
    return master_->getPhysLibCell()->getTechSite();
  }
  return nullptr;
}
DbuX Node::siteWidth() const
{
  if (master_ && master_->getPhysLibCell()) {
    auto site = master_->getPhysLibCell()->getTechSite();
    if (site) {
      return DbuX{site->getWidth().getStorage()};
    }
  }
  return DbuX{0};
}
// bool Node::isHybrid() const
// {
//   odb::dbSite* site = getSite();
//   return site ? site->isHybrid() : false;
// }
// bool Node::isHybridParent() const
// {
//   odb::dbSite* site = getSite();
//   return site ? site->hasRowPattern() : false;
// }
int64_t Node::area() const
{
  if (master_ && master_->getPhysLibCell()) {
    const PhysLibCell* master = master_->getPhysLibCell();
    return int64_t(master->getWidth().getStorage()) *
      master->getHeight().getStorage();
  }
  return 0;
}
// std::string Node::name(eUNL::PhysEditor* phys_editor) const
// {
//   if (type_ == CELL) {
//     return phys_editor->getLeafCellName(getDbInst());
//   }
//   if (type_ == TERMINAL) {
//     return getBTerm()->getName();
//   }
//   if (type_ == FILLER) {
//     return fmt::format("FILLER_{}", id_);
//   }
//   return "";
// }
bool Node::isTerminal() const
{
  return (type_ == TERMINAL);
}
bool Node::isFiller() const
{
  return (type_ == FILLER);
}
bool Master::isFiller() const
{
  return is_filler_;
}
// PhysMacroType::isCore() can also be true for a configured filler. Use the
// Node type inherited from Master instead of re-reading the UDM macro type.
bool Node::isStdCell() const
{
  if (master_ && master_->getPhysLibCell()) {
    const PhysLibCell& cell = *master_->getPhysLibCell();
    return (cell.getType().isCore() || cell.getType().isEndcap())
           && !isFiller();
  }
  return false;
}
bool Node::isBlock() const
{
  if (master_ && master_->getPhysLibCell()) {
    return master_->getPhysLibCell()->getType().isBlock();
  }
  return false;
}
bool Node::inGroup() const
{
  return group_ != nullptr;
}
Rect Node::getBBox() const
{
  return Rect(
      UvDist(left_.v), UvDist(bottom_.v), UvDist(left_.v + width_.v),
      UvDist(bottom_.v + height_.v));
}
void Node::setDbInst(LeafCellID cellId)
{
  db_owner_ = cellId;
}

void Node::addUsedLayer(int layer)
{
  used_layers_ |= 1 << layer;
}
bool Node::adjustCurrOrient(const PhysOrientation& newOri)
{

  // Change the orientation of the cell, but leave the lower-left corner
  // alone.  This means changing the locations of pins and possibly
  // changing the edge types as well as the height and width.
  auto curOri = orient_;
  if (newOri == curOri) {
    return true;
  }

  if (curOri.getValue() == PhysOrientationE::R90 ||
      curOri.getValue() == PhysOrientationE::MX90
      || curOri.getValue() == PhysOrientationE::R270 ||
      curOri.getValue() == PhysOrientationE::MY90) {
    if (newOri.getValue() == PhysOrientationE::R0 ||
      newOri.getValue() == PhysOrientationE::MY
      || newOri.getValue() == PhysOrientationE::MX ||
      newOri.getValue() == PhysOrientationE::R180) {
      // Rotate the cell counter-clockwise by 90 degrees.
      // for (Pin* pin : pins_) {
      //   const auto dx = pin->getOffsetX().v;
      //   const auto dy = pin->getOffsetY().v;
      //   pin->setOffsetX(DbuX{-dy});
      //   pin->setOffsetY(DbuY{dx});
      // }
      {
        int tmp = width_.v;
        width_ = DbuX{height_.v};
        height_ = DbuY{tmp};
      }
      if (curOri.getValue() == PhysOrientationE::R90) {
        curOri = PhysOrientationE::R0;
      } else if (curOri.getValue() == PhysOrientationE::MX90) {
        curOri = PhysOrientationE::MX;
      } else if (curOri.getValue() == PhysOrientationE::MY90) {
        curOri = PhysOrientationE::MY;
      } else {
        curOri = PhysOrientationE::R180;
      }
    }
  } else {
    if (newOri.getValue() == PhysOrientationE::R90 ||
        newOri.getValue() == PhysOrientationE::MX90
        || newOri.getValue() == PhysOrientationE::MY90 ||
        newOri.getValue() == PhysOrientationE::R270) {
      // Rotate the cell clockwise by 90 degrees.
      // for (Pin* pin : pins_) {
      //   const auto dx = pin->getOffsetX().v;
      //   const auto dy = pin->getOffsetY().v;
      //   pin->setOffsetX(DbuX{dy});
      //   pin->setOffsetY(DbuY{-dx});
      // }
      {
        int tmp = width_.v;
        width_ = DbuX{height_.v};
        height_ = DbuY{tmp};
      }
      if (curOri.getValue() == PhysOrientationE::R0) {
        curOri = PhysOrientationE::R90;
      } else if (curOri.getValue() == PhysOrientationE::MX) {
        curOri = PhysOrientationE::MX90;
      } else if (curOri.getValue() == PhysOrientationE::MY) {
        curOri = PhysOrientationE::MY90;
      } else {
        curOri = PhysOrientationE::R270;
      }
    }
  }
  // Both the current and new orientations should be {N, FN, FS, S} or {E, FE,
  // FW, W}.
  int mX = 1;
  int mY = 1;
  if (curOri.getValue() == PhysOrientationE::R90 ||
      curOri.getValue() == PhysOrientationE::MX90
      || curOri.getValue() == PhysOrientationE::MY90 ||
      curOri.getValue() == PhysOrientationE::R270) {
    const bool test1
      = (curOri.getValue() == PhysOrientationE::R90 ||
         curOri.getValue() == PhysOrientationE::MY90);
    const bool test2
      = (newOri.getValue() == PhysOrientationE::R90 ||
         newOri.getValue() == PhysOrientationE::MY90);
    if (test1 != test2) {
      mX = -1;
    }
    const bool test3
      = (curOri.getValue() == PhysOrientationE::R90 ||
         curOri.getValue() == PhysOrientationE::MX90);
    const bool test4
      = (newOri.getValue() == PhysOrientationE::R90 ||
         newOri.getValue() == PhysOrientationE::MX90);
    if (test3 != test4) {
      mY = -1;
    }
  } else {
    const bool test1
      = (curOri.getValue() == PhysOrientationE::R0 ||
         curOri.getValue() == PhysOrientationE::MX);
    const bool test2
      = (newOri.getValue() == PhysOrientationE::R0 ||
         newOri.getValue() == PhysOrientationE::MX);
    if (test1 != test2) {
      mX = -1;
    }
    const bool test3
      = (curOri.getValue() == PhysOrientationE::R0 ||
         curOri.getValue() == PhysOrientationE::MY);
    const bool test4
      = (newOri.getValue() == PhysOrientationE::R0 ||
         newOri.getValue() == PhysOrientationE::MY);
    if (test3 != test4) {
      mY = -1;
    }
  }

  // for (Pin* pin : pins_) {
  //   pin->setOffsetX(pin->getOffsetX() * DbuX{mX});
  //   pin->setOffsetY(pin->getOffsetY() * DbuY{mY});
  // }
  orient_ = newOri;
  return true;
}
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
const std::vector<Rect>& Group::getRects() const
{
  return region_boundaries_;
}
std::vector<Node*> Group::getCells() const
{
  return cells_;
}
const Rect& Group::getBBox() const
{
  return boundary_;
}
void Group::addRect(const Rect& in)
{
  region_boundaries_.emplace_back(in);
}
void Group::addCell(Node* cell)
{
  cells_.emplace_back(cell);
}
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
int Edge::getNumPins() const
{
  return (int) pins_.size();
}
const std::vector<Pin*>& Edge::getPins() const
{
  return pins_;
}
void Edge::addPin(Pin* pin)
{
  pins_.emplace_back(pin);
}
void Edge::removePin(Pin* pin)
{
  std::erase(pins_, pin);
}
uint64_t Edge::hpwl() const
{
  Rect rect(UvDist(INT_MAX), UvDist(INT_MAX), UvDist(INT_MIN), UvDist(INT_MIN));
  for (const Pin* pinj : getPins()) {
    const Node* ndj = pinj->getNode();

    const DbuX x = ndj->getCenterX() + pinj->getOffsetX();
    const DbuY y = ndj->getCenterY() + pinj->getOffsetY();

    rect.expand(Point2D(UvDist(x.v), UvDist(y.v)));
  }

  return rect.halfPerimeter().getStorage();
}

int EdgeTypeTable::getEdgeTypeIdx(const std::string& edge_type) const
{
  auto it = edge_types_indices_.find(edge_type);
  if (it != edge_types_indices_.end()) {
    return it->second;
  }
  return -1;
}

} // namespace dpl2
