// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The planner's read-only view of the placement: the first of the two seams
// the runtime engine implements (the other is RepairOracle).
//
// It answers "what is placed where, and which masters may replace a filler" --
// nothing else. The planner never mutates the design; commit stays with the
// infrastructure owner.
//
// Thread model: after initialization the runtime snapshot is immutable, so all
// const methods must be safe for CONCURRENT readers (any internal lazy cache
// must synchronize itself) and returned references stay valid for the view's
// lifetime. Test implementations may use mutable builders but are test-only.

#pragma once

#include <vector>

#include <fillerRepair/Debug.h>
#include <fillerRepair/RepairTypes.h>

namespace dpl2::fillerRepair {

// --- Infrastructure candidate query ---------------------------------------

struct MasterCandidateRequest
{
  InstanceId fillerInstanceId = 0;
};

struct MasterCandidate
{
  MasterId masterId = 0;
};

struct MasterCandidateResult
{
  std::vector<MasterCandidate> candidates;
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
  virtual FillerCellRecord fillerCellRecord(InstanceId instanceId,
                                            MasterId newMasterId) const = 0;
  virtual MasterCandidateResult getUsableMasterCandidates(
      const MasterCandidateRequest& request) const;

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

// `instancesInRow` is x-sorted and, on the planner path (which only runs after
// a clean gap/overlap snapshot), non-overlapping -- so each instance's right
// edge is non-decreasing. That lets a window scan binary-search to the
// relevant x-range instead of walking the whole row, which is decisive on
// 100%-utilization designs where a row holds thousands of instances but only a
// sparse minority are editable fillers near the target.

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

// --- inline implementations (merged from PlacementView.cpp) ---------

inline const std::vector<PlacedInstance>& PlacementView::emptyInstances()
{
  static const std::vector<PlacedInstance> kEmpty;
  return kEmpty;
}

inline MasterCandidateResult PlacementView::getUsableMasterCandidates(
    const MasterCandidateRequest& request) const
{
  MasterCandidateResult result;
  const PlacedInstance* inst = instance(request.fillerInstanceId);
  if (inst == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "UnknownInstance",
        cat("instance ", request.fillerInstanceId, " not found")));
    return result;
  }
  if (!inst->isFiller) {
    result.diagnostics.push_back(makeDiag(
        Severity::Warning, "NotAFiller",
        cat("instance ", request.fillerInstanceId, " is not a filler")));
    return result;
  }
  const MasterInfo* current = masterInfo(inst->masterId);
  if (current == nullptr || !current->isFiller) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "UnknownMaster",
        cat("invalid current filler master for instance ", request.fillerInstanceId)));
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
      result.candidates.push_back(MasterCandidate{id});
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
              request.fillerInstanceId,
              " dropped only by the band-polarity layout filter")));
    } else {
      result.diagnostics.push_back(makeDiag(
          Severity::Info, "NoUsableMaster",
          cat("no configured same-size VT replacement for instance ",
              request.fillerInstanceId)));
    }
  }
  return result;
}

}  // namespace dpl2::fillerRepair
