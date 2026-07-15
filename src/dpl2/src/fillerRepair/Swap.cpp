// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "Swap.h"

#include <algorithm>
#include <utility>

#include "Log.h"
#include "Window.h"

namespace dpl2::fillerRepair {

std::optional<Swap> makeSwap(const PlacementView& view,
                             InstanceId instanceId,
                             MasterId newMasterId,
                             std::string* error)
{
  const auto fail = [error](std::string why) -> std::optional<Swap> {
    if (error != nullptr) {
      *error = std::move(why);
    }
    return std::nullopt;
  };

  const PlacedInstance* inst = view.instance(instanceId);
  if (inst == nullptr) {
    return fail(cat("instance ", instanceId, " not found"));
  }
  if (!inst->isFiller) {
    return fail(cat("instance ", instanceId, " is not a filler"));
  }
  const MasterInfo* oldMaster = view.masterInfo(inst->masterId);
  const MasterInfo* newMaster = view.masterInfo(newMasterId);
  if (oldMaster == nullptr || newMaster == nullptr) {
    return fail(cat("unknown master (old=", inst->masterId, " new=", newMasterId, ")"));
  }
  if (!newMaster->isFiller) {
    return fail(cat("master ", newMasterId, " is not a filler master"));
  }
  if (newMasterId == inst->masterId) {
    return fail(cat("master ", newMasterId, " is the current master"));
  }
  if (newMaster->width != oldMaster->width || newMaster->height != oldMaster->height) {
    return fail(cat("size mismatch: master ", newMasterId, " w=", newMaster->width,
                    " h=", newMaster->height, " vs current w=", oldMaster->width,
                    " h=", oldMaster->height));
  }

  Swap swap;
  swap.instanceId = instanceId;
  swap.oldMasterId = inst->masterId;
  swap.newMasterId = newMasterId;
  swap.rowId = inst->rowId;
  swap.span = XInterval{inst->x, inst->x + oldMaster->width};
  swap.oldVt = oldMaster->vt;
  swap.newVt = newMaster->vt;
  return swap;
}

std::string canonicalKey(const Overlay& overlay)
{
  std::vector<std::pair<InstanceId, MasterId>> pairs;
  pairs.reserve(overlay.size());
  for (const Swap& swap : overlay) {
    pairs.emplace_back(swap.instanceId, swap.newMasterId);
  }
  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

  std::string key;
  for (const auto& [instanceId, masterId] : pairs) {
    key += cat('i', instanceId, 'm', masterId, '|');
  }
  return key;
}

std::vector<FillerChange> toFillerChanges(const Overlay& overlay)
{
  std::vector<FillerChange> changes;
  changes.reserve(overlay.size());
  for (const Swap& swap : overlay) {
    changes.push_back(swap.change());
  }
  std::sort(changes.begin(),
            changes.end(),
            [](const FillerChange& a, const FillerChange& b) {
              return a.instanceId < b.instanceId;
            });
  return changes;
}

SwapGenerationResult generateSwaps(
    const RepairWindow& window,
    const PlacementView& view,
    const DebugLog& log)
{
  SwapGenerationResult result;

  for (const InstanceId fillerId : window.editableFillers) {
    MasterCandidateResult candidates =
        view.getUsableMasterCandidates({fillerId});
    // Provider diagnostics (unknown instance, not a filler, ...) are kept:
    // they explain why a filler contributed no moves.
    result.diagnostics.insert(result.diagnostics.end(),
                              candidates.diagnostics.begin(),
                              candidates.diagnostics.end());

    if (candidates.candidates.empty()) {
      result.diagnostics.push_back(
          makeDiag(Severity::Info, "NoUsableMaster",
                   cat("filler ", fillerId, ": no same-size replacement")));
      log.msg("swapgen",
              cat("filler ", fillerId, " -> 0 candidates, no swaps"));
      continue;
    }

    // Deterministic per-filler order regardless of provider ordering.
    std::sort(candidates.candidates.begin(),
              candidates.candidates.end(),
              [](const MasterCandidate& a, const MasterCandidate& b) {
                return a.masterId < b.masterId;
              });

    int emitted = 0;
    for (const MasterCandidate& candidate : candidates.candidates) {
      std::string error;
      // Defense in depth: the provider guarantees compatibility, but a swap
      // that fails validation must never enter the search.
      const auto swap = makeSwap(view, fillerId, candidate.masterId, &error);
      if (!swap.has_value()) {
        result.diagnostics.push_back(
            makeDiag(Severity::Warning, "RejectedCandidate",
                     cat("filler ", fillerId, " -> master ",
                         candidate.masterId, ": ", error)));
        continue;
      }
      result.swaps.push_back(*swap);
      ++emitted;
    }
    log.msg("swapgen",
            cat("filler ", fillerId, " (row=",
                view.instance(fillerId)->rowId, " x=",
                view.instance(fillerId)->x, ") -> ", emitted, " swap(s)"));
  }

  log.msg("swapgen",
          cat("window L", window.level, ": ", window.editableFillers.size(),
              " editable filler(s) -> ", result.swaps.size(),
              " swap(s) total"));
  return result;
}


}  // namespace dpl2::fillerRepair
