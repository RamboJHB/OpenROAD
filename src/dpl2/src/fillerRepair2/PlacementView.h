// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Seam 1 of 2: how the search sees the design. (Seam 2 is RepairOracle --

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
  VtId vt = kUnknownVt;
// Bottom-band implant polarity in the master's R0 frame; bands alternate
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
  virtual const std::vector<RowId>& rows() const = 0;

  virtual DbCoord siteWidth() const = 0;

// Sorted by x ascending (ties by id). A multi-height instance is present in
// every row it occupies.
  virtual const std::vector<PlacedInstance>& instancesInRow(
      RowId rowId) const = 0;

  virtual const PlacedInstance* instance(InstanceId id) const = 0;
  virtual const MasterInfo* masterInfo(MasterId id) const = 0;

// Configured replacement universe, sorted ascending and unique (the
  virtual const std::vector<MasterId>& fillerMasterIds() const = 0;
// Build one planner Replace record; the engine assembles layout Delete/Add.
  virtual CellChangeRecord cellChangeRecord(InstanceId instanceId,
                                            MasterId newMasterId) const = 0;
// Has a working default built from the accessors above; virtual so a view
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

// First index whose right edge lies strictly right of `bound` (the first
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
