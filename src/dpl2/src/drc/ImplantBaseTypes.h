// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// UDM-free base types for the filler repair planner (dpl2::fillerRepair).
//
// HISTORY / WARNING: originally extracted from the (now removed)
// ImplantLayerCheckerHelper.h to share ids with the checker. The 2026-07-12
// checker update defines its own copies of these names (Dbu, LayerId,
// XInterval, ...) directly inside ImplantLayerChecker.h in the same ipl
// namespace and no longer includes this header -- so this header is now
// PLANNER-ONLY. Never include it and ImplantLayerChecker.h in the same
// translation unit (ODR clash on XInterval etc.). Follow-up (AGENTS D17):
// move the planner to fully self-owned base types in the fillerRepair
// namespace and retire this file before the real adapter is written.
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
