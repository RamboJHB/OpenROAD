// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "SubsetSearch.h"

#include "PlannerEngine.h"

#include <algorithm>
#include <functional>

namespace dpl2::fillerRepair {

namespace {

// Full subset space size: prod(1 + |domain_i|) - 1, clamped to `cap + 1` so
// the multiplication cannot overflow.
long long fullSpaceSize(const std::vector<FillerDomain>& ranked, long long cap)
{
  long long size = 1;
  for (const FillerDomain& domain : ranked) {
    size *= 1 + static_cast<long long>(domain.options.size());
    if (size > cap + 1) {
      return cap + 2;  // anything above cap means "does not fit"
    }
  }
  return size - 1;  // exclude the empty subset
}

}  // namespace

EnumerationPlan enumerateOverlays(const std::vector<FillerDomain>& ranked,
                                  const RepairConfig& config,
                                  int budget,
                                  const DebugLog& log)
{
  EnumerationPlan plan;
  if (ranked.empty() || budget <= 0) {
    return plan;
  }

  const int fillerTotal = static_cast<int>(ranked.size());
  size_t optionTotal = 0;
  for (const FillerDomain& domain : ranked) {
    optionTotal += domain.options.size();
  }

  const long long space = fullSpaceSize(ranked, budget);
  plan.complete = space <= budget;

  const int maxSize = plan.complete
                          ? fillerTotal
                          : std::min(config.maxSubsetSize, fillerTotal);
  // Member cap counts FILLERS (V2.1 #9): size-s subsets draw from the first
  // N_s ranked fillers, each contributing its full domain.
  const auto memberCap = [&](int size) -> int {
    if (plan.complete || size == 1) {
      return fillerTotal;
    }
    const int cap = size == 2   ? config.memberCapSize2
                    : size == 3 ? config.memberCapSize3
                                : config.memberCapSize4;
    return std::min(cap, fillerTotal);
  };

  std::vector<int> combo;  // filler indices of the current combination
  Overlay current;         // one option per chosen filler, filler-rank order
  bool budgetHit = false;

  // Cartesian product over the chosen fillers' domains, last filler's option
  // varying fastest, so the all-first-choice assignment (anchor-follow) leads.
  const std::function<void(size_t)> emitProducts = [&](size_t k) {
    if (budgetHit) {
      return;
    }
    if (k == combo.size()) {
      plan.overlays.push_back(current);
      if (static_cast<int>(plan.overlays.size()) >= budget) {
        budgetHit = true;
      }
      return;
    }
    for (const Swap& option : ranked[combo[k]].options) {
      current.push_back(option);
      emitProducts(k + 1);
      current.pop_back();
      if (budgetHit) {
        return;
      }
    }
  };

  // Lexicographic filler combinations of `size` within the rank prefix `cap`.
  const std::function<void(int, int, int)> choose = [&](int size, int from, int cap) {
    if (budgetHit) {
      return;
    }
    if (static_cast<int>(combo.size()) == size) {
      emitProducts(0);
      return;
    }
    for (int i = from; i < cap; ++i) {
      combo.push_back(i);
      choose(size, i + 1, cap);
      combo.pop_back();
      if (budgetHit) {
        return;
      }
    }
  };

  for (int size = 1; size <= maxSize && !budgetHit; ++size) {
    choose(size, 0, memberCap(size));
  }
  // Reaching the budget on the final element is still a complete search.
  // Derive completeness from what was actually emitted so space == budget
  // cannot be mislabeled as truncated.
  plan.complete = space <= budget
                  && static_cast<long long>(plan.overlays.size()) == space;

  log.msg("enumerate",
          cat(fillerTotal, " filler domain(s), ", optionTotal,
              " option(s), space=", space,
              plan.complete ? " (complete)" : " (truncated)", " -> ",
              plan.overlays.size(), " overlay candidate(s), maxSize=", maxSize));
  return plan;
}

}  // namespace dpl2::fillerRepair
