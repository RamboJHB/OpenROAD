// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "PlacementView.h"

#include "Log.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

namespace dpl2::fillerRepair {

namespace {

const char* kindName(CoverageIssueKind kind)
{
  switch (kind) {
    case CoverageIssueKind::Gap:
      return "Gap";
    case CoverageIssueKind::Overlap:
      return "Overlap";
    case CoverageIssueKind::OffGrid:
      return "OffGrid";
    case CoverageIssueKind::IllegalOccupant:
      return "IllegalOccupant";
  }
  return "?";
}

}  // namespace

MasterCandidateResult PlacementView::getUsableMasterCandidates(
    const MasterCandidateRequest& request) const
{
  MasterCandidateResult result;
  const PlacedInstance* inst = instance(request.fillerInstanceId);
  if (inst == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "UnknownInstance",
        cat("instance ", request.fillerInstanceId, " not found")));
    return result;
  }
  if (!inst->isFiller) {
    result.diagnostics.push_back(makeDiag(
        Severity::Warning, "NotAFiller",
        cat("instance ", request.fillerInstanceId, " is not a filler")));
    return result;
  }
  const MasterInfo* current = masterInfo(inst->masterId);
  if (current == nullptr || !current->isFiller) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "UnknownMaster",
        cat("invalid current filler master for instance ", request.fillerInstanceId)));
    return result;
  }

  if (current->vt == kUnknownVt) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "UnknownCurrentVt",
        cat("current filler master ", inst->masterId,
            " has no checker VT")));
    return result;
  }

  std::vector<MasterId> configured = fillerMasterIds();
  std::sort(configured.begin(), configured.end());
  configured.erase(std::unique(configured.begin(), configured.end()), configured.end());
  for (const MasterId id : configured) {
    const MasterInfo* candidate = masterInfo(id);
    if (candidate == nullptr) {
      result.diagnostics.push_back(makeDiag(
          Severity::Warning, "UnknownConfiguredMaster",
          cat("configured filler master ", id, " is not in the view")));
      continue;
    }
    // Same size, different (known) VT family, and the same R0-frame band
    // polarity layout: a swap keeps position/orientation, so a candidate
    // whose bottom band has the opposite polarity would land every band on
    // the wrong track -- the checker rejects such overlays unconditionally,
    // offering them only burns checker calls.
    if (id != inst->masterId && candidate->isFiller
        && candidate->vt != kUnknownVt && candidate->vt != current->vt
        && candidate->width == current->width
        && candidate->height == current->height
        && candidate->bottomBandPolarity == current->bottomBandPolarity) {
      result.candidates.push_back(MasterCandidate{id});
    }
  }
  if (result.candidates.empty()) {
    result.diagnostics.push_back(makeDiag(
        Severity::Info, "NoUsableMaster",
        cat("no configured same-size VT replacement for instance ",
            request.fillerInstanceId)));
  }
  return result;
}

SiteCoverageResult PlacementView::checkSiteCoverage(const DebugLog& log) const
{
  SiteCoverageResult result;
  const DbCoord site_width = std::max<DbCoord>(this->siteWidth(), 1);

  const auto addIssue = [&](CoverageIssueKind kind,
                            RowId rowId,
                            DbCoord xLo,
                            DbCoord xHi,
                            std::vector<InstanceId> instances) {
    CoverageIssue issue;
    issue.kind = kind;
    issue.rowId = rowId;
    issue.xLo = xLo;
    issue.xHi = xHi;
    issue.siteCount = static_cast<int>((xHi - xLo) / site_width);
    issue.instances = std::move(instances);
    result.issues.push_back(std::move(issue));
  };

  for (const RowId rowId : rows()) {
    const XInterval legal = rowLegalSpan(rowId);
    const std::vector<PlacedInstance> instances = instancesInRow(rowId);
    std::vector<XInterval> clippedSpans;
    clippedSpans.reserve(instances.size());
    std::vector<DbCoord> cuts{legal.xl, legal.xh};

    for (const PlacedInstance& inst : instances) {
      const XInterval span = instanceSpan(*this, inst);

      if ((span.xl - legal.xl) % site_width != 0) {
        addIssue(CoverageIssueKind::OffGrid, rowId, span.xl, span.xh, {inst.id});
      }
      if (span.xl < legal.xl || span.xh > legal.xh) {
        addIssue(CoverageIssueKind::IllegalOccupant, rowId, span.xl, span.xh,
                 {inst.id});
      }

      const XInterval clipped{std::max(span.xl, legal.xl),
                              std::min(span.xh, legal.xh)};
      clippedSpans.push_back(clipped);
      if (!clipped.empty()) {
        cuts.push_back(clipped.xl);
        cuts.push_back(clipped.xh);
      }
    }

    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    // Sweep instead of scanning every instance per segment (dense rows made
    // that O(N^2)): every span boundary is a cut, so a span overlaps segment
    // [cuts[i], cuts[i+1]) iff span.xl <= cuts[i] < span.xh -- maintain that
    // active set incrementally. std::set keeps ids sorted for the issues.
    std::vector<std::pair<DbCoord, InstanceId>> starts;
    std::vector<std::pair<DbCoord, InstanceId>> ends;
    starts.reserve(clippedSpans.size());
    ends.reserve(clippedSpans.size());
    for (size_t j = 0; j < clippedSpans.size(); ++j) {
      if (!clippedSpans[j].empty()) {
        starts.emplace_back(clippedSpans[j].xl, instances[j].id);
        ends.emplace_back(clippedSpans[j].xh, instances[j].id);
      }
    }
    std::sort(starts.begin(), starts.end());
    std::sort(ends.begin(), ends.end());

    std::set<InstanceId> active;
    size_t nextStart = 0;
    size_t nextEnd = 0;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
      const XInterval segment{cuts[i], cuts[i + 1]};
      if (segment.empty()) {
        continue;
      }
      while (nextEnd < ends.size() && ends[nextEnd].first <= segment.xl) {
        active.erase(ends[nextEnd].second);
        ++nextEnd;
      }
      while (nextStart < starts.size() && starts[nextStart].first <= segment.xl) {
        active.insert(starts[nextStart].second);
        ++nextStart;
      }
      if (active.empty()) {
        addIssue(CoverageIssueKind::Gap, rowId, segment.xl, segment.xh, {});
      } else if (active.size() > 1) {
        addIssue(CoverageIssueKind::Overlap,
                 rowId,
                 segment.xl,
                 segment.xh,
                 {active.begin(), active.end()});
      }
    }
  }

  std::sort(result.issues.begin(),
            result.issues.end(),
            [](const CoverageIssue& a, const CoverageIssue& b) {
              return std::tie(a.rowId, a.xLo, a.kind, a.xHi, a.instances)
                     < std::tie(b.rowId, b.xLo, b.kind, b.xHi, b.instances);
            });

  std::vector<CoverageIssue> merged;
  for (CoverageIssue& issue : result.issues) {
    if (!merged.empty() && merged.back().kind == issue.kind
        && merged.back().rowId == issue.rowId
        && merged.back().xHi == issue.xLo
        && merged.back().instances == issue.instances) {
      merged.back().xHi = issue.xHi;
      merged.back().siteCount
          = static_cast<int>((merged.back().xHi - merged.back().xLo) / site_width);
    } else {
      merged.push_back(std::move(issue));
    }
  }
  result.issues = std::move(merged);

  result.isFullUtility = result.issues.empty();
  if (result.isFullUtility) {
    log.msg("precheck", "all rows fully covered -> 100% utility OK");
  } else {
    for (const CoverageIssue& issue : result.issues) {
      log.msg("precheck",
              cat(kindName(issue.kind), " row=", issue.rowId, " x=",
                  show(XInterval{issue.xLo, issue.xHi}), " sites=",
                  issue.siteCount, " -> precondition failure"));
      result.diagnostics.push_back(makeDiag(
          Severity::Error,
          "NonFullUtility",
          cat(kindName(issue.kind), " row=", issue.rowId, " x=",
              show(XInterval{issue.xLo, issue.xHi}), " sites=", issue.siteCount)));
    }
  }
  return result;
}

}  // namespace dpl2::fillerRepair
