// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "Ranker.h"

#include <algorithm>
#include <map>
#include <set>

namespace dpl2::fillerRepair {

namespace {

// Neighbor majority VT of a filler: x-adjacent instances in its row plus
// instances in rows +-1 overlapping its span, counted on current masters.
// Tie breaks toward the smaller VT id (deterministic).
VtId neighborMajorityVt(const PlacementView& view, const PlacedInstance& inst)
{
  const XInterval span = instanceSpan(view, inst);
  std::map<VtId, int> votes;

  for (const PlacedInstance& other : view.instancesInRow(inst.rowId)) {
    if (other.id == inst.id) {
      continue;
    }
    const XInterval otherSpan = instanceSpan(view, other);
    if (otherSpan.xh == span.xl || otherSpan.xl == span.xh) {
      ++votes[view.masterInfo(other.masterId)->vt];
    }
  }
  for (const RowId rowId : {inst.rowId - 1, inst.rowId + 1}) {
    for (const PlacedInstance& other : view.instancesInRow(rowId)) {
      if (instanceSpan(view, other).overlaps(span)) {
        ++votes[view.masterInfo(other.masterId)->vt];
      }
    }
  }

  VtId majority = kUnknownVt;
  int best = 0;
  for (const auto& [vt, count] : votes) {  // std::map: ascending vt id
    if (count > best) {
      best = count;
      majority = vt;
    }
  }
  return majority;
}

struct RankKey
{
  bool thirdVt = false;      // demoted band last
  int direct = 0;            // descending
  int bridge = 0;            // descending
  int anchorVote = 0;        // descending
  DbCoord width = 0;         // ascending
  DbCoord x = 0;             // ascending
  RowId row = 0;             // ascending
  MasterId newMaster = 0;    // ascending

  bool operator<(const RankKey& o) const
  {
    if (thirdVt != o.thirdVt) {
      return !thirdVt;
    }
    if (direct != o.direct) {
      return direct > o.direct;
    }
    if (bridge != o.bridge) {
      return bridge > o.bridge;
    }
    if (anchorVote != o.anchorVote) {
      return anchorVote > o.anchorVote;
    }
    if (width != o.width) {
      return width < o.width;
    }
    if (x != o.x) {
      return x < o.x;
    }
    if (row != o.row) {
      return row < o.row;
    }
    return newMaster < o.newMaster;
  }
};

}  // namespace

std::vector<Swap> rankSwaps(std::vector<Swap> swaps,
                            const TargetPlace& anchor,
                            const std::vector<NormalizedViolation>& violations,
                            const RepairWindow& window,
                            const PlacementView& view,
                            const DebugLog& log)
{
  const MasterInfo* anchorMaster = view.masterInfo(anchor.masterId);
  const VtId anchorVt = anchorMaster != nullptr ? anchorMaster->vt : kUnknownVt;

  std::set<InstanceId> direct;
  for (const NormalizedViolation& nv : violations) {
    direct.insert(nv.fillerParticipants.begin(), nv.fillerParticipants.end());
  }
  std::set<InstanceId> bridge(window.bridgeFillers.begin(),
                              window.bridgeFillers.end());

  // Per-filler neighbor majority, computed once.
  std::map<InstanceId, VtId> majority;
  for (const Swap& swap : swaps) {
    if (majority.count(swap.instanceId) == 0) {
      majority[swap.instanceId] =
          neighborMajorityVt(view, *view.instance(swap.instanceId));
    }
  }

  const auto keyOf = [&](const Swap& swap) {
    RankKey key;
    key.thirdVt = swap.newVt != anchorVt
                  && swap.newVt != majority[swap.instanceId];
    key.direct = direct.count(swap.instanceId) > 0 ? 1 : 0;
    key.bridge = bridge.count(swap.instanceId) > 0 ? 1 : 0;
    key.anchorVote = swap.newVt == anchorVt ? 1 : 0;
    key.width = swap.span.length();
    key.x = swap.span.xl;
    key.row = swap.rowId;
    key.newMaster = swap.newMasterId;
    return key;
  };

  std::stable_sort(swaps.begin(),
                   swaps.end(),
                   [&](const Swap& a, const Swap& b) { return keyOf(a) < keyOf(b); });

  if (log.enabled() && !swaps.empty()) {
    int demoted = 0;
    for (const Swap& swap : swaps) {
      demoted += keyOf(swap).thirdVt ? 1 : 0;
    }
    log.msg("rank",
            cat(swaps.size(), " swap(s), anchorVt=", anchorVt, ", demoted(thirdVt)=",
                demoted, "; top: filler ", swaps[0].instanceId, " -> vt",
                swaps[0].newVt, " (master ", swaps[0].newMasterId, ")"));
  }
  return swaps;
}

}  // namespace dpl2::fillerRepair
