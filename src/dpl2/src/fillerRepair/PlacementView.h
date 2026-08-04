// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Seam 1 of 2: how the search sees the design. (Seam 2 is RepairOracle --
// whether something is legal.) The runtime engine implements both; the search
// itself knows nothing about UDM, Grid or Network.
//
// It answers exactly two questions -- "what is placed where?" and "which
// masters could replace this filler?" -- and it is read-only. Nothing here
// changes the design; committing an answer is the caller's job.
//
// Threading: one repair at a time, so these const methods are NOT required to
// be safe for concurrent readers -- the runtime fills per-row caches lazily
// from inside them. What const does promise is that calling one never changes
// what a later call answers, and that any reference handed out stays alive as
// long as the view does.

#pragma once

#include <algorithm>
#include <vector>

#include <fillerRepair/Debug.h>
#include <fillerRepair/RepairTypes.h>

namespace dpl2::fillerRepair {

// --- "which masters could replace this filler?" -----------------------------

struct MasterCandidateResult
{
  std::vector<MasterId> candidates;
  std::vector<Diagnostic> diagnostics;
};

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

  // Sorted ascending. The reference stays valid for the data source's lifetime;
  // window building and guard clamping call this on every step, so
  // implementations must NOT rebuild the list per call.
  virtual const std::vector<RowId>& rows() const = 0;

  virtual DbCoord siteWidth() const = 0;

  // Horizontal distance the legality oracle can inspect outside an edited
  // interval. Lightweight planner views may use one site; the runtime engine
  // overrides this with the checker's exact reach.
  virtual DbCoord checkerReachX() const { return siteWidth(); }

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
  // Build the exact checker/public wire record for one planner swap. This is
  // the sole mapping point from dense planner ids to UDM ids and coordinates.
  virtual CellChangeRecord cellChangeRecord(InstanceId instanceId,
                                            MasterId newMasterId) const = 0;
  // Has a working default built from the accessors above; virtual so a view
  // that already knows its usable replacements can answer directly instead of
  // being re-derived.
  virtual MasterCandidateResult getUsableMasterCandidates(
      InstanceId fillerInstanceId) const;

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

// A row on a real design holds thousands of instances; we care about the
// handful near the target. `instancesInRow` is sorted by x and -- on this path,
// which only runs once the region is known gap- and overlap-free -- the
// instances do not overlap, so their right edges rise monotonically too. That
// is what lets the two searches below jump straight to the range of interest
// instead of walking the row.

// First index whose right edge lies strictly right of `bound` (the first
// instance not entirely to the left of it).
inline int firstRightEdgeAfter(const PlacementView& view,
                               const std::vector<PlacedInstance>& all,
                               DbCoord bound)
{
  int lo = 0;
  for (int hi = static_cast<int>(all.size()); lo < hi;) {
    const int mid = lo + (hi - lo) / 2;
    if (instanceSpan(view, all[mid]).xh > bound) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

// First index whose left edge is at or right of `bound`.
inline int firstStartAtOrAfter(const std::vector<PlacedInstance>& all,
                               DbCoord bound)
{
  int lo = 0;
  for (int hi = static_cast<int>(all.size()); lo < hi;) {
    const int mid = lo + (hi - lo) / 2;
    if (all[mid].x >= bound) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

// Everything in one row that overlaps `x`, plus `ring` more whole instances
// off each end:
//
//     x:                 [-------)
//     row:  [ A ][ B ][ C ][ D ][ E ][ F ][ G ]
//     ring=1 gives:      B  C  D  E  F     (C..E overlap, B and F are the ring)
//
// Counted by INDEX, so every kind of cell counts -- a std cell is a ring
// member like any other and never stops the walk. When `x` falls in a gap the
// overlap range is empty and the ring simply yields the nearest instance on
// each side, which is what a caller looking for neighbours wants.
inline std::vector<PlacedInstance> instancesInRing(const PlacementView& view,
                                                   RowId rowId,
                                                   const XInterval& x,
                                                   int ring)
{
  const std::vector<PlacedInstance>& all = view.instancesInRow(rowId);
  std::vector<PlacedInstance> result;
  const int n = static_cast<int>(all.size());
  if (n == 0) {
    return result;
  }

  const int lo = firstRightEdgeAfter(view, all, x.xl);
  const int hi = firstStartAtOrAfter(all, x.xh);
  const int from = std::max(0, lo - ring);
  const int to = std::min(n, hi + ring);
  for (int i = from; i < to; ++i) {
    result.push_back(all[i]);
  }
  return result;
}

// --- inline implementations ------------------------------------------------

inline const std::vector<PlacedInstance>& PlacementView::emptyInstances()
{
  static const std::vector<PlacedInstance> kEmpty;
  return kEmpty;
}

inline MasterCandidateResult PlacementView::getUsableMasterCandidates(
    InstanceId fillerInstanceId) const
{
  MasterCandidateResult result;
  const PlacedInstance* inst = instance(fillerInstanceId);
  if (inst == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "UnknownInstance",
        cat("instance ", fillerInstanceId, " not found")));
    return result;
  }
  if (!inst->isFiller) {
    result.diagnostics.push_back(makeDiag(
        Severity::Warning, "NotAFiller",
        cat("instance ", fillerInstanceId, " is not a filler")));
    return result;
  }
  const MasterInfo* current = masterInfo(inst->masterId);
  if (current == nullptr || !current->isFiller) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "UnknownMaster",
        cat("invalid current filler master for instance ", fillerInstanceId)));
    return result;
  }

  if (current->vt == kUnknownVt) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "UnknownCurrentVt",
        cat("current filler master ", inst->masterId,
            " has no checker VT")));
    return result;
  }

  // fillerMasterIds() is sorted unique by contract -> candidates come out
  // ascending and deterministic without a per-call sort.
  int polarityFiltered = 0;
  for (const MasterId id : fillerMasterIds()) {
    const MasterInfo* candidate = masterInfo(id);
    if (candidate == nullptr) {
      result.diagnostics.push_back(makeDiag(
          Severity::Warning, "UnknownConfiguredMaster",
          cat("configured filler master ", id, " is not in the view")));
      continue;
    }
    // Same size, different (known) VT family, and the same R0-frame band
    // polarity layout: a swap keeps position/orientation, so a candidate
    // whose bottom band has the opposite polarity would land every band on
    // the wrong track -- the checker rejects such overlays unconditionally,
    // offering them only burns checker calls.
    if (id != inst->masterId && candidate->isFiller
        && candidate->vt != kUnknownVt && candidate->vt != current->vt
        && candidate->width == current->width
        && candidate->height == current->height) {
      if (candidate->bottomBandPolarity != current->bottomBandPolarity) {
        ++polarityFiltered;
        continue;
      }
      result.candidates.push_back(id);
    }
  }
  if (result.candidates.empty()) {
    // Distinguish "the library has nothing" from "everything size/VT
    // compatible was dropped by the polarity-layout filter": the latter
    // pattern usually means the polarity metadata (layer-name parse) is
    // broken, and silently reporting NoUsableMaster would hide it.
    if (polarityFiltered > 0) {
      result.diagnostics.push_back(makeDiag(
          Severity::Warning, "PolarityLayoutFiltered",
          cat(polarityFiltered, " same-size VT replacement(s) for instance ",
              fillerInstanceId,
              " dropped only by the band-polarity layout filter")));
    } else {
      result.diagnostics.push_back(makeDiag(
          Severity::Info, "NoUsableMaster",
          cat("no configured same-size VT replacement for instance ",
              fillerInstanceId)));
    }
  }
  return result;
}

}  // namespace dpl2::fillerRepair
