// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// UDM-free base types shared by the implant checker (dpl2::ipl) and the
// filler repair planner (dpl2::fillerRepair).
//
// Extracted from ImplantLayerCheckerHelper.h so pure components can reuse
// the exact same ids and interval type without pulling the UDM headers.
// XInterval keeps its original aggregate layout (brace init unchanged for
// existing checker code); only convenience methods were added.
//
// Convention: intervals are half-open [xl, xh).

#pragma once

#include <cstdint>

namespace dpl2 {
namespace ipl {

using DbCoord = int64_t;
using LayerId = int32_t;
using MasterId = int32_t;
using InstanceId = int32_t;
using ShapeId = int32_t;
using RowId = int32_t;
using GroupId = int32_t;

struct XInterval
{
  DbCoord xl = 0;
  DbCoord xh = 0;

  DbCoord length() const { return xh - xl; }
  bool empty() const { return xh <= xl; }
  bool overlaps(const XInterval& o) const { return xl < o.xh && o.xl < xh; }
  bool contains(DbCoord x) const { return x >= xl && x < xh; }
  bool operator==(const XInterval& o) const { return xl == o.xl && xh == o.xh; }
};

}  // namespace ipl
}  // namespace dpl2
