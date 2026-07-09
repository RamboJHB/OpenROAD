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

// Filler-level ordering key (V2.1 #9): which fillers the searcher combines
// first. VT-choice features (anchor vote, third-VT demotion) do NOT belong
// here -- they order options WITHIN a domain, below.
struct FillerKey
{
  int direct = 0;      // descending
  int bridge = 0;      // descending
  DbCoord width = 0;   // ascending
  DbCoord x = 0;       // ascending
  RowId row = 0;       // ascending

  bool operator<(const FillerKey& o) const
  {
    if (direct != o.direct) {
      return direct > o.direct;
    }
    if (bridge != o.bridge) {
      return bridge > o.bridge;
    }
    if (width != o.width) {
      return width < o.width;
    }
    if (x != o.x) {
      return x < o.x;
    }
    return row < o.row;
  }
};

}  // namespace

std::vector<FillerDomain> rankFillers(
    const std::vector<Swap>& swaps,
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

  // Group swaps into per-filler domains, keyed for deterministic grouping.
  std::map<InstanceId, FillerDomain> byFiller;
  for (const Swap& swap : swaps) {
    FillerDomain& domain = byFiller[swap.instanceId];
    domain.instanceId = swap.instanceId;
    domain.options.push_back(swap);
  }

  std::vector<FillerDomain> ranked;
  ranked.reserve(byFiller.size());
  int demoted = 0;
  for (auto& [id, domain] : byFiller) {
    // Domain order: anchor's new VT -> neighbor majority -> stable master id;
    // the third VT (neither) last -- demoted within THIS domain only, so it
    // stays reachable in every subset the filler joins (V2.1 #9).
    const VtId majorityVt = neighborMajorityVt(view, *view.instance(id));
    const auto optionKey = [&](const Swap& s) {
      const bool third = s.newVt != anchorVt && s.newVt != majorityVt;
      const int anchorVote = s.newVt == anchorVt ? 0 : 1;
      const int majorityVote = s.newVt == majorityVt ? 0 : 1;
      return std::make_tuple(third, anchorVote, majorityVote, s.newMasterId);
    };
    std::stable_sort(domain.options.begin(), domain.options.end(),
                     [&](const Swap& a, const Swap& b) {
                       return optionKey(a) < optionKey(b);
                     });
    for (const Swap& s : domain.options) {
      demoted += std::get<0>(optionKey(s)) ? 1 : 0;
    }
    ranked.push_back(std::move(domain));
  }

  const auto fillerKey = [&](const FillerDomain& domain) {
    const Swap& any = domain.options.front();  // geometry is per-filler
    FillerKey key;
    key.direct = direct.count(domain.instanceId) > 0 ? 1 : 0;
    key.bridge = bridge.count(domain.instanceId) > 0 ? 1 : 0;
    key.width = any.span.length();
    key.x = any.span.xl;
    key.row = any.rowId;
    return key;
  };
  std::stable_sort(ranked.begin(), ranked.end(),
                   [&](const FillerDomain& a, const FillerDomain& b) {
                     return fillerKey(a) < fillerKey(b);
                   });

  if (log.enabled() && !ranked.empty()) {
    std::string top;
    for (const Swap& s : ranked[0].options) {
      top += cat(top.empty() ? "" : ",", "vt", s.newVt);
    }
    log.msg("rank",
            cat(ranked.size(), " filler domain(s) over ", swaps.size(),
                " swap(s), anchorVt=", anchorVt, ", demoted(thirdVt)=", demoted,
                "; top: filler ", ranked[0].instanceId, " domain=[", top, "]"));
  }
  return ranked;
}

}  // namespace dpl2::fillerRepair
