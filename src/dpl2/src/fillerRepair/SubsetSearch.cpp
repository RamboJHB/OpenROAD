// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "SubsetSearch.h"

#include <algorithm>
#include <functional>
#include <map>

namespace dpl2::fillerRepair {

namespace {

// Full subset space size: prod(1 + candidates per filler) - 1, clamped to
// `cap + 1` so the multiplication cannot overflow.
long long fullSpaceSize(const std::vector<Swap>& ranked, long long cap)
{
  std::map<InstanceId, long long> perFiller;
  for (const Swap& swap : ranked) {
    ++perFiller[swap.instanceId];
  }
  long long size = 1;
  for (const auto& [id, count] : perFiller) {
    size *= (1 + count);
    if (size > cap + 1) {
      return cap + 2;  // anything above cap means "does not fit"
    }
  }
  return size - 1;  // exclude the empty subset
}

}  // namespace

EnumerationPlan enumerateOverlays(const std::vector<Swap>& ranked,
                                  const RepairConfig& config,
                                  int budget,
                                  const DebugLog& log)
{
  EnumerationPlan plan;
  if (ranked.empty() || budget <= 0) {
    return plan;
  }

  std::map<InstanceId, int> fillerCount;
  for (const Swap& swap : ranked) {
    ++fillerCount[swap.instanceId];
  }
  const int fillerTotal = static_cast<int>(fillerCount.size());

  const long long space = fullSpaceSize(ranked, budget);
  plan.complete = space <= budget;

  const int maxSize = plan.complete
                          ? fillerTotal
                          : std::min(config.maxSubsetSize, fillerTotal);
  const auto memberCap = [&](int size) -> int {
    if (plan.complete || size == 1) {
      return static_cast<int>(ranked.size());
    }
    const int cap = size == 2   ? config.memberCapSize2
                    : size == 3 ? config.memberCapSize3
                                : config.memberCapSize4;
    return std::min(cap, static_cast<int>(ranked.size()));
  };

  // Lexicographic combination enumeration over rank indices; a combination
  // is skipped when two members swap the same filler.
  std::vector<int> combo;
  bool budgetHit = false;
  const std::function<void(int, int, int)> emit = [&](int size, int from, int cap) {
    if (budgetHit) {
      return;
    }
    if (static_cast<int>(combo.size()) == size) {
      Overlay overlay;
      overlay.reserve(size);
      for (const int idx : combo) {
        overlay.push_back(ranked[idx]);
      }
      plan.overlays.push_back(std::move(overlay));
      if (static_cast<int>(plan.overlays.size()) >= budget) {
        budgetHit = true;
      }
      return;
    }
    for (int i = from; i < cap; ++i) {
      bool dup = false;
      for (const int idx : combo) {
        dup |= ranked[idx].instanceId == ranked[i].instanceId;
      }
      if (dup) {
        continue;
      }
      combo.push_back(i);
      emit(size, i + 1, cap);
      combo.pop_back();
      if (budgetHit) {
        return;
      }
    }
  };

  for (int size = 1; size <= maxSize && !budgetHit; ++size) {
    emit(size, 0, memberCap(size));
  }
  if (budgetHit) {
    plan.complete = false;  // truncated by budget after all
  }

  log.msg("enumerate",
          cat(ranked.size(), " swap(s) over ", fillerTotal, " filler(s), space=",
              space, plan.complete ? " (complete)" : " (truncated)",
              " -> ", plan.overlays.size(), " overlay candidate(s), maxSize=",
              maxSize));
  return plan;
}

}  // namespace dpl2::fillerRepair
