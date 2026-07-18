// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Read-only placement/DB view consumed by the repair planner.
//
// This is the planner's only window into the infrastructure database. The
// FillerRepairEngine's private view wraps UDM; fake/FakeDesign.h implements
// it for unit tests. The planner never mutates the design -- commit stays with
// the infrastructure (spec section 3.1).
//
// Thread model: after construction a production view is an immutable
// snapshot -- all const methods must be safe for CONCURRENT readers (any
// internal lazy cache must synchronize itself), and returned references stay
// valid for the view's lifetime. FakeDesign (test-only) keeps mutable
// builders and is single-threaded by design.

#pragma once

#include <vector>

#include "Types.h"

namespace dpl2::fillerRepair {

class DebugLog;

struct MasterInfo
{
  MasterId id = 0;
  DbCoord width = 0;
  DbCoord height = 0;
  bool isFiller = false;
  // VT family. Uniform across the master's bands by checker construction
  // (master_implant_family_mismatch), so one value covers every band.
  VtId vt = kUnknownVt;
  // Bottom-band implant polarity in the master's R0 frame; bands alternate
  // upward (checker rebuildMasterShapes, anchored at the bottommost shape's
  // layer). Placement under MX/R180 flips the bands, but a SWAP keeps
  // position AND orientation, so a replacement only has to match this
  // R0-frame layout -- a mismatched layout puts every band on the opposite
  // track and the checker rejects the overlay (polarity mismatch).
  BandPolarity bottomBandPolarity = BandPolarity::N;
};

struct PlacedInstance
{
  InstanceId id = 0;
  MasterId masterId = 0;
  RowId rowId = 0;
  DbCoord x = 0;
  Orient orientation = Orient::R0;
  bool isFiller = false;
};

class PlacementView
{
 public:
  virtual ~PlacementView() = default;

  // Sorted ascending. The reference stays valid for the view's lifetime;
  // window building and guard clamping call this on every step, so
  // implementations must NOT rebuild the list per call.
  virtual const std::vector<RowId>& rows() const = 0;

  // Legal std-cell site range of a row in DBU. Macros/blockages/core cutouts
  // must already be excluded by the production view; anything a
  // std cell or filler may legally occupy is inside this span.
  virtual XInterval rowLegalSpan(RowId rowId) const = 0;

  virtual DbCoord siteWidth() const = 0;

  // Sorted by x ascending (ties by id). Contract for multi-height (future):
  // an instance spanning several rows is reported by every row it occupies.
  // V1 designs are single-height. Returned by reference: this is the
  // planner's hottest query (precheck, window building, ranking) and rows
  // hold thousands of instances on real designs -- per-call copies are the
  // dominant planner cost, so implementations must return stored buckets.
  virtual const std::vector<PlacedInstance>& instancesInRow(
      RowId rowId) const = 0;

  // nullptr when unknown.
  virtual const PlacedInstance* instance(InstanceId id) const = 0;
  virtual const MasterInfo* masterInfo(MasterId id) const = 0;

  // Configured replacement universe, sorted ascending and unique (the
  // default candidate filter relies on that order for determinism).
  virtual const std::vector<MasterId>& fillerMasterIds() const = 0;
  virtual MasterCandidateResult getUsableMasterCandidates(
      const MasterCandidateRequest& request) const;
  virtual SiteCoverageResult checkSiteCoverage(const DebugLog& log) const;

 protected:
  // Shared "no such row" result so implementations can return a reference.
  static const std::vector<PlacedInstance>& emptyInstances();
};

// Occupied x span of a placed instance (width comes from its master).
inline XInterval instanceSpan(const PlacementView& view, const PlacedInstance& inst)
{
  const MasterInfo* master = view.masterInfo(inst.masterId);
  const DbCoord width = master != nullptr ? master->width : 0;
  return XInterval{inst.x, inst.x + width};
}

}  // namespace dpl2::fillerRepair
