// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "Move.h"

#include <algorithm>
#include <utility>

#include "Log.h"

namespace dpl2::fillerRepair {

std::optional<SwapMove> makeSwapMove(const PlacementView& view,
                                     InstanceId instanceId,
                                     MasterId newMasterId,
                                     std::string* error)
{
  const auto fail = [error](std::string why) -> std::optional<SwapMove> {
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

  SwapMove move;
  move.instanceId = instanceId;
  move.oldMasterId = inst->masterId;
  move.newMasterId = newMasterId;
  move.rowId = inst->rowId;
  move.span = XInterval{inst->x, inst->x + oldMaster->width};
  move.oldVt = oldMaster->vt;
  move.newVt = newMaster->vt;
  return move;
}

bool overlayHasConflict(const Overlay& overlay)
{
  for (size_t i = 0; i < overlay.size(); ++i) {
    for (size_t j = i + 1; j < overlay.size(); ++j) {
      if (overlay[i].instanceId == overlay[j].instanceId) {
        return true;
      }
    }
  }
  return false;
}

std::string canonicalKey(const Overlay& overlay)
{
  std::vector<std::pair<InstanceId, MasterId>> pairs;
  pairs.reserve(overlay.size());
  for (const SwapMove& move : overlay) {
    pairs.emplace_back(move.instanceId, move.newMasterId);
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
  for (const SwapMove& move : overlay) {
    changes.push_back(move.change());
  }
  std::sort(changes.begin(),
            changes.end(),
            [](const FillerChange& a, const FillerChange& b) {
              return a.instanceId < b.instanceId;
            });
  return changes;
}

}  // namespace dpl2::fillerRepair
