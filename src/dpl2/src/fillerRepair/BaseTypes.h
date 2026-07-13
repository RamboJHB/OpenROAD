// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// UDM-free primitive types owned by the filler repair planner.
//
// The real checker owns a separate wire representation in
// drc/ImplantLayerChecker.h. The eventual adapter converts explicitly between
// the two APIs; keeping these definitions in fillerRepair lets both headers be
// included in one translation unit without namespace/type collisions.

#pragma once

#include <cstdint>

namespace dpl2::fillerRepair {

using DbCoord = int64_t;
using LayerId = int32_t;
using MasterId = int32_t;
using InstanceId = int32_t;
using ShapeId = int32_t;
using RowId = int32_t;

// Half-open interval [xl, xh).
struct XInterval
{
  DbCoord xl = 0;
  DbCoord xh = 0;

  DbCoord length() const { return xh - xl; }
  bool empty() const { return xh <= xl; }
  bool overlaps(const XInterval& other) const
  {
    return xl < other.xh && other.xl < xh;
  }
  bool contains(DbCoord x) const { return x >= xl && x < xh; }
  bool operator==(const XInterval& other) const
  {
    return xl == other.xl && xh == other.xh;
  }
};

}  // namespace dpl2::fillerRepair
