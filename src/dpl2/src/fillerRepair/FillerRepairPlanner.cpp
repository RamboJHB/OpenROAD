// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "FillerRepairPlanner.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace dpl2::fillerRepair {

// --------------------------------------------------------------------------
// merged from Swap.cpp
// --------------------------------------------------------------------------

std::optional<Swap> makeSwap(const PlannerDataSource& view,
                             InstanceId instanceId,
                             MasterId newMasterId,
                             std::string* error)
{
  const auto fail = [error](std::string why) -> std::optional<Swap> {
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

  Swap swap;
  swap.instanceId = instanceId;
  swap.oldMasterId = inst->masterId;
  swap.newMasterId = newMasterId;
  swap.rowId = inst->rowId;
  swap.span = XInterval{inst->x, inst->x + oldMaster->width};
  swap.oldVt = oldMaster->vt;
  swap.newVt = newMaster->vt;
  return swap;
}

std::string canonicalKey(const Overlay& overlay)
{
  std::vector<std::pair<InstanceId, MasterId>> pairs;
  pairs.reserve(overlay.size());
  for (const Swap& swap : overlay) {
    pairs.emplace_back(swap.instanceId, swap.newMasterId);
  }
  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

  std::string key;
  for (const auto& [instanceId, masterId] : pairs) {
    key += cat('i', instanceId, 'm', masterId, '|');
  }
  return key;
}

ipl::FillerChanges toFillerChanges(const Overlay& overlay,
                                   const PlannerDataSource& dataSource)
{
  ipl::FillerChanges changes;
  changes.reserve(overlay.size());
  for (const Swap& swap : overlay) {
    changes.push_back(
        dataSource.fillerCellRecord(swap.instanceId, swap.newMasterId));
  }
  std::sort(changes.begin(),
            changes.end(),
            [](const FillerCellRecord& a, const FillerCellRecord& b) {
              return a.cell_id_.getIndexValue() < b.cell_id_.getIndexValue();
            });
  return changes;
}

SwapGenerationResult generateSwaps(
    const RepairWindow& window,
    const PlannerDataSource& view,
    const DebugLog& log)
{
  SwapGenerationResult result;

  for (const InstanceId fillerId : window.editableFillers) {
    MasterCandidateResult candidates =
        view.getUsableMasterCandidates({fillerId});
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
        log.msg("swapgen",
                cat("reject filler=", fillerId, " master=",
                    candidate.masterId, " reason=", error));
        continue;
      }
      result.swaps.push_back(*swap);
      ++emitted;
      log.msg("swapgen",
              cat("emit filler=", swap->instanceId, " row=", swap->rowId,
                  " span=", show(swap->span), " master ", swap->oldMasterId,
                  "(vt", swap->oldVt, ") -> ", swap->newMasterId, "(vt",
                  swap->newVt, ')'));
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

// --------------------------------------------------------------------------
// merged from Signature.cpp
// --------------------------------------------------------------------------

namespace {

std::vector<RowId> sortedUniqueRows(const std::vector<RowId>& rows)
{
  std::vector<RowId> result = rows;
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

const char* kindName(ViolationKind kind)
{
  return kind == ViolationKind::MinWidth ? "MW" : "MS";
}

const char* relationName(ViolationRelation relation)
{
  switch (relation) {
    case ViolationRelation::IntraRow:
      return "intraRow";
    case ViolationRelation::InterRow:
      return "interRow";
  }
  return "?";
}

// Distance between two disjoint intervals; <= 0 when they overlap/touch.
DbCoord intervalDistance(const XInterval& a, const XInterval& b)
{
  return std::max(a.xl, b.xl) - std::min(a.xh, b.xh);
}

}  // namespace

std::vector<NormalizedViolation> normalizeViolations(
    const FillerRepairRequest& request,
    const PlannerDataSource& view,
    const DebugLog& log)
{
  std::vector<NormalizedViolation> result;
  result.reserve(request.violations.size());
  int rowFallbacks = 0;

  for (size_t i = 0; i < request.violations.size(); ++i) {
    const Violation& raw = request.violations[i];
    NormalizedViolation nv;
    nv.raw = raw;

    // Rows: checker-provided, sorted unique; when missing, fall back to the
    // anchor row and flag it (spec 6.2).
    nv.rowIds = sortedUniqueRows(raw.rowIds);
    if (nv.rowIds.empty()) {
      nv.rowIds = {request.targetPlace.rowId};
      nv.rowIdFallback = true;
      ++rowFallbacks;
    }

    // Footprint: xWindow united with every participant's x range.
    nv.xRange = raw.xWindow;
    for (const ViolationParticipant& p : raw.participants) {
      if (!p.xRange.empty()) {
        nv.xRange.xl = std::min(nv.xRange.xl, p.xRange.xl);
        nv.xRange.xh = std::max(nv.xRange.xh, p.xRange.xh);
      }
      if (p.isFiller) {
        nv.fillerParticipants.push_back(p.instanceId);
      } else {
        nv.cellAnchors.push_back(p.instanceId);
      }
    }

    // The changed std cell is always an anchor, participant or not.
    const InstanceId target = request.targetPlace.instanceId;
    if (std::find(nv.cellAnchors.begin(), nv.cellAnchors.end(), target)
        == nv.cellAnchors.end()) {
      nv.cellAnchors.push_back(target);
    }
    std::sort(nv.cellAnchors.begin(), nv.cellAnchors.end());
    std::sort(nv.fillerParticipants.begin(), nv.fillerParticipants.end());

    log.msg("normalize",
            cat("violation#", i, " rule=", raw.ruleId, ' ', kindName(raw.kind),
                '/', relationName(raw.relation), " layer=", raw.primaryLayer,
                (raw.secondaryLayer ? cat('/', *raw.secondaryLayer) : std::string()),
                " rows=", nv.rowIds.size(),
                (nv.rowIdFallback ? " (fallback to anchor row)" : ""),
                " xWindow=", show(raw.xWindow), " -> footprint=",
                show(nv.xRange), " anchors=", nv.cellAnchors.size(),
                " fillers=", nv.fillerParticipants.size()));

    result.push_back(std::move(nv));
  }

  log.msg("normalize",
          cat("normalized ", result.size(), " violation(s); rowFallbacks=",
              rowFallbacks, " anchor=", request.targetPlace.instanceId));
  (void) view;  // reserved for participant lookups when checker data is thin
  return result;
}

bool sameSignature(const Violation& a, const Violation& b, DbCoord siteWidth)
{
  if (a.ruleId != b.ruleId || a.kind != b.kind || a.relation != b.relation) {
    return false;
  }
  // Implant layers distinguish otherwise-identical violations -- in
  // particular P-band vs N-band MS at the same x gap, which the checker may
  // report with the same ruleId/kind/relation/rows (spec 6.2). Both default
  // to 0/nullopt when the checker leaves them unset, so this is a no-op for
  // layer-agnostic checkers.
  if (a.primaryLayer != b.primaryLayer || a.secondaryLayer != b.secondaryLayer) {
    return false;
  }
  if (sortedUniqueRows(a.rowIds) != sortedUniqueRows(b.rowIds)) {
    return false;
  }

  // xWindow tolerance: overlap of at least half the shorter window, or the
  // windows lie within one site of each other (covers zero-length windows
  // and one-site jitter across snapshots).
  const DbCoord overlap = std::min(a.xWindow.xh, b.xWindow.xh)
                          - std::max(a.xWindow.xl, b.xWindow.xl);
  const DbCoord shorter = std::min(a.xWindow.length(), b.xWindow.length());
  if (overlap * 2 >= shorter && overlap > 0) {
    return true;
  }
  return intervalDistance(a.xWindow, b.xWindow) <= siteWidth;
}

bool isRelatedToOverlay(const Violation& violation,
                        const Overlay& overlay,
                        DbCoord ruleDistance)
{
  for (const Swap& swap : overlay) {
    // Direct participation of a changed instance.
    for (const ViolationParticipant& p : violation.participants) {
      if (p.instanceId == swap.instanceId) {
        return true;
      }
    }
    // Geometric proximity: within one rule distance of the changed span, on
    // the same or a vertically adjacent row (implant rules couple at most
    // adjacent rows).
    bool rowNear = violation.rowIds.empty();  // no row info -> conservative
    for (const RowId row : violation.rowIds) {
      if (row >= swap.rowId - 1 && row <= swap.rowId + 1) {
        rowNear = true;
        break;
      }
    }
    if (rowNear && intervalDistance(violation.xWindow, swap.span) <= ruleDistance) {
      return true;
    }
  }
  return false;
}

DbCoord estimateRuleDistance(const std::vector<Violation>& violations,
                             DbCoord siteWidth)
{
  DbCoord distance = siteWidth;
  for (const Violation& v : violations) {
    distance = std::max(distance, v.requiredValue);
  }
  return distance;
}

// --------------------------------------------------------------------------
// merged from Window.cpp
// --------------------------------------------------------------------------

namespace {

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

// --------------------------------------------------------------------------
// merged from Ranker.cpp
// --------------------------------------------------------------------------

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

  // Only the immediate x-neighbors matter, so binary-search to inst's span
  // instead of scanning the whole (packed) row for every ranked filler.
  // Defensive: an instance with a missing master must not crash the vote
  // (upstream validation makes it unreachable in runtime, but the ranker
  // must not rely on two layers above it).

  // Same-row x-adjacent neighbors (touching an edge) each cast two band votes.
  // A toucher does not overlap span (it meets an edge), so it sits just
  // outside the overlap range -- widen by one instance on each side.
  {
    const std::vector<PlacedInstance>& all = view.instancesInRow(inst.rowId);
    const int lo = firstRightEdgeAfter(view, all, span.xl);
    const int hi = firstStartAtOrAfter(all, span.xh);
    const int from = std::max(0, lo - 1);
    const int to = std::min(static_cast<int>(all.size()), hi + 1);
    for (int i = from; i < to; ++i) {
      const PlacedInstance& other = all[i];
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
  }
  // Rows +-1 neighbors that overlap span each cast one band vote. The overlap
  // range is exactly [firstRightEdgeAfter(xl), firstStartAtOrAfter(xh)).
  for (const RowId rowId : {inst.rowId - 1, inst.rowId + 1}) {
    const std::vector<PlacedInstance>& all = view.instancesInRow(rowId);
    const int lo = firstRightEdgeAfter(view, all, span.xl);
    const int hi = firstStartAtOrAfter(all, span.xh);
    for (int i = lo; i < hi; ++i) {
      const MasterInfo* master = view.masterInfo(all[i].masterId);
      if (master == nullptr) {
        continue;
      }
      votes[master->vt] += 1;
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

// --------------------------------------------------------------------------
// merged from SubsetSearch.cpp
// --------------------------------------------------------------------------

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
    log.msg("enumerate",
            cat("skip enumeration: domains=", ranked.size(),
                " budget=", budget));
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
    const size_t before = plan.overlays.size();
    const int cap = memberCap(size);
    choose(size, 0, cap);
    // Per-size accounting makes it obvious when a large-window member cap or
    // the checker budget, rather than the legality oracle, removed candidates.
    log.msg("enumerate",
            cat("subsetSize=", size, " memberPrefix=", cap, '/',
                fillerTotal, " -> emitted=", plan.overlays.size() - before,
                budgetHit ? " (budget reached)" : ""));
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

// --------------------------------------------------------------------------
// merged from OracleGate.cpp
// --------------------------------------------------------------------------

namespace {

// A violation falls inside the repair window when its x overlaps the editable
// span and at least one of its rows is editable (spec 6.8 rule 3). Empty rows
// -> not in-window, matching the checker-provided-rows fallback.
bool inRepairWindow(const Violation& v, const RepairWindow& window)
{
  if (!v.xWindow.overlaps(window.x)) {
    return false;
  }
  for (const RowId row : v.rowIds) {
    if (std::find(window.rows.begin(), window.rows.end(), row)
        != window.rows.end()) {
      return true;
    }
  }
  return false;
}

// A violation is observable in a baseline collected within `guard` when it
// overlaps the guard geometrically. Used to scope the baseline-reproduces-
// originals check: an original outside the current guard is simply not this
// window's responsibility (it is covered once the window grows).
bool inGuardRegion(const Violation& v, const Region& guard)
{
  if (!v.xWindow.overlaps(guard.x)) {
    return false;
  }
  if (v.rowIds.empty()) {
    return true;  // no row info -> conservative: treat as in-guard by x
  }
  for (const RowId row : v.rowIds) {
    if (row >= guard.rowLo && row <= guard.rowHi) {
      return true;
    }
  }
  return false;
}

}  // namespace

OracleGate::OracleGate(const PlannerDataSource& dataSource,
                       PlannerOracle& oracle,
                       const TargetPlace& anchor,
                       const std::vector<Violation>& originals,
                       DbCoord siteWidth,
                       DbCoord ruleDistance,
                       const RepairConfig& config,
                       const DebugLog& log)
    : data_source_(dataSource),
      oracle_(oracle),
      anchor_(anchor),
      originals_(originals),
      site_width_(siteWidth),
      rule_distance_(ruleDistance),
      config_(config),
      log_(log)
{
}

std::string OracleGate::cacheKey(const Region& guard, const Overlay& overlay) const
{
  // Guard region is part of the key: the same overlay under a different
  // guard is a different checker question.
  return cat('g', guard.x.xl, ':', guard.x.xh, ':', guard.rowLo, ':',
             guard.rowHi, '|', canonicalKey(overlay));
}

bool OracleGate::runBaseline(const RepairWindow& window, int& budget)
{
  const Region& guard = window.guardRegion;
  const std::string key = cacheKey(guard, {});
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    ++cache_hits_;
    log_.msg("gate", cat("baseline cache hit for guard ", show(guard)));
  } else {
    if (budget <= 0) {
      log_.msg("gate", "baseline skipped: checker budget is exhausted");
      return false;
    }
    OracleRequest request;
    request.requestId = next_request_id_++;
    request.targetPlace = anchor_;
    request.guardRegion = guard;
    log_.msg("gate",
             cat("baseline send id=", request.requestId, " guard=",
                 show(guard), " budgetBefore=", budget));
    const OracleResult result = oracle_.checkPlaceWithOverlay(request);
    ++requests_sent_;
    --budget;
    if (result.requestId != request.requestId) {
      diagnostics_.push_back(makeDiag(
          Severity::Fatal, "CheckerProtocolError",
          cat("baseline echoed id ", result.requestId, " != ", request.requestId)));
      return false;
    }
    log_.msg("gate",
             cat("baseline recv id=", result.requestId, " status=",
                 static_cast<int>(result.status), " legal=", result.isLegal,
                 " violations=", result.violations.size(), " diagnostics=",
                 result.diagnostics.size(), " budgetAfter=", budget));
    it = cache_.emplace(key, result).first;
  }

  baseline_ = &it->second;
  if (baseline_->status != OracleStatus::Checked) {
    diagnostics_.push_back(makeDiag(Severity::Error, "BaselineUnusable",
                                    "baseline check did not complete"));
    baseline_ = nullptr;
    return false;
  }
  log_.msg("gate",
           cat("baseline for guard ", show(guard), ": ",
               baseline_->violations.size(), " violation(s)"));

  // Baseline consistency gate: refuse to search on a stale/inconsistent
  // snapshot (spec 6.8, V2.1 #2+#4).
  if (!checkBaselineConsistency(window)) {
    baseline_ = nullptr;
    return false;
  }
  return true;
}

bool OracleGate::checkBaselineConsistency(const RepairWindow& window)
{
  // One-to-one bookkeeping so a single baseline finding cannot satisfy two
  // originals (and, below, cannot be double-counted as unexpected).
  std::vector<char> consumed(baseline_->violations.size(), 0);

  // (a) Every original inside the guard must reproduce in the baseline. If it
  //     does not, the input snapshot is stale/inconsistent and a candidate
  //     that merely "does not observe" it would be mistaken for a repair.
  int inGuardOriginals = 0;
  for (const Violation& original : originals_) {
    if (!inGuardRegion(original, window.guardRegion)) {
      continue;  // outside this window's scope; a larger window will cover it
    }
    ++inGuardOriginals;
    bool matched = false;
    for (size_t i = 0; i < baseline_->violations.size(); ++i) {
      if (!consumed[i]
          && sameSignature(original, baseline_->violations[i], site_width_)) {
        consumed[i] = 1;
        matched = true;
        break;
      }
    }
    if (!matched) {
      diagnostics_.push_back(makeDiag(
          Severity::Fatal, "BaselineMismatch",
          cat("original rule=", original.ruleId, ' ', show(original.xWindow),
              " lies in the guard but is absent from the baseline; input "
              "snapshot is stale/inconsistent -- refusing to search")));
      log_.msg("gate",
               cat("baseline MISMATCH: original ", show(original.xWindow),
                   " not reproduced -> abort window"));
      return false;
    }
  }

  // (b) No unexpected in-window violation may pre-exist in the baseline: by the
  //     §2.3 assumption the input snapshot is clean apart from the originals,
  //     so an unmatched baseline finding inside the repair window signals an
  //     inconsistent snapshot. Unmatched findings OUTSIDE the window are the
  //     allowed unrelated pre-existing halo (spec 6.8 rule 5).
  for (size_t i = 0; i < baseline_->violations.size(); ++i) {
    if (consumed[i]) {
      continue;
    }
    const Violation& b = baseline_->violations[i];
    if (inRepairWindow(b, window)) {
      diagnostics_.push_back(makeDiag(
          Severity::Fatal, "BaselineMismatch",
          cat("baseline carries an in-window violation rule=", b.ruleId, ' ',
              show(b.xWindow), " that is not among the original snapshot; "
              "input is inconsistent -- refusing to search")));
      log_.msg("gate",
               cat("baseline MISMATCH: unexpected in-window ", show(b.xWindow),
                   " -> abort window"));
      return false;
    }
  }

  log_.msg("gate",
           cat("baseline consistent: ", inGuardOriginals,
               " in-guard original(s) all reproduced, no unexpected in-window "
               "violation"));
  return true;
}

DeltaSummary OracleGate::classify(const OracleResult& result,
                                  const Overlay& overlay,
                                  const RepairWindow& window) const
{
  DeltaSummary summary;
  summary.usable = result.status == OracleStatus::Checked;
  for (const Diagnostic& diag : result.diagnostics) {
    summary.usable &= diag.severity != Severity::Fatal;
  }
  if (!summary.usable) {
    return summary;
  }
  // Self-consistency both ways (V2.1 #1): isLegal must agree with whether the
  // result reports violations. `isLegal && violations non-empty` AND
  // `!isLegal && violations empty` (an unexplained illegal result, which the
  // real checker returns for blocking overlaps / off-grid / polarity) are both
  // rejected as not clean.
  summary.inconsistent = (result.isLegal != result.violations.empty());

  // Residual originals: match each original to a DISTINCT result finding, so
  // two originals cannot both claim the same one (V2.1 #3, one-to-one).
  {
    std::vector<char> consumed(result.violations.size(), 0);
    for (const Violation& original : originals_) {
      for (size_t i = 0; i < result.violations.size(); ++i) {
        if (!consumed[i]
            && sameSignature(original, result.violations[i], site_width_)) {
          consumed[i] = 1;
          ++summary.residualOriginals;
          summary.blockingViolations.push_back(result.violations[i]);
          break;
        }
      }
    }
  }

  // New violations = result findings not matched one-to-one against the
  // baseline (V2.1 #3): one baseline finding absorbs at most one candidate
  // finding, so a second same-signature finding is correctly counted as new
  // (P/N bands + the one-site signature tolerance make duplicates real). Inside
  // the repair window a new violation always rejects; in the guard halo only
  // when related to this overlay.
  std::vector<char> baselineConsumed(baseline_->violations.size(), 0);
  for (const Violation& v : result.violations) {
    bool preExisting = false;
    for (size_t i = 0; i < baseline_->violations.size(); ++i) {
      if (!baselineConsumed[i]
          && sameSignature(v, baseline_->violations[i], site_width_)) {
        baselineConsumed[i] = 1;
        preExisting = true;
        break;
      }
    }
    if (preExisting) {
      continue;
    }
    if (inRepairWindow(v, window)) {
      ++summary.newInWindow;
      summary.blockingViolations.push_back(v);
    } else if (isRelatedToOverlay(v, overlay,
                                  std::max(rule_distance_, v.requiredValue))) {
      // Per-violation rule distance (V2.1 #5): a new violation from a
      // larger-distance rule must not be mislabeled unrelated and let through.
      ++summary.relatedInHalo;
      summary.blockingViolations.push_back(v);
    } else {
      ++summary.unrelatedInHalo;
    }
  }

  summary.clean = !summary.inconsistent && summary.residualOriginals == 0
                  && summary.newInWindow == 0 && summary.relatedInHalo == 0;
  return summary;
}

const OracleResult* OracleGate::resolve(const std::vector<Overlay>& chunk,
                                        const Region& guard,
                                        int& budget,
                                        bool& protocolError)
{
  // Send everything in the chunk that is not cached yet as one batch.
  std::vector<OracleRequest> requests;
  std::vector<std::string> keys;
  int cachedInChunk = 0;
  int skippedForBudget = 0;
  const int budgetBefore = budget;
  for (const Overlay& overlay : chunk) {
    const std::string key = cacheKey(guard, overlay);
    const bool cached = cache_.count(key) > 0;
    if (cached || budget <= 0) {
      if (cached) {
        ++cache_hits_;
        ++cachedInChunk;
      } else {
        ++skippedForBudget;
      }
      continue;
    }
    OracleRequest request;
    request.requestId = next_request_id_++;
    request.targetPlace = anchor_;
    request.guardRegion = guard;
    request.fillerChanges = toFillerChanges(overlay, data_source_);
    requests.push_back(std::move(request));
    keys.push_back(key);
    --budget;
  }
  if (!requests.empty()) {
    log_.msg("gate",
             cat("batch send: chunk=", chunk.size(), " uncached=",
                 requests.size(), " cached=", cachedInChunk,
                 " skippedForBudget=", skippedForBudget, " guard=",
                 show(guard), " budget ", budgetBefore, " -> ", budget));
    const std::vector<OracleResult> results =
        oracle_.checkPlaceWithOverlays(requests);
    ++batches_sent_;
    requests_sent_ += static_cast<int>(requests.size());

    // Protocol validation: one result per request, ids echo exactly once,
    // no unknown ids. Order must NOT matter -- map back by id.
    if (results.size() != requests.size()) {
      diagnostics_.push_back(makeDiag(
          Severity::Fatal, "CheckerProtocolError",
          cat("batch returned ", results.size(), " result(s) for ",
              requests.size(), " request(s)")));
      protocolError = true;
      log_.msg("gate",
               cat("batch protocol error: results=", results.size(),
                   " requests=", requests.size()));
      return nullptr;
    }
    std::map<OracleRequestId, const OracleResult*> byId;
    for (const OracleResult& result : results) {
      if (!byId.emplace(result.requestId, &result).second) {
        diagnostics_.push_back(makeDiag(Severity::Fatal, "CheckerProtocolError",
                                        cat("duplicate requestId ", result.requestId)));
        protocolError = true;
        log_.msg("gate",
                 cat("batch protocol error: duplicate id=", result.requestId));
        return nullptr;
      }
    }
    for (size_t i = 0; i < requests.size(); ++i) {
      const auto it = byId.find(requests[i].requestId);
      if (it == byId.end()) {
        diagnostics_.push_back(makeDiag(Severity::Fatal, "CheckerProtocolError",
                                        cat("missing result for requestId ",
                                            requests[i].requestId)));
        protocolError = true;
        log_.msg("gate",
                 cat("batch protocol error: missing id=",
                     requests[i].requestId));
        return nullptr;
      }
      cache_.emplace(keys[i], *it->second);
    }
    log_.msg("gate",
             cat("batch recv: results=", results.size(), " cacheSize=",
                 cache_.size(), " requestsTotal=", requests_sent_,
                 " batchesTotal=", batches_sent_));
  } else {
    log_.msg("gate",
             cat("batch avoided: chunk=", chunk.size(), " cached=",
                 cachedInChunk, " skippedForBudget=", skippedForBudget));
  }
  return baseline_;  // non-null marker; per-overlay lookup goes via cache_
}

OracleGate::SearchResult OracleGate::search(const std::vector<Overlay>& candidates,
                                            const RepairWindow& window,
                                            const Region& guard,
                                            int& budget)
{
  SearchResult sr;
  log_.msg("gate",
           cat("search start: candidates=", candidates.size(), " batchSize=",
               config_.batchSize, " budget=", budget, " window=",
               show(window.area()), " guard=", show(guard)));

  size_t next = 0;
  while (next < candidates.size()) {
    const size_t chunkEnd =
        std::min(candidates.size(),
                 next + static_cast<size_t>(config_.batchSize));
    const std::vector<Overlay> chunk(candidates.begin() + next,
                                     candidates.begin() + chunkEnd);
    bool protocolError = false;
    if (resolve(chunk, guard, budget, protocolError) == nullptr && protocolError) {
      sr.protocolError = true;
      return sr;
    }

    // Evaluate the chunk in enumeration order; first delta-clean wins.
    for (size_t i = next; i < chunkEnd; ++i) {
      const auto it = cache_.find(cacheKey(guard, candidates[i]));
      if (it == cache_.end()) {
        sr.budgetExhausted = true;  // was not evaluated: out of budget
        continue;
      }
      const DeltaSummary summary = classify(it->second, candidates[i], window);
      if (summary.clean) {
        sr.foundClean = true;
        sr.cleanOverlay = candidates[i];
        log_.msg("gate",
                 cat("clean overlay #", i, " (", candidates[i].size(),
                     " swap(s)) residual=0 newInWindow=0 relatedInHalo=0 ",
                     "unrelatedInHalo=", summary.unrelatedInHalo,
                     " -> accept"));
        return sr;
      }
      // Track best non-clean for diagnostics (fewer blocking findings, then
      // fewer changes, then earlier enumeration index).
      const int blockers = summary.residualOriginals + summary.newInWindow
                           + summary.relatedInHalo + (summary.usable ? 0 : 1000);
      const int bestBlockers = sr.bestSummary.residualOriginals
                               + sr.bestSummary.newInWindow
                               + sr.bestSummary.relatedInHalo
                               + (sr.bestSummary.usable ? 0 : 1000);
      if (!sr.hasBest || blockers < bestBlockers
          || (blockers == bestBlockers
              && candidates[i].size() < sr.bestOverlay.size())) {
        sr.hasBest = true;
        sr.bestOverlay = candidates[i];
        sr.bestSummary = summary;
        log_.msg("gate",
                 cat("best candidate #", i, " swaps=", candidates[i].size(),
                     " usable=", summary.usable,
                     " inconsistent=", summary.inconsistent,
                     " residual=", summary.residualOriginals,
                     " newInWindow=", summary.newInWindow,
                     " relatedInHalo=", summary.relatedInHalo,
                     " unrelatedInHalo=", summary.unrelatedInHalo));
      }
    }
    next = chunkEnd;
    if (budget <= 0) {
      sr.budgetExhausted = next < candidates.size();
      break;
    }
  }
  log_.msg("gate",
           cat("search end: clean=false best=", sr.hasBest,
               " budgetExhausted=", sr.budgetExhausted,
               " budgetRemaining=", budget));
  return sr;
}

// --------------------------------------------------------------------------
// merged from FillerRepairPlanner.cpp
// --------------------------------------------------------------------------

namespace {

int blockerCount(const OracleGate::SearchResult& result)
{
  return result.bestSummary.residualOriginals
         + result.bestSummary.newInWindow
         + result.bestSummary.relatedInHalo
         + (result.bestSummary.usable ? 0 : 1000);
}

bool betterBest(const OracleGate::SearchResult& candidate,
                const OracleGate::SearchResult& current)
{
  return candidate.hasBest
         && (!current.hasBest || blockerCount(candidate) < blockerCount(current)
             || (blockerCount(candidate) == blockerCount(current)
                 && candidate.bestOverlay.size() < current.bestOverlay.size()));
}

std::string windowLabel(const RepairWindow& window)
{
  return window.level == 0 ? "L0" : cat("adaptive-L1 step ", window.level);
}

}  // namespace

namespace internal {

FillerRepairPlanner::FillerRepairPlanner(
    const PlannerDataSource& view,
    PlannerOracle& oracle,
    RepairConfig config)
    : view_(view),
      oracle_(oracle),
      config_(config),
      log_(config.verbose)
{
}

FillerRepairResult FillerRepairPlanner::repair(
    const FillerRepairRequest& request)
{
  FillerRepairResult result;

  // Spec 3.3: the overlay API is a pure query and must never call back into
  // repair, and one planner instance never runs two repairs at once
  // (concurrent repairs = one planner per thread over a shared immutable
  // view). Turn a violation into a fatal result instead of corrupted state.
  if (repair_active_.exchange(true, std::memory_order_acq_rel)) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "ReentrantRepair",
        "repair() re-entered on this planner instance (oracle callback or "
        "concurrent use) -> refused"));
    return result;
  }
  struct ActiveGuard
  {
    std::atomic<bool>& flag;
    ~ActiveGuard() { flag.store(false, std::memory_order_release); }
  } activeGuard{repair_active_};

  // The transcript starts with both the immutable request and every search
  // knob. This makes a runtime failure reproducible from one captured log
  // without relying on hidden defaults.
  log_.msg("planner",
           cat("repair start: anchor inst=", request.targetPlace.instanceId,
               " master=", request.targetPlace.masterId,
               " row=", request.targetPlace.rowId,
               " x=", request.targetPlace.x,
               " violations=", request.violations.size()));
  log_.msg("planner",
           cat("config: budget/window=", config_.checkerCallBudgetPerWindow,
               " batch=", config_.batchSize,
               " maxSubset=", config_.maxSubsetSize,
               " memberCaps=[", config_.memberCapSize2, ',',
               config_.memberCapSize3, ',', config_.memberCapSize4,
               "] adaptiveStep=", config_.adaptiveStepFillers,
               " maxAdaptiveLevels=", config_.maxAdaptiveLevels));

  // Placement coverage is intentionally not checked here. Runtime opto
  // calls FillerRepairEngine::precheck() before any mutation; keeping that
  // gate out of repair preserves the explicit orchestration contract.
  // Empty snapshot: nothing to repair is a success with no changes.
  if (request.violations.empty()) {
    result.hasSolution = true;
    result.diagnostics.push_back(makeDiag(
        Severity::Info, "EmptySnapshot", "no violations in initial snapshot"));
    log_.msg("planner",
             "empty violation snapshot -> hasSolution=true, 0 changes");
    return result;
  }

  // Stage 2 (spec 6.2): normalize the snapshot into signatures/footprints.
  const std::vector<NormalizedViolation> violations =
      normalizeViolations(request, view_, log_);

  const DbCoord ruleDistance =
      estimateRuleDistance(request.violations, view_.siteWidth());
  log_.msg("planner",
           cat("normalized=", violations.size(), " siteWidth=",
               view_.siteWidth(), " ruleDistance=", ruleDistance,
               " -> build L0 window"));
  OracleGate gate(view_, oracle_, request.targetPlace, request.violations,
                  view_.siteWidth(), ruleDistance, config_, log_);

  // Stages 3..7 under the adaptive window loop (spec 6.3/6.7/6.8,
  // V2.1 #8): search L0, then grow K fillers toward the best non-clean
  // candidate's blocking side until clean or an expansion cutoff.
  OracleGate::SearchResult best;  // best non-clean across windows (diagnostics)
  // Definitive iff the LAST window we actually searched was fully enumerated
  // (V2.1 #10): an earlier smaller window being complete does not prove the
  // later truncated window has no solution.
  bool lastSearchedDefinitive = false;
  RepairWindow window = buildWindow(0,
                                    request.targetPlace,
                                    violations,
                                    view_,
                                    ruleDistance,
                                    log_);

  for (;;) {
    const std::string label = windowLabel(window);
    std::vector<Violation> blockingForExpansion = request.violations;
    bool currentDefinitive = false;

    // One loop iteration is one independently budgeted search question. The
    // window and guard are logged before generating swaps so a transcript can
    // explain exactly which fillers were editable versus check-only.
    log_.msg("planner",
             cat("search ", label, ": area=", show(window.area()),
                 " guard=", show(window.guardRegion), " editable=",
                 window.editableFillers.size(), " bridge=",
                 window.bridgeFillers.size()));

    if (window.editableFillers.empty()) {
      result.diagnostics.push_back(makeDiag(
          Severity::Warning, "NoEditableFiller",
          cat("window ", label, " ", show(window.area()),
              " contains no editable filler")));
    } else {
      const SwapGenerationResult generated =
          generateSwaps(window, view_, log_);
      result.diagnostics.insert(result.diagnostics.end(),
                                generated.diagnostics.begin(),
                                generated.diagnostics.end());
      if (generated.swaps.empty()) {
        result.diagnostics.push_back(makeDiag(
            Severity::Warning, "NoSwapGenerated",
            cat("window ", label, ": no usable swap")));
      } else {
        const std::vector<FillerDomain> ranked =
            rankFillers(generated.swaps,
                        request.targetPlace,
                        violations,
                        window,
                        view_,
                        log_);

        int budget = config_.checkerCallBudgetPerWindow;
        if (!gate.runBaseline(window, budget)) {
          result.hasSolution = false;
          result.diagnostics.insert(result.diagnostics.end(),
                                    gate.diagnostics().begin(),
                                    gate.diagnostics().end());
          result.diagnostics.push_back(makeDiag(
              Severity::Fatal, "BaselineGateFailed",
              cat("baseline gate failed at window ", label,
                  " (see BaselineUnusable/BaselineMismatch above)")));
          log_.msg("planner", "baseline gate failed -> abort");
          return result;
        }

        const EnumerationPlan plan =
            enumerateOverlays(ranked, config_, budget, log_);
        OracleGate::SearchResult sr =
            gate.search(plan.overlays, window, window.guardRegion, budget);
        if (sr.protocolError) {
          result.hasSolution = false;
          result.diagnostics.insert(result.diagnostics.end(),
                                    gate.diagnostics().begin(),
                                    gate.diagnostics().end());
          result.diagnostics.push_back(makeDiag(
              Severity::Fatal, "CheckerProtocolError",
              "batch protocol violated; rejecting this repair"));
          log_.msg("planner", "checker protocol error -> abort");
          return result;
        }

        if (sr.foundClean) {
          result.hasSolution = true;
          result.changes = toFillerChanges(sr.cleanOverlay, view_);
          result.diagnostics.push_back(makeDiag(
              Severity::Info, "Solution",
              cat("window ", label, ": ", result.changes.size(),
                  " change(s); checker requests=", gate.requestsSent(),
                  " batches=", gate.batchesSent(),
                  " cacheHits=", gate.cacheHits())));
          log_.msg("planner",
                   cat("SOLUTION at ", label, ": ", result.changes.size(),
                       " change(s), requests=", gate.requestsSent(),
                       " cacheHits=", gate.cacheHits()));
          return result;
        }

        if (betterBest(sr, best)) {
          best = sr;
          log_.msg("planner",
                   cat("best-so-far updated at ", label, ": swaps=",
                       best.bestOverlay.size(), " residual=",
                       best.bestSummary.residualOriginals, " newInWindow=",
                       best.bestSummary.newInWindow, " relatedInHalo=",
                       best.bestSummary.relatedInHalo));
        }
        currentDefinitive = plan.complete && !sr.budgetExhausted;
        lastSearchedDefinitive = currentDefinitive;
        if (sr.hasBest && !sr.bestSummary.blockingViolations.empty()) {
          blockingForExpansion = sr.bestSummary.blockingViolations;
        }
        result.diagnostics.push_back(makeDiag(
            Severity::Info, "WindowExhausted",
            cat("window ", label, ": ", plan.overlays.size(),
                " candidate(s), ",
                currentDefinitive
                    ? "complete enumeration, definitively no clean overlay"
                    : "truncated (size caps or budget), no clean overlay found")));
        log_.msg("planner",
                 cat("window ", label, " no clean overlay (",
                     currentDefinitive ? "definitive" : "truncated",
                     ") -> adaptive expansion"));
      }
    }

    // A stable blocking set is not a proof that farther fillers cannot form a
    // clean non-monotone multi-swap. Keep expanding until no adjacent filler
    // can be added, the level cap fires, or the normal per-window search
    // limits stop enumeration.
    if (window.level >= config_.maxAdaptiveLevels) {
      result.diagnostics.push_back(makeDiag(
          Severity::Info, "ExpansionCutoff",
          cat("window ", label, " reached maxAdaptiveLevels=",
              config_.maxAdaptiveLevels, " -> stop escalation (truncated)")));
      log_.msg("planner",
               cat("window ", label, " adaptive level cap ",
                   config_.maxAdaptiveLevels, " -> truncated"));
      // Farther windows were never searched, so no-solution is not definitive.
      lastSearchedDefinitive = false;
      break;
    }
    const RepairWindow expanded = expandWindowAdaptive(
        window,
        request.targetPlace,
        blockingForExpansion,
        view_,
        config_.adaptiveStepFillers,
        log_);
    if (expanded.editableFillers == window.editableFillers) {
      result.diagnostics.push_back(makeDiag(
          Severity::Info, "ExpansionCutoff",
          cat("window ", label,
              " adaptive step adds no new editable filler -> stop escalation")));
      log_.msg("planner",
               cat("window ", label,
                   " adaptive step adds no filler -> expansion cutoff"));
      break;
    }
    window = expanded;
  }

  // No clean overlay anywhere (spec 6.9): empty changes, explain why.
  result.hasSolution = false;
  result.diagnostics.push_back(makeDiag(
      Severity::Error, "NoCleanOverlay",
      cat("no baseline-delta clean overlay found; ",
          lastSearchedDefinitive ? "window space exhausted definitively" : "budget/caps truncated",
          "; checker requests=", gate.requestsSent(), " batches=",
          gate.batchesSent(), " cacheHits=", gate.cacheHits())));
  if (best.hasBest) {
    result.diagnostics.push_back(makeDiag(
        Severity::Info, "BestOverlay",
        cat("best non-clean candidate: ", best.bestOverlay.size(),
            " swap(s), residualOriginals=", best.bestSummary.residualOriginals,
            " newInWindow=", best.bestSummary.newInWindow,
            " relatedInHalo=", best.bestSummary.relatedInHalo,
            " unrelatedInHalo=", best.bestSummary.unrelatedInHalo)));
  }
  log_.msg("planner",
           cat("NO SOLUTION (", lastSearchedDefinitive ? "definitive" : "truncated",
               "), requests=", gate.requestsSent(),
               " cacheHits=", gate.cacheHits()));
  return result;
}

}  // namespace internal

}  // namespace dpl2::fillerRepair
