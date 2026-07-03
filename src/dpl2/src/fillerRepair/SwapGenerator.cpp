// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "SwapGenerator.h"

#include <algorithm>

namespace dpl2::fillerRepair {

SwapGenerationResult generateSwaps(
    const RepairWindow& window,
    const PlacementView& view,
    const FillerMasterCandidateProvider& provider,
    const DebugLog& log)
{
  SwapGenerationResult result;

  for (const InstanceId fillerId : window.editableFillers) {
    MasterCandidateResult candidates =
        provider.getUsableMasterCandidates({fillerId});
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
