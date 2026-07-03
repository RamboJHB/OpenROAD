// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "Swap.h"

#include <algorithm>
#include <utility>

#include "Log.h"

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

}  // namespace dpl2::fillerRepair
