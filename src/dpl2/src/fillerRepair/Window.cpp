// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "Window.h"

#include <algorithm>
#include <set>

namespace dpl2::fillerRepair {

namespace {

// Instances of one row overlapping `x`, plus up to `ring` whole instances
// beyond each side. This is the shared "cell ring" primitive for guard
// regions and the unfixable fast check.
std::vector<PlacedInstance> instancesInRing(const PlacementView& view,
                                            RowId rowId,
                                            const XInterval& x,
                                            int ring)
{
  const std::vector<PlacedInstance> all = view.instancesInRow(rowId);
  std::vector<PlacedInstance> result;

  // Index range overlapping x.
  int first = -1;
  int last = -1;
  for (int i = 0; i < static_cast<int>(all.size()); ++i) {
    if (instanceSpan(view, all[i]).overlaps(x)) {
      if (first < 0) {
        first = i;
      }
      last = i;
    }
  }
  if (first < 0) {
    // Nothing overlaps: the ring is the up-to-`ring` nearest instances on
    // each side of x.
    int before = -1;
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
      if (instanceSpan(view, all[i]).xh <= x.xl) {
        before = i;
      }
    }
    for (int i = std::max(0, before - ring + 1); i <= before; ++i) {
      result.push_back(all[i]);
    }
    for (int i = before + 1;
         i < static_cast<int>(all.size()) && i <= before + ring;
         ++i) {
      result.push_back(all[i]);
    }
    return result;
  }
  for (int i = std::max(0, first - ring);
       i <= std::min(static_cast<int>(all.size()) - 1, last + ring);
       ++i) {
    result.push_back(all[i]);
  }
  return result;
}

std::vector<RowId> clampRows(const PlacementView& view, RowId lo, RowId hi)
{
  std::vector<RowId> result;
  for (const RowId row : view.rows()) {
    if (row >= lo && row <= hi) {
      result.push_back(row);
    }
  }
  return result;
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
                         const PlacementView& view,
                         DbCoord ruleDistance,
                         const DebugLog& log)
{
  RepairWindow window;
  window.level = level;

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
    for (const PlacedInstance& inst : view.instancesInRow(rowId)) {
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

  // --- L1 (spec 6.3): rows ±1; per row, extend x outward across contiguous
  // fillers up to the nearest non-filler boundary (fixed cell / row edge);
  // then sweep every filler overlapping the extended x into the editable set,
  // whole instances. (V2.1 #7 dropped L2; #8's adaptive per-K-filler growth is
  // a pending refinement of this sweep.)
  if (level >= 1) {
    const RowId lo = *rowSet.begin() - 1;
    const RowId hi = *rowSet.rbegin() + 1;
    for (const RowId row : clampRows(view, lo, hi)) {
      rowSet.insert(row);
    }
    for (const RowId rowId : rowSet) {
      const std::vector<PlacedInstance> all = view.instancesInRow(rowId);
      DbCoord xl = x.xl;
      for (int i = static_cast<int>(all.size()) - 1; i >= 0; --i) {
        const XInterval span = instanceSpan(view, all[i]);
        if (span.xh > xl) {
          continue;  // not yet left of the current edge
        }
        if (!all[i].isFiller) {
          break;  // fixed boundary stops the extension
        }
        xl = span.xl;
      }
      DbCoord xh = x.xh;
      for (const PlacedInstance& inst : all) {
        const XInterval span = instanceSpan(view, inst);
        if (span.xl < xh) {
          continue;
        }
        if (!inst.isFiller) {
          break;
        }
        xh = span.xh;
      }
      x.xl = std::min(x.xl, xl);
      x.xh = std::max(x.xh, xh);
    }
    for (const RowId rowId : rowSet) {
      for (const PlacedInstance& inst : view.instancesInRow(rowId)) {
        if (inst.isFiller && instanceSpan(view, inst).overlaps(x)) {
          include(inst, /*isBridge=*/false);
        }
      }
    }
  }

  // --- Finalize: deterministic (row, x) order for the editable set.
  window.rows.assign(rowSet.begin(), rowSet.end());
  window.x = x;
  for (const RowId rowId : window.rows) {
    for (const PlacedInstance& inst : view.instancesInRow(rowId)) {
      if (editable.count(inst.id) > 0) {
        window.editableFillers.push_back(inst.id);
      }
    }
  }
  window.bridgeFillers.assign(bridge.begin(), bridge.end());

  // --- guardRegion = expandByCellRing(window, 2): rows ±2 clamped to the
  // design, x widened to cover two placed instances beyond each side on
  // every guard row. Conservative (never smaller than the two-cell ring).
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
          cat("L", level, " rows=[", window.rows.front(), ",",
              window.rows.back(), "] x=", show(window.x), " editable=",
              window.editableFillers.size(), " bridge=",
              window.bridgeFillers.size(), " -> guard=",
              show(window.guardRegion)));
  return window;
}

bool hasFillerNearViolation(const NormalizedViolation& violation,
                            const PlacementView& view)
{
  const RowId lo = violation.rowIds.front() - 2;
  const RowId hi = violation.rowIds.back() + 2;
  for (const RowId rowId : clampRows(view, lo, hi)) {
    for (const PlacedInstance& inst :
         instancesInRing(view, rowId, violation.xRange, 2)) {
      if (inst.isFiller) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace dpl2::fillerRepair
