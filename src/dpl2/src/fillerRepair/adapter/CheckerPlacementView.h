// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// fillerRepair::PlacementView backed by the checker's normalized tables
// (masters()/placedInsts()/rows()/layers()/siteWidth()) plus the UdmIdBridge
// for row legal spans. Everything is snapshotted once at construction into
// plain deterministic maps -- the planner never touches UDM or live checker
// state during a repair, and the view is immutable for the repair's duration
// (rebuild it after any commit).
//
// Frame: the checker frame -- x is DBU relative to the row origin
// (x = colId * siteWidth). VT = implant-layer FAMILY of the master's shapes
// (VTS=0 VTL=1 VTH=2 VTUL=3, mirroring ipl::Family; unknown -> kUnknownVt).
//
// Coverage extras: placed core cells the checker does not model (masters
// without implant shapes) are inserted as synthetic NON-filler instances
// with negative ids so the 100%-utility precheck and window boundaries see
// full site coverage. Negative ids never leave the planner (no checker
// violation can reference them; isFiller=false keeps them out of swaps).

#pragma once

#include <map>
#include <vector>

#include "../Log.h"
#include "../PlacementView.h"
#include "UdmIdBridge.h"
#include "drc/ImplantLayerChecker.h"

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

Orient toPlannerOrient(eUTL::PhysOrientation orientation);
eUTL::PhysOrientation toUdmOrient(Orient orient);

class CheckerPlacementView : public PlacementView
{
 public:
  CheckerPlacementView(const ipl::ImplantLayerChecker& checker,
                       const UdmIdBridge& bridge,
                       const DebugLog& log);

  // PlacementView ------------------------------------------------------------
  std::vector<RowId> rows() const override;
  XInterval rowLegalSpan(RowId rowId) const override;
  DbCoord siteWidth() const override { return site_width_; }
  std::vector<PlacedInstance> instancesInRow(RowId rowId) const override;
  const PlacedInstance* instance(InstanceId id) const override;
  const MasterInfo* masterInfo(MasterId id) const override;

 private:
  DbCoord site_width_ = 1;
  std::map<RowId, XInterval> row_spans_;                    // deterministic
  std::map<MasterId, MasterInfo> masters_;
  std::map<InstanceId, PlacedInstance> instances_;
  std::map<RowId, std::vector<PlacedInstance>> by_row_;     // sorted by x
};

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
