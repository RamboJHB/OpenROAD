// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "Window.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace dpl2::fillerRepair {

namespace {

// instancesInRow is x-sorted and, on the planner path (which only runs after a
// clean gap/overlap snapshot), non-overlapping -- so each instance's right edge
// is non-decreasing. That lets every window scan binary-search to the relevant
// x-range instead of walking the whole row, which matters on 100%-utilization
// designs where a row holds thousands of instances but only a sparse minority
// are editable fillers near the target.

// First index whose right edge lies strictly right of `bound` (i.e. the first
// instance not entirely to the left of it).
int firstRightEdgeAfter(const PlannerDataSource& view,
                        const std::vector<PlacedInstance>& all,
                        DbCoord bound)
{
  int lo = 0;
  for (int hi = static_cast<int>(all.size()); lo < hi;) {
    const int mid = lo + (hi - lo) / 2;
    if (instanceSpan(view, all[mid]).xh > bound) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

// First index whose left edge is at or right of `bound`.
int firstStartAtOrAfter(const std::vector<PlacedInstance>& all, DbCoord bound)
{
  int lo = 0;
  for (int hi = static_cast<int>(all.size()); lo < hi;) {
    const int mid = lo + (hi - lo) / 2;
    if (all[mid].x >= bound) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

// Instances of one row overlapping `x`, plus up to `ring` whole instances
// beyond each side. This is the shared "cell ring" primitive for guard
// regions and the unfixable fast check. The instances overlapping x are the
// contiguous index range [lo, hi): lo = first whose right edge exceeds x.xl,
// hi = first that starts at/after x.xh. When nothing overlaps, lo == hi at the
// gap and the +/- ring extension yields exactly the nearest instances on each
// side, matching the previous full-row-scan behavior.
std::vector<PlacedInstance> instancesInRing(const PlannerDataSource& view,
                                            RowId rowId,
                                            const XInterval& x,
                                            int ring)
{
  const std::vector<PlacedInstance>& all = view.instancesInRow(rowId);
  std::vector<PlacedInstance> result;
  const int n = static_cast<int>(all.size());
  if (n == 0) {
    return result;
  }

  const int lo = firstRightEdgeAfter(view, all, x.xl);
  const int hi = firstStartAtOrAfter(all, x.xh);
  const int from = std::max(0, lo - ring);
  const int to = std::min(n, hi + ring);
  for (int i = from; i < to; ++i) {
    result.push_back(all[i]);
  }
  return result;
}

std::vector<RowId> clampRows(const PlannerDataSource& view, RowId lo, RowId hi)
{
  std::vector<RowId> result;
  for (const RowId row : view.rows()) {
    if (row >= lo && row <= hi) {
      result.push_back(row);
    }
  }
  return result;
}

RepairWindow finalizeWindow(int level,
                            const std::set<RowId>& rowSet,
                            const std::set<InstanceId>& editable,
                            const std::set<InstanceId>& bridge,
                            XInterval x,
                            const PlannerDataSource& view,
                            const DebugLog& log)
{
  RepairWindow window;
  window.level = level;
  window.rows.assign(rowSet.begin(), rowSet.end());
  window.x = x;
  // editableFillers is the move-generation universe, ordered by (row, x, id).
  // Look the members up directly rather than scanning whole rows for them: on a
  // packed row the editable set is a sparse minority of the instances present.
  std::vector<const PlacedInstance*> editableInsts;
  editableInsts.reserve(editable.size());
  for (const InstanceId id : editable) {
    if (const PlacedInstance* inst = view.instance(id)) {
      editableInsts.push_back(inst);
    }
  }
  std::sort(editableInsts.begin(), editableInsts.end(),
            [](const PlacedInstance* a, const PlacedInstance* b) {
              return std::tie(a->rowId, a->x, a->id)
                     < std::tie(b->rowId, b->x, b->id);
            });
  for (const PlacedInstance* inst : editableInsts) {
    window.editableFillers.push_back(inst->id);
  }
  window.bridgeFillers.assign(bridge.begin(), bridge.end());

  const std::vector<RowId> guardRows =
      clampRows(view, window.rows.front() - 2, window.rows.back() + 2);
  XInterval guardX = x;
  for (const RowId rowId : guardRows) {
    for (const PlacedInstance& inst : instancesInRing(view, rowId, x, 2)) {
      const XInterval span = instanceSpan(view, inst);
      guardX.xl = std::min(guardX.xl, span.xl);
      guardX.xh = std::max(guardX.xh, span.xh);
    }
  }
  window.guardRegion = Region{guardX, guardRows.front(), guardRows.back()};

  log.msg("window",
          cat(level == 0 ? "L0" : cat("adaptive-L1 step ", level),
              " rows=[", window.rows.front(), ",", window.rows.back(),
              "] x=", show(window.x), " editable=",
              window.editableFillers.size(), " bridge=",
              window.bridgeFillers.size(), " -> guard=",
              show(window.guardRegion)));
  return window;
}

}  // namespace

bool RepairWindow::containsEditable(InstanceId id) const
{
  return std::find(editableFillers.begin(), editableFillers.end(), id)
         != editableFillers.end();
}

RepairWindow buildWindow(int level,
                         const TargetPlace& anchor,
                         const std::vector<NormalizedViolation>& violations,
                         const PlannerDataSource& view,
                         DbCoord ruleDistance,
                         const DebugLog& log)
{
  (void) level;  // adaptive-L1 growth is stateful; this builder always makes L0.

  const MasterInfo* anchorMaster = view.masterInfo(anchor.masterId);
  const DbCoord anchorWidth = anchorMaster != nullptr ? anchorMaster->width : 0;
  const XInterval anchorSpan{anchor.x, anchor.x + anchorWidth};

  std::set<RowId> rowSet{anchor.rowId};
  std::set<InstanceId> editable;
  std::set<InstanceId> bridge;
  XInterval x = anchorSpan;

  const auto include = [&](const PlacedInstance& inst, bool isBridge) {
    editable.insert(inst.id);
    if (isBridge) {
      bridge.insert(inst.id);
    }
    rowSet.insert(inst.rowId);
    const XInterval span = instanceSpan(view, inst);
    x.xl = std::min(x.xl, span.xl);
    x.xh = std::max(x.xh, span.xh);
  };

  // --- L0 seed set (spec 6.3): violation filler participants, plus the
  // violation footprints/rows for the x range.
  for (const NormalizedViolation& nv : violations) {
    rowSet.insert(nv.rowIds.begin(), nv.rowIds.end());
    x.xl = std::min(x.xl, nv.xRange.xl);
    x.xh = std::max(x.xh, nv.xRange.xh);
    for (const InstanceId id : nv.fillerParticipants) {
      const PlacedInstance* inst = view.instance(id);
      if (inst != nullptr && inst->isFiller) {
        include(*inst, /*isBridge=*/false);
      }
    }
  }

  // --- Bridge fillers (default-mandatory, spec 6.3): fillers touching the
  // anchor in its row, and fillers in rows ±1 overlapping the anchor span
  // widened by one rule distance. They join even outside the footprint.
  const XInterval bridgeSpan{anchorSpan.xl - ruleDistance,
                             anchorSpan.xh + ruleDistance};
  for (const RowId rowId : clampRows(view, anchor.rowId - 1, anchor.rowId + 1)) {
    // Only instances overlapping bridgeSpan can qualify: the row==anchor
    // touch cases (span touches an anchor edge) and the coupled-row overlap
    // case both lie inside [anchorSpan +/- ruleDistance]. Binary-search that
    // band instead of walking the whole row.
    const std::vector<PlacedInstance>& all = view.instancesInRow(rowId);
    const int lo = firstRightEdgeAfter(view, all, bridgeSpan.xl);
    const int hi = firstStartAtOrAfter(all, bridgeSpan.xh);
    for (int i = lo; i < hi; ++i) {
      const PlacedInstance& inst = all[i];
      if (!inst.isFiller) {
        continue;
      }
      const XInterval span = instanceSpan(view, inst);
      const bool touchesAnchor = rowId == anchor.rowId
                                 && (span.xh == anchorSpan.xl
                                     || span.xl == anchorSpan.xh);
      const bool underOrOverAnchor = rowId != anchor.rowId
                                     && span.overlaps(bridgeSpan);
      if (touchesAnchor || underOrOverAnchor) {
        include(inst, /*isBridge=*/true);
      }
    }
  }

  return finalizeWindow(0, rowSet, editable, bridge, x, view, log);
}

RepairWindow expandWindowAdaptive(const RepairWindow& current,
                                  const TargetPlace& anchor,
                                  const std::vector<Violation>& blocking,
                                  const PlannerDataSource& view,
                                  int fillersPerRow,
                                  const DebugLog& log)
{
  std::set<RowId> rowSet(current.rows.begin(), current.rows.end());
  std::set<InstanceId> editable(current.editableFillers.begin(),
                                current.editableFillers.end());
  std::set<InstanceId> bridge(current.bridgeFillers.begin(),
                              current.bridgeFillers.end());
  XInterval x = current.x;

  bool growLeft = false;
  bool growRight = false;
  for (const Violation& violation : blocking) {
    for (const RowId rowId : violation.rowIds) {
      for (const RowId coupled : clampRows(view, rowId - 1, rowId + 1)) {
        rowSet.insert(coupled);
      }
    }
    if (violation.xWindow.xl <= current.x.xl) {
      growLeft = true;
    }
    if (violation.xWindow.xh >= current.x.xh) {
      growRight = true;
    }
    if (violation.xWindow.xl > current.x.xl
        && violation.xWindow.xh < current.x.xh) {
      const DbCoord leftDistance = violation.xWindow.xl - current.x.xl;
      const DbCoord rightDistance = current.x.xh - violation.xWindow.xh;
      growLeft |= leftDistance <= rightDistance;
      growRight |= rightDistance <= leftDistance;
    }
  }
  if (!growLeft && !growRight) {
    growLeft = true;
    growRight = true;
  }
  log.msg("window",
          cat("expand from L", current.level, ": blocking=",
              blocking.size(), " direction=",
              growLeft ? "left" : "",
              growLeft && growRight ? "+" : "",
              growRight ? "right" : "", " stepPerRow=",
              std::max(1, fillersPerRow)));

  const MasterInfo* anchorMaster = view.masterInfo(anchor.masterId);
  const XInterval anchorSpan{
      anchor.x,
      anchor.x + (anchorMaster != nullptr ? anchorMaster->width : 0)};
  const int step = std::max(1, fillersPerRow);
  int addedLeftTotal = 0;
  int addedRightTotal = 0;

  const auto addOnSides = [&](bool addLeft, bool addRight) {
    const int before = addedLeftTotal + addedRightTotal;
    for (const RowId rowId : rowSet) {
      const std::vector<PlacedInstance>& all = view.instancesInRow(rowId);
      DbCoord leftFrontier = current.x.xl;
      DbCoord rightFrontier = current.x.xh;
      bool hasSeed = false;
      if (rowId == anchor.rowId) {
        leftFrontier = anchorSpan.xl;
        rightFrontier = anchorSpan.xh;
        hasSeed = true;
      }
      for (const InstanceId id : current.editableFillers) {
        const PlacedInstance* inst = view.instance(id);
        if (inst == nullptr || inst->rowId != rowId) {
          continue;
        }
        const XInterval span = instanceSpan(view, *inst);
        leftFrontier = hasSeed ? std::min(leftFrontier, span.xl) : span.xl;
        rightFrontier = hasSeed ? std::max(rightFrontier, span.xh) : span.xh;
        hasSeed = true;
      }
      if (!hasSeed) {
        leftFrontier = current.x.xl;
        rightFrontier = current.x.xh;
      }

      if (addLeft) {
        int added = 0;
        // Walk left from the instance just left of the frontier (everything at
        // or right of it has span.xh > leftFrontier and was skipped before).
        for (int i = firstRightEdgeAfter(view, all, leftFrontier) - 1;
             i >= 0 && added < step;
             --i) {
          const XInterval span = instanceSpan(view, all[i]);
          if (span.xh > leftFrontier || editable.count(all[i].id) > 0) {
            continue;
          }
          if (!all[i].isFiller || span.xh < leftFrontier) {
            break;
          }
          editable.insert(all[i].id);
          leftFrontier = span.xl;
          x.xl = std::min(x.xl, span.xl);
          ++added;
          ++addedLeftTotal;
        }
      }
      if (addRight) {
        int added = 0;
        // Walk right from the first instance at or right of the frontier
        // (everything before it has span.xl < rightFrontier and was skipped).
        for (int i = firstStartAtOrAfter(all, rightFrontier);
             i < static_cast<int>(all.size()) && added < step;
             ++i) {
          const XInterval span = instanceSpan(view, all[i]);
          if (span.xl < rightFrontier || editable.count(all[i].id) > 0) {
            continue;
          }
          if (!all[i].isFiller || span.xl > rightFrontier) {
            break;
          }
          editable.insert(all[i].id);
          rightFrontier = span.xh;
          x.xh = std::max(x.xh, span.xh);
          ++added;
          ++addedRightTotal;
        }
      }
    }
    return addedLeftTotal + addedRightTotal > before;
  };

  // Residual violations choose the primary direction. Because multi-swap
  // legality is non-monotone, that direction can be locally blocked while a
  // required filler is immediately available on the other side. In that
  // case, try the opposite side once before declaring the window exhausted.
  const bool primaryAdded = addOnSides(growLeft, growRight);
  if (!primaryAdded && growLeft != growRight) {
    log.msg("window",
            cat("primary adaptive direction ", growLeft ? "left" : "right",
                " added no filler -> try ", growLeft ? "right" : "left"));
    addOnSides(growRight, growLeft);
  }

  log.msg("window",
          cat("adaptive step L", current.level, " -> L",
              current.level + 1, " addedLeft=", addedLeftTotal,
              " addedRight=", addedRightTotal, " editableTotal=",
              editable.size()));

  return finalizeWindow(current.level + 1,
                        rowSet,
                        editable,
                        bridge,
                        x,
                        view,
                        log);
}

}  // namespace dpl2::fillerRepair
