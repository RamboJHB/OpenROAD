// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "Ranker.h"

#include <algorithm>
#include <map>
#include <set>

namespace dpl2::fillerRepair {

namespace {

// Neighbor majority VT of a filler, counted PER BAND SLOT (spec 6.6: "per
// band-slot, not per cell"). A master's VT family is uniform across its
// bands (checker: master_implant_family_mismatch), so the band structure
// shows up as WEIGHT: an x-adjacent same-row neighbor faces the filler on
// BOTH half-row bands (two band votes), while a row +-1 neighbor interacts
// only through the single facing band pair across the row boundary
// (checker: activeKindByBoundary) -- one band vote.
// Tie breaks toward the smaller VT id (deterministic).
VtId neighborMajorityVt(const PlannerDataSource& view, const PlacedInstance& inst)
{
  const XInterval span = instanceSpan(view, inst);
  std::map<VtId, int> votes;

  // Defensive: an instance with a missing master must not crash the vote
  // (upstream validation makes it unreachable in runtime, but the ranker
  // must not rely on two layers above it).
  for (const PlacedInstance& other : view.instancesInRow(inst.rowId)) {
    if (other.id == inst.id) {
      continue;
    }
    const MasterInfo* master = view.masterInfo(other.masterId);
    if (master == nullptr) {
      continue;
    }
    const XInterval otherSpan = instanceSpan(view, other);
    if (otherSpan.xh == span.xl || otherSpan.xl == span.xh) {
      votes[master->vt] += 2;
    }
  }
  for (const RowId rowId : {inst.rowId - 1, inst.rowId + 1}) {
    for (const PlacedInstance& other : view.instancesInRow(rowId)) {
      const MasterInfo* master = view.masterInfo(other.masterId);
      if (master == nullptr) {
        continue;
      }
      if (instanceSpan(view, other).overlaps(span)) {
        votes[master->vt] += 1;
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
    const PlannerDataSource& view,
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

  if (log.enabled()) {
    log.msg("rank",
            cat(ranked.size(), " filler domain(s) over ", swaps.size(),
                " swap(s), anchorVt=", anchorVt,
                ", demoted(thirdVt)=", demoted));
    // Print the actual domain order consumed by SubsetSearcher. Each line
    // contains the filler-level key followed by the complete, still-reachable
    // master domain (anchor/majority choices first, third VT last).
    for (size_t rank = 0; rank < ranked.size(); ++rank) {
      const FillerDomain& domain = ranked[rank];
      const FillerKey key = fillerKey(domain);
      std::string options;
      for (const Swap& swap : domain.options) {
        options += cat(options.empty() ? "" : ",", "m", swap.newMasterId,
                       ":vt", swap.newVt);
      }
      log.msg("rank",
              cat("#", rank, " filler=", domain.instanceId,
                  " key{direct=", key.direct, " bridge=", key.bridge,
                  " width=", key.width, " x=", key.x, " row=", key.row,
                  "} domain=[", options, ']'));
    }
  }
  return ranked;
}

}  // namespace dpl2::fillerRepair
