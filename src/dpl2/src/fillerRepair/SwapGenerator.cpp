// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "SwapGenerator.h"

#include <algorithm>

namespace dpl2::fillerRepair {

SwapGenerationResult generateSwapMoves(
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
      log.msg("movegen",
              cat("filler ", fillerId, " -> 0 candidates, no moves"));
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
      // Defense in depth: the provider guarantees compatibility, but a move
      // that fails validation must never enter the search.
      const auto move = makeSwapMove(view, fillerId, candidate.masterId, &error);
      if (!move.has_value()) {
        result.diagnostics.push_back(
            makeDiag(Severity::Warning, "RejectedCandidate",
                     cat("filler ", fillerId, " -> master ",
                         candidate.masterId, ": ", error)));
        continue;
      }
      result.moves.push_back(*move);
      ++emitted;
    }
    log.msg("movegen",
            cat("filler ", fillerId, " (row=",
                view.instance(fillerId)->rowId, " x=",
                view.instance(fillerId)->x, ") -> ", emitted, " swap move(s)"));
  }

  log.msg("movegen",
          cat("window L", window.level, ": ", window.editableFillers.size(),
              " editable filler(s) -> ", result.moves.size(),
              " swap move(s) total"));
  return result;
}

}  // namespace dpl2::fillerRepair
