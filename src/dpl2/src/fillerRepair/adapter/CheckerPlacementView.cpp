// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "CheckerPlacementView.h"

#include <algorithm>

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

Orient toPlannerOrient(eUTL::PhysOrientation orientation)
{
  if (orientation == eUTL::PhysOrientationE::R180) {
    return Orient::R180;
  }
  if (orientation == eUTL::PhysOrientationE::MX) {
    return Orient::MX;
  }
  if (orientation == eUTL::PhysOrientationE::MY) {
    return Orient::MY;
  }
  return Orient::R0;
}

eUTL::PhysOrientation toUdmOrient(Orient orient)
{
  switch (orient) {
    case Orient::R180:
      return eUTL::PhysOrientationE::R180;
    case Orient::MX:
      return eUTL::PhysOrientationE::MX;
    case Orient::MY:
      return eUTL::PhysOrientationE::MY;
    case Orient::R0:
      break;
  }
  return eUTL::PhysOrientationE::R0;
}

namespace {

// VT = the FAMILY of the implant layers under the master's shapes, exactly
// like the checker derives it (buildMasters requires a single family per
// master). VtId is pinned to the Family enum order: VTS=0 VTL=1 VTH=2 VTUL=3.
VtId vtOfMaster(const ipl::ImplantLayerChecker& checker,
                const ipl::MasterInput& master)
{
  for (const ipl::MasterShape& shape : master.shapes) {
    for (const ipl::ImplantLayer& layer : checker.layers()) {
      if (layer.id == shape.layer) {
        if (layer.family == ipl::Family::Unknown) {
          return kUnknownVt;
        }
        return static_cast<VtId>(layer.family);
      }
    }
  }
  return kUnknownVt;
}

}  // namespace

CheckerPlacementView::CheckerPlacementView(
    const ipl::ImplantLayerChecker& checker,
    const UdmIdBridge& bridge,
    const DebugLog& log)
{
  site_width_ = static_cast<DbCoord>(checker.siteWidth());
  const DbCoord rowHeight = static_cast<DbCoord>(bridge.rowHeight());

  for (const ipl::MasterInput& mi : checker.masters()) {
    MasterInfo info;
    info.id = static_cast<MasterId>(mi.masterId);
    info.width = static_cast<DbCoord>(mi.width);
    // Planner heights are in ROW units (single-height == 1).
    info.height = rowHeight > 0
                      ? static_cast<DbCoord>((mi.height + rowHeight - 1)
                                             / rowHeight)
                      : 1;
    info.isFiller = mi.isFiller;
    info.vt = vtOfMaster(checker, mi);
    masters_[info.id] = info;
  }

  for (const ipl::RowId rowId : checker.rows()) {
    const auto span = bridge.rowSpan(rowId);
    row_spans_[static_cast<RowId>(rowId)] =
        XInterval{static_cast<DbCoord>(span.first),
                  static_cast<DbCoord>(span.second)};
  }

  for (const auto& [id, inst] : checker.placedInsts()) {
    PlacedInstance p;
    p.id = static_cast<InstanceId>(inst.instanceId);
    p.masterId = static_cast<MasterId>(inst.masterId);
    p.rowId = static_cast<RowId>(inst.rowId);
    p.x = static_cast<DbCoord>(inst.colId) * site_width_;
    p.orientation = toPlannerOrient(inst.orientation);
    p.isFiller = inst.isFiller;
    instances_[p.id] = p;
    by_row_[p.rowId].push_back(p);
  }
  // Coverage extras: placed core cells the checker does NOT model (their
  // master has no implant shapes) still occupy sites. Insert them as
  // synthetic NON-filler instances with negative ids -- one synthetic master
  // per extra -- so the 100%-utility precheck and window boundaries see full
  // site coverage. They are never swap candidates (isFiller=false) and never
  // appear in checker violations, so the negative ids stay planner-internal.
  int extraIndex = 0;
  for (const UdmIdBridge::CoverageExtra& extra : bridge.coverageExtras()) {
    const InstanceId instId = static_cast<InstanceId>(-1 - extraIndex);
    const MasterId masterId = static_cast<MasterId>(-1 - extraIndex);
    ++extraIndex;

    MasterInfo info;
    info.id = masterId;
    info.width = static_cast<DbCoord>(extra.width);
    info.height = rowHeight > 0
                      ? static_cast<DbCoord>(
                          (static_cast<DbCoord>(extra.height) + rowHeight - 1)
                          / rowHeight)
                      : 1;
    info.isFiller = false;
    info.vt = kUnknownVt;
    masters_[masterId] = info;

    PlacedInstance p;
    p.id = instId;
    p.masterId = masterId;
    p.rowId = static_cast<RowId>(extra.rowId);
    p.x = static_cast<DbCoord>(extra.x);
    p.orientation = Orient::R0;
    p.isFiller = false;
    instances_[p.id] = p;
    // Multi-height extras are reported by every row they cover
    // (PlacementView contract).
    for (DbCoord r = 0; r < info.height; ++r) {
      PlacedInstance rowCopy = p;
      rowCopy.rowId = p.rowId + static_cast<RowId>(r);
      by_row_[rowCopy.rowId].push_back(rowCopy);
    }
  }

  for (auto& [rowId, list] : by_row_) {
    std::sort(list.begin(), list.end(),
              [](const PlacedInstance& a, const PlacedInstance& b) {
                return a.x != b.x ? a.x < b.x : a.id < b.id;
              });
  }

  log.msg("adapter",
          cat("view snapshot: ", instances_.size(), " instance(s) (",
              bridge.coverageExtras().size(), " coverage extra(s)), ",
              masters_.size(), " master(s), ", row_spans_.size(),
              " row(s), siteWidth=", site_width_));
}

std::vector<RowId> CheckerPlacementView::rows() const
{
  std::vector<RowId> ids;
  ids.reserve(row_spans_.size());
  for (const auto& [id, span] : row_spans_) {
    ids.push_back(id);
  }
  return ids;
}

XInterval CheckerPlacementView::rowLegalSpan(RowId rowId) const
{
  const auto it = row_spans_.find(rowId);
  return it != row_spans_.end() ? it->second : XInterval{};
}

std::vector<PlacedInstance> CheckerPlacementView::instancesInRow(
    RowId rowId) const
{
  const auto it = by_row_.find(rowId);
  return it != by_row_.end() ? it->second : std::vector<PlacedInstance>{};
}

const PlacedInstance* CheckerPlacementView::instance(InstanceId id) const
{
  const auto it = instances_.find(id);
  return it != instances_.end() ? &it->second : nullptr;
}

const MasterInfo* CheckerPlacementView::masterInfo(MasterId id) const
{
  const auto it = masters_.find(id);
  return it != masters_.end() ? &it->second : nullptr;
}

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
