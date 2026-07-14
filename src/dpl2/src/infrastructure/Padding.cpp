// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2024-2025, The OpenROAD Authors

#include "Padding.h"
#include "Objects.h"
#include "dpl2/DePlace.h"
#include "infrastructure/Coordinates.h"

namespace dpl2 {

void Padding::setPaddingGlobal(GridX left, GridX right)
{
    pad_left_ = left;
    pad_right_ = right;
}

void Padding::setPadding(LeafCellID cellId, GridX left, GridX right)
{
    inst_padding_map_[cellId] = {left, right};
}

void Padding::setPadding(PhysLibCell* master, GridX left, GridX right)
{
    master_padding_map_[master] = {left, right};
}

bool Padding::havePadding() const
{
    return pad_left_ > 0 || pad_right_ > 0 || !master_padding_map_.empty()
        || !inst_padding_map_.empty();
}

bool Padding::isPaddedType(const PhysLibCell* master) const
{
    using eLIB::PhysMacroType;
    PhysMacroType type = master->getType();
    // Use switch so if new Types are added we get a compiler warning.
    switch (type.getType()) {
    case PhysMacroType::TypeE::CORE:
    case PhysMacroType::TypeE::CORE_ANTENNACELL:
    case PhysMacroType::TypeE::CORE_FEEDTHRU:
    case PhysMacroType::TypeE::CORE_TIEHIGH:
    case PhysMacroType::TypeE::CORE_TIELOW:
    case PhysMacroType::TypeE::CORE_WELLTAP:
        return true;
    case PhysMacroType::TypeE::ENDCAP:
    case PhysMacroType::TypeE::ENDCAP_PRE:
    case PhysMacroType::TypeE::ENDCAP_POST:
    case PhysMacroType::TypeE::ENDCAP_LEF58_RIGHTEDGE:
    case PhysMacroType::TypeE::ENDCAP_LEF58_LEFTEDGE:
    case PhysMacroType::TypeE::CORE_FILLER:
    case PhysMacroType::TypeE::BLOCK:
    case PhysMacroType::TypeE::BLOCK_BLACKBOX:
    case PhysMacroType::TypeE::BLOCK_SOFT:
    case PhysMacroType::TypeE::ENDCAP_TOPLEFT:
    case PhysMacroType::TypeE::ENDCAP_TOPRIGHT:
    case PhysMacroType::TypeE::ENDCAP_BOTTOMLEFT:
    case PhysMacroType::TypeE::ENDCAP_BOTTOMRIGHT:
    case PhysMacroType::TypeE::ENDCAP_LEF58_BOTTOMEDGE:
    case PhysMacroType::TypeE::ENDCAP_LEF58_TOPEDGE:
    case PhysMacroType::TypeE::ENDCAP_LEF58_RIGHTBOTTOMEDGE:
    case PhysMacroType::TypeE::ENDCAP_LEF58_LEFTBOTTOMEDGE:
    case PhysMacroType::TypeE::ENDCAP_LEF58_RIGHTTOPEDGE:
    case PhysMacroType::TypeE::ENDCAP_LEF58_LEFTTOPEDGE:
    case PhysMacroType::TypeE::ENDCAP_LEF58_RIGHTBOTTOMCORNER:
    case PhysMacroType::TypeE::ENDCAP_LEF58_LEFTBOTTOMCORNER:
    case PhysMacroType::TypeE::ENDCAP_LEF58_RIGHTTOPCORNER:
    case PhysMacroType::TypeE::ENDCAP_LEF58_LEFTTOPCORNER:
        // These classes are completely ignored by the placer.
    case PhysMacroType::TypeE::COVER:
    case PhysMacroType::TypeE::COVER_BUMP:
    case PhysMacroType::TypeE::RING:
    case PhysMacroType::TypeE::PAD:
    case PhysMacroType::TypeE::PAD_AREAIO:
    case PhysMacroType::TypeE::PAD_INPUT:
    case PhysMacroType::TypeE::PAD_OUTPUT:
    case PhysMacroType::TypeE::PAD_INOUT:
    case PhysMacroType::TypeE::PAD_POWER:
    case PhysMacroType::TypeE::PAD_FILLER:
        return false;
    }
    // gcc warning
    return false;
}

GridX Padding::padLeft(const Node* cell) const
{
    if (isPaddedType(cell->getMaster()->getPhysLibCell())) {
        auto itr1 = inst_padding_map_.find(cell->getDbInst());
        if (itr1 != inst_padding_map_.end()) {
            return itr1->second.first;
        }
        auto itr2 = master_padding_map_.find(cell->getMaster()->getPhysLibCell());
        if (itr2 != master_padding_map_.end()) {
            return itr2->second.first;
        }
        return pad_left_;
    }
    return GridX{0};
}

GridX Padding::padRight(const Node* cell) const
{
    if (isPaddedType(cell->getMaster()->getPhysLibCell())) {
        auto itr1 = inst_padding_map_.find(cell->getDbInst());
        if (itr1 != inst_padding_map_.end()) {
            return itr1->second.second;
        }
        auto itr2 = master_padding_map_.find(cell->getMaster()->getPhysLibCell());
        if (itr2 != master_padding_map_.end()) {
            return itr2->second.second;
        }
        return pad_right_;
    }
    return GridX{0};
}

DbuX Padding::paddedWidth(const Node* cell) const
{
    return cell->getWidth()
        + gridToDbu(padLeft(cell) + padRight(cell), cell->siteWidth());
}

void Padding::setDesginManager(const PhysDesMgr* desMgr)
{
    desMgr_ = desMgr;
}

} // namespace dpl2
