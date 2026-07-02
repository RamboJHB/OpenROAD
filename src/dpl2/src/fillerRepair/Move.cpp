// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "Move.h"

#include <algorithm>

#include "Log.h"

namespace dpl2::fillerRepair {

std::optional<SwapMove> makeSwapMove(const PlacementView& view,
                                     InstanceId instanceId,
                                     MasterId newMasterId,
                                     std::string* error)
{
  const auto fail = [error](std::string why) -> std::optional<SwapMove> {
    if (error != nullptr) {
      *error = std::move(why);
    }
    return std::nullopt;
  };

  const PlacedInstance* inst = view.instance(instanceId);
  if (inst == nullptr) {
    return fail(cat("instance ", instanceId, " not found"));
  }
  if (!inst->isFiller) {
    return fail(cat("instance ", instanceId, " is not a filler"));
  }
  const MasterInfo* oldMaster = view.masterInfo(inst->masterId);
  const MasterInfo* newMaster = view.masterInfo(newMasterId);
  if (oldMaster == nullptr || newMaster == nullptr) {
    return fail(cat("unknown master (old=", inst->masterId, " new=", newMasterId, ")"));
  }
  if (!newMaster->isFiller) {
    return fail(cat("master ", newMasterId, " is not a filler master"));
  }
  if (newMasterId == inst->masterId) {
    return fail(cat("master ", newMasterId, " is the current master"));
  }
  if (newMaster->width != oldMaster->width || newMaster->height != oldMaster->height) {
    return fail(cat("size mismatch: master ", newMasterId, " w=", newMaster->width,
                    " h=", newMaster->height, " vs current w=", oldMaster->width,
                    " h=", oldMaster->height));
  }

  SwapMove move;
  move.instanceId = instanceId;
  move.oldMasterId = inst->masterId;
  move.newMasterId = newMasterId;
  move.rowId = inst->rowId;
  move.span = XInterval{inst->x, inst->x + oldMaster->width};
  move.oldVt = oldMaster->vt;
  move.newVt = newMaster->vt;
  return move;
}

bool movesConflict(const FillerRewrite& a, const FillerRewrite& b)
{
  if (!a.span.overlaps(b.span)) {
    return false;
  }
  // rowIds are tiny vectors (V1: size 1); linear scan beats set machinery.
  for (const RowId ra : a.rowIds) {
    for (const RowId rb : b.rowIds) {
      if (ra == rb) {
        return true;
      }
    }
  }
  return false;
}

bool overlayHasConflict(const Overlay& overlay)
{
  for (size_t i = 0; i < overlay.size(); ++i) {
    const FillerRewrite ri = overlay[i].rewrite();
    for (size_t j = i + 1; j < overlay.size(); ++j) {
      if (movesConflict(ri, overlay[j].rewrite())) {
        return true;
      }
    }
  }
  return false;
}

std::string canonicalKey(const Overlay& overlay)
{
  std::vector<FillerRewrite> rewrites;
  rewrites.reserve(overlay.size());
  for (const SwapMove& move : overlay) {
    rewrites.push_back(move.rewrite());
  }
  std::sort(rewrites.begin(),
            rewrites.end(),
            [](const FillerRewrite& a, const FillerRewrite& b) {
              if (a.rowIds != b.rowIds) {
                return a.rowIds < b.rowIds;
              }
              if (a.span.xl != b.span.xl) {
                return a.span.xl < b.span.xl;
              }
              if (a.span.xh != b.span.xh) {
                return a.span.xh < b.span.xh;
              }
              return a.newMasterIds < b.newMasterIds;
            });

  std::string key;
  for (const FillerRewrite& r : rewrites) {
    key += 'r';
    for (const RowId row : r.rowIds) {
      key += cat(row, '.');
    }
    key += cat(':', r.span.xl, '-', r.span.xh, ':');
    for (const MasterId m : r.newMasterIds) {
      key += cat('m', m, '.');
    }
    key += '|';
  }
  return key;
}

std::vector<FillerChange> toFillerChanges(const Overlay& overlay)
{
  std::vector<FillerChange> changes;
  changes.reserve(overlay.size());
  for (const SwapMove& move : overlay) {
    changes.push_back(move.change());
  }
  std::sort(changes.begin(),
            changes.end(),
            [](const FillerChange& a, const FillerChange& b) {
              return a.instanceId < b.instanceId;
            });
  return changes;
}

std::optional<std::string> validateRewriteCoverage(const PlacementView& view,
                                                   const FillerRewrite& rewrite)
{
  if (rewrite.span.empty()) {
    return cat("empty span ", show(rewrite.span));
  }
  if (rewrite.rowIds.empty()) {
    return std::string("no rows");
  }

  // New masters: fillers only, widths must tile the span exactly.
  DbCoord tiledWidth = 0;
  for (const MasterId masterId : rewrite.newMasterIds) {
    const MasterInfo* master = view.masterInfo(masterId);
    if (master == nullptr) {
      return cat("unknown master ", masterId);
    }
    if (!master->isFiller) {
      return cat("master ", masterId, " is not a filler master");
    }
    tiledWidth += master->width;
  }
  if (tiledWidth != rewrite.span.length()) {
    return cat("tiling width ", tiledWidth, " != span length ", rewrite.span.length());
  }

  // Per row: instances overlapping the span must be fillers fully inside it,
  // and together must cover it without holes.
  for (const RowId rowId : rewrite.rowIds) {
    DbCoord cursor = rewrite.span.xl;
    for (const PlacedInstance& inst : view.instancesInRow(rowId)) {
      const XInterval span = instanceSpan(view, inst);
      if (!span.overlaps(rewrite.span)) {
        continue;
      }
      if (!inst.isFiller) {
        return cat("row ", rowId, ": non-filler instance ", inst.id,
                   " inside span ", show(rewrite.span));
      }
      if (span.xl < rewrite.span.xl || span.xh > rewrite.span.xh) {
        return cat("row ", rowId, ": instance ", inst.id, " ", show(span),
                   " partially covered by span ", show(rewrite.span));
      }
      if (span.xl != cursor) {
        return cat("row ", rowId, ": coverage hole ", show(XInterval{cursor, span.xl}),
                   " inside span ", show(rewrite.span));
      }
      cursor = span.xh;
    }
    if (cursor != rewrite.span.xh) {
      return cat("row ", rowId, ": coverage hole ",
                 show(XInterval{cursor, rewrite.span.xh}), " inside span ",
                 show(rewrite.span));
    }
  }
  return std::nullopt;
}

}  // namespace dpl2::fillerRepair
