// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// UDM <-> checker id bridge.
//
// ImplantLayerChecker::initFromUDM assigns its MasterId/InstanceId/RowId as
// SEQUENTIAL ids in enumeration order and does not expose the mapping back to
// UDM objects. The adapter needs that mapping in both directions:
//   - upstream identifies the opto target by LeafCellID and the new master by
//     PhysLibCell, and wants FillerChange results back in UDM terms;
//   - participants/candidates work in checker id space.
//
// This bridge REPLAYS the exact enumeration/filter logic of initFromUDM
// (steps 1/3/5a/5b/6 in ImplantLayerChecker.cpp) against the same PhysDesMgr,
// so the synthesized ids line up with the checker's. It must be kept in
// lockstep with initFromUDM -- any change to its iteration order or filters
// changes the id assignment. [VERIFY-UDM] The robust long-term fix is for the
// checker to expose its masterToId / instId maps; until then this file
// mirrors it and validate() cross-checks the reconstruction against the
// checker's public tables (count, width, height, isFiller per id).
//
// Coordinate frame (pinned by initFromUDM): RowId = PhysRow iteration index;
// an instance's x is DBU RELATIVE TO ITS ROW ORIGIN (colId = x / siteWidth).
// The planner assumes one shared x frame across rows, so validate() also
// checks that every non-pad row has the same origin X and reports a
// diagnostic when not. [VERIFY-UDM]

#pragma once

#include <map>
#include <string>
#include <vector>

// UDM (mirrors the checker header's include set).
#include <phys/physDesMgr.hh>
#include <physHierImpl.hh>
#include <libObjAccessor.hh>
#include <unl/unlObjTypes.hh>

#include "drc/ImplantLayerChecker.h"

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

class UdmIdBridge
{
 public:
  // Replays initFromUDM's enumeration against `desMgr`. `checker` must have
  // been initialized from the SAME desMgr state (no commits in between).
  UdmIdBridge(const eUNL::PhysDesMgr& desMgr,
              const ipl::ImplantLayerChecker& checker);

  // --- instances -----------------------------------------------------------
  // -1 when the leaf cell was not part of the checker's placed set.
  ipl::InstanceId instanceIdOf(eUNL::LeafCellID cellId) const;
  // Invalid LeafCellID (isValid()==false on the returned id) when unknown.
  eUNL::LeafCellID leafCellOf(ipl::InstanceId instanceId) const;

  // --- masters -------------------------------------------------------------
  // -1 when the master carries no implant shapes (not in the checker set).
  ipl::MasterId masterIdOf(const eLIB::PhysLibCell& master) const;
  ipl::MasterId masterIdOf(eLIB::LibCellID libCellId) const;
  // nullptr when unknown.
  const eLIB::PhysLibCell* physLibCellOf(ipl::MasterId masterId) const;

  // --- rows / frame --------------------------------------------------------
  ipl::Dbu siteWidth() const { return site_width_; }
  ipl::Dbu rowHeight() const { return row_height_; }
  // Row legal span in the checker frame (x relative to the row origin),
  // half-open [0, width). Empty when the row id is unknown.
  // [VERIFY-UDM] Derived from PhysRow bbox width; adjust the accessor if the
  // row API differs (getBbox() vs getWidth()/site count).
  std::pair<ipl::Dbu, ipl::Dbu> rowSpan(ipl::RowId rowId) const;
  int rowCount() const { return static_cast<int>(row_spans_.size()); }

  // Placed cells the CHECKER does not model (master without implant shapes,
  // e.g. physical-only cells): they still occupy sites, so the 100%-utility
  // precheck and window boundaries must see them. Reported in the checker
  // frame; they never get a checker InstanceId.
  struct CoverageExtra
  {
    ipl::RowId rowId = 0;   // row containing the cell origin
    ipl::Dbu x = 0;         // relative to the row origin
    ipl::Dbu width = 0;     // DBU
    ipl::Dbu height = 0;    // DBU; multi-row cells cover ceil(h/rowHeight) rows
  };
  const std::vector<CoverageExtra>& coverageExtras() const
  {
    return coverage_extras_;
  }

  // Reconstruction sanity: compares the replayed master table against the
  // checker's public masters()/placedInsts() (sizes and per-id width/height/
  // isFiller). Returns false and fills `problems` when they diverge -- which
  // means this file fell out of lockstep with initFromUDM.
  bool validate(std::vector<std::string>& problems) const;

 private:
  std::map<int, ipl::InstanceId> inst_by_leaf_;   // LeafCellID index value ->
  std::map<ipl::InstanceId, eUNL::LeafCellID> leaf_by_inst_;
  std::map<const eLIB::PhysLibCell*, ipl::MasterId> master_by_cell_;
  std::map<int, ipl::MasterId> master_by_libcell_;  // LibCellID index value ->
  std::map<ipl::MasterId, const eLIB::PhysLibCell*> cell_by_master_;
  std::vector<std::pair<ipl::Dbu, ipl::Dbu>> row_spans_;
  std::vector<bool> row_is_pad_;
  std::vector<CoverageExtra> coverage_extras_;
  ipl::Dbu site_width_ = 0;
  ipl::Dbu row_height_ = 0;
  bool row_origins_aligned_ = true;
  const ipl::ImplantLayerChecker& checker_;
};

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
