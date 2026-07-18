// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "PlacementView.h"

#include "Log.h"

namespace dpl2::fillerRepair {

const std::vector<PlacedInstance>& PlacementView::emptyInstances()
{
  static const std::vector<PlacedInstance> kEmpty;
  return kEmpty;
}

MasterCandidateResult PlacementView::getUsableMasterCandidates(
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
