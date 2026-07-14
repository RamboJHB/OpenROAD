
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2024-2025, The OpenROAD Authors

#pragma once

#include <unordered_map>
#include <utility>

#include "Coordinates.h"
#include "dpl2/DePlace.h"

// UDM
#include <phys/fpManager.hh>
#include <phys/physDesMgr.hh>
#include <phys/physHier.hh>
#include <phys/HierImpl.hh>
#include <util/iter.hh>
using eLIB::PhysMacroType;
using eLIB::PhysLibCell;
using eUNL::PhysCell;
using eUNL::LeafCellID;

namespace dpl2 {
class Node;
class Padding
{
public:
    GridX padGlobalLeft() const { return pad_left_ ; }
    GridX padGlobalRight() const { return pad_right_ ; }

    void setPaddingGlobal(GridX left, GridX right);
    void setPadding(LeafCellID cellId, GridX left, GridX right);
    void setPadding(PhysLibCell* master, GridX left, GridX right);
    bool havePadding() const;

    // Find instance/master/global padding value for an instance.
    GridX padLeft(const Node* cell) const;
    GridX padRight(const Node* cell) const;
    bool isPaddedType(const PhysLibCell* master) const;
    DbuX paddedWidth(const Node* cell) const;
    void setDesginManager(const PhysDesMgr* desMgr);

private:
    using InstPaddingMap = std::unordered_map<LeafCellID, std::pair<GridX, GridX>>;
    using MasterPaddingMap = std::unordered_map<const PhysLibCell*, std::pair<GridX, GridX>>;

    GridX pad_left_{0};
    GridX pad_right_{0};
    InstPaddingMap inst_padding_map_ ;
    MasterPaddingMap master_padding_map_ ;
    const PhysDesMgr* desMgr_;
};

} // namespace dpl2
