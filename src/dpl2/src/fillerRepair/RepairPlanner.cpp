// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerRepair/RepairPlanner.h>

#include <iterator>
#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace dpl2::fillerRepair {

// --------------------------------------------------------------------------
// Swap model -- one filler, one new master, everything else unchanged.
// --------------------------------------------------------------------------

std::optional<Swap> makeSwap(const PlacementView& view,
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

std::size_t OverlayKeyHash::operator()(const OverlayKey& key) const
{
  // FNV-1a over the key's integer fields. The swap list is already sorted and
  // deduplicated, so equal overlays hash equal regardless of enumeration
  // order.
  std::size_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](std::int64_t value) {
    hash ^= static_cast<std::size_t>(value);
    hash *= 1099511628211ULL;
  };
  mix(key.guardXl);
  mix(key.guardXh);
  mix(key.guardRowLo);
  mix(key.guardRowHi);
  for (const auto& [instanceId, masterId] : key) {
    mix(instanceId);
    mix(masterId);
  }
  return hash;
}

OverlayKey overlayKey(const Region& guard, const Overlay& overlay)
{
  // The guard is part of the identity: the same overlay under a different
  // guard is a different checker question.
  OverlayKey key;
  key.guardXl = guard.x.xl;
  key.guardXh = guard.x.xh;
  key.guardRowLo = guard.rowLo;
  key.guardRowHi = guard.rowHi;
  key.resize(overlay.size());
  OverlayKey::Entry* entries = key.data();
  for (std::size_t i = 0; i < overlay.size(); ++i) {
    entries[i] = {overlay[i].instanceId, overlay[i].newMasterId};
  }
  std::sort(entries, entries + key.size());
  key.resize(static_cast<std::size_t>(
      std::unique(entries, entries + key.size()) - entries));
  return key;
}

ipl::FillerChanges toFillerChanges(const Overlay& overlay,
                                   const PlacementView& dataSource)
{
  ipl::FillerChanges changes;
  changes.reserve(overlay.size());
  for (const Swap& swap : overlay) {
    changes.push_back(
        dataSource.cellChangeRecord(swap.instanceId, swap.newMasterId));
  }
  std::sort(changes.begin(),
            changes.end(),
            [](const CellChangeRecord& a, const CellChangeRecord& b) {
              return cellChangeRecordInstanceId(a)
                     < cellChangeRecordInstanceId(b);
            });
  return changes;
}

namespace {

const char* candidateOrientName(Orient orientation)
{
  switch (orientation) {
    case Orient::R180: return "R180";
    case Orient::MX: return "MX";
    case Orient::MY: return "MY";
    case Orient::R0: return "R0";
  }
  return "UNKNOWN";
}

const char* candidatePolarityName(BandPolarity polarity)
{
  return polarity == BandPolarity::P ? "P" : "N";
}

void appendCandidateReason(std::string& reasons, const char* reason)
{
  if (!reasons.empty()) {
    reasons += ',';
  }
  reasons += reason;
}

bool providerReturned(const MasterCandidateResult& result, MasterId id)
{
  return std::any_of(result.candidates.begin(), result.candidates.end(),
                     [id](const MasterCandidate& candidate) {
                       return candidate.masterId == id;
                     });
}

void logCandidateProviderTrace(const PlacementView& view,
                               InstanceId fillerId,
                               const MasterCandidateResult& result,
                               const DebugLog& log)
{
  if (!log.enabled()) {
    return;
  }

  const PlacedInstance* inst = view.instance(fillerId);
  const MasterInfo* current
      = inst != nullptr ? view.masterInfo(inst->masterId) : nullptr;
  log.msg("candidate", [&] {
    const std::string instanceInfo
        = inst == nullptr
              ? "instance=MISSING"
              : cat("instance{row=", inst->rowId, " x=", inst->x,
                    " orient=", candidateOrientName(inst->orientation),
                    " filler=", inst->isFiller,
                    " currentMaster=", inst->masterId, '}');
    const std::string currentInfo
        = current == nullptr
              ? "current{metadata=MISSING}"
              : cat("current{master=", current->id,
                    " filler=", current->isFiller, " vt=", current->vt,
                    " width=", current->width, " height=", current->height,
                    " bottom=",
                    candidatePolarityName(current->bottomBandPolarity), '}');
    return cat("provider request filler=", fillerId, ' ', instanceInfo, ' ',
               currentInfo, " configuredCount=", view.fillerMasterIds().size(),
               " returnedCount=", result.candidates.size(),
               " diagnosticCount=", result.diagnostics.size());
  });

  // A zero result gets the full rejection matrix. On the normal path, avoid
  // repeating the whole configured library for every editable filler and log
  // only the candidates the provider actually returned.
  const bool logRejected
      = result.candidates.empty() || !result.diagnostics.empty();
  for (const MasterId id : view.fillerMasterIds()) {
    const MasterInfo* candidate = view.masterInfo(id);
    const bool returned = providerReturned(result, id);
    if (!returned && !logRejected) {
      continue;
    }
    std::string reasons;
    if (inst == nullptr) {
      appendCandidateReason(reasons, "UNKNOWN_INSTANCE");
    } else if (!inst->isFiller) {
      appendCandidateReason(reasons, "INSTANCE_NOT_FILLER");
    }
    if (current == nullptr) {
      appendCandidateReason(reasons, "CURRENT_MASTER_METADATA_MISSING");
    } else if (current->vt == kUnknownVt) {
      appendCandidateReason(reasons, "CURRENT_VT_UNKNOWN");
    }
    if (candidate == nullptr) {
      appendCandidateReason(reasons, "CONFIGURED_MASTER_METADATA_MISSING");
    } else if (current != nullptr) {
      if (id == inst->masterId) {
        appendCandidateReason(reasons, "CURRENT_MASTER");
      }
      if (!candidate->isFiller) {
        appendCandidateReason(reasons, "NOT_FILLER");
      }
      if (candidate->vt == kUnknownVt) {
        appendCandidateReason(reasons, "VT_UNKNOWN");
      }
      if (candidate->vt == current->vt) {
        appendCandidateReason(reasons, "SAME_VT");
      }
      if (candidate->width != current->width) {
        appendCandidateReason(reasons, "WIDTH_MISMATCH");
      }
      if (candidate->height != current->height) {
        appendCandidateReason(reasons, "HEIGHT_MISMATCH");
      }
      if (candidate->bottomBandPolarity != current->bottomBandPolarity) {
        appendCandidateReason(reasons, "POLARITY_MISMATCH");
      }
    }
    if (!returned && reasons.empty()) {
      appendCandidateReason(reasons, "PROVIDER_DID_NOT_RETURN");
    }
    const char* decision = returned ? (reasons.empty() ? "accept"
                                                       : "returned-with-conflict")
                                    : "reject";
    log.msg("candidate", [&] {
      const std::string metadata
          = candidate == nullptr
                ? "metadata=MISSING"
                : cat("filler=", candidate->isFiller,
                      " vt=", candidate->vt, " width=", candidate->width,
                      " height=", candidate->height, " bottom=",
                      candidatePolarityName(candidate->bottomBandPolarity));
      return cat("configured master=", id, ' ', metadata,
                 " decision=", decision, " reasons=",
                 reasons.empty() ? "NONE" : reasons);
    });
  }

  for (const Diagnostic& diagnostic : result.diagnostics) {
    log.msg("candidate",
            cat("provider diagnostic filler=", fillerId,
                " code=", diagnostic.code,
                " message=", diagnostic.message));
  }
}

}  // namespace

SwapGenerationResult generateSwaps(
    const RepairWindow& window,
    const PlacementView& view,
    const DebugLog& log)
{
  SwapGenerationResult result;

  for (const InstanceId fillerId : window.editableFillers) {
    MasterCandidateResult candidates =
        view.getUsableMasterCandidates({fillerId});
    logCandidateProviderTrace(view, fillerId, candidates, log);
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
        log.msg("swapgen", [&] {
          return cat("reject filler=", fillerId, " master=",
                     candidate.masterId, " reason=", error);
        });
        continue;
      }
      result.swaps.push_back(*swap);
      ++emitted;
      // Deferred: this is the innermost loop of swap generation, so a
      // silenced transcript must not pay for the string.
      log.msg("swapgen", [&] {
        return cat("emit filler=", swap->instanceId, " row=", swap->rowId,
                   " span=", show(swap->span), " master ", swap->oldMasterId,
                   "(vt", swap->oldVt, ") -> ", swap->newMasterId, "(vt",
                   swap->newVt, ')');
      });
    }
    log.msg("swapgen",
            [&] {
              const PlacedInstance* filler = view.instance(fillerId);
              return filler != nullptr
                         ? cat("filler ", fillerId, " (row=", filler->rowId,
                               " x=", filler->x, ") -> ", emitted, " swap(s)")
                         : cat("filler ", fillerId,
                               " disappeared from PlacementView");
            });
  }

  log.msg("swapgen",
          cat("window L", window.level, ": ", window.editableFillers.size(),
              " editable filler(s) -> ", result.swaps.size(),
              " swap(s) total"));
  return result;
}

// --------------------------------------------------------------------------
// What the checker reported, turned into what the search needs: which rows,
// how much x, and who is a filler we may touch versus a cell we may not.
// --------------------------------------------------------------------------

namespace {

std::vector<RowId> sortedUniqueRows(const std::vector<RowId>& rows)
{
  std::vector<RowId> result = rows;
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

// Set equality of two row lists, ignoring order and duplicates -- the same
// question `sortedUniqueRows(a) == sortedUniqueRows(b)` answers, without
// materializing either side. Signature comparison runs once per candidate per
// violation, so allocating here allocates on the hottest loop of the search.
// Quadratic on purpose: a violation couples a handful of rows, and for those
// sizes a linear scan beats sorting, let alone two heap allocations.
bool sameRowSet(const std::vector<RowId>& a, const std::vector<RowId>& b)
{
  const auto coveredBy = [](const std::vector<RowId>& lhs,
                            const std::vector<RowId>& rhs) {
    for (const RowId row : lhs) {
      if (std::find(rhs.begin(), rhs.end(), row) == rhs.end()) {
        return false;
      }
    }
    return true;
  };
  return coveredBy(a, b) && coveredBy(b, a);
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
    const PlacementView& view,
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
    // anchor row and flag it.
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
  // report with the same ruleId/kind/relation/rows. Both default
  // to 0/nullopt when the checker leaves them unset, so this is a no-op for
  // layer-agnostic checkers.
  if (a.primaryLayer != b.primaryLayer || a.secondaryLayer != b.secondaryLayer) {
    return false;
  }
  if (!sameRowSet(a.rowIds, b.rowIds)) {
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
// The window: which fillers may be edited, and how much design the checker
// must be shown so its answer about them is trustworthy.
// --------------------------------------------------------------------------

namespace {

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

// Snaps a non-negative distance up to the next power-of-two multiple of
// `unit` (at least one unit).
DbCoord snapUpPow2(DbCoord distance, DbCoord unit)
{
  DbCoord snapped = std::max<DbCoord>(unit, 1);
  while (snapped < distance) {
    snapped *= 2;
  }
  return snapped;
}

// Why the guard is rounded rather than fitted.
//
// The guard IS the question -- "check this region" -- so two candidates only
// count as the same question if their guards match. Growth adds a couple of
// fillers at a time, so a guard fitted to the window moves a little at every
// step, and every answer already in hand stops matching. Measured on the
// no-solution path, 90% of all checker calls were an overlay we had already
// asked about, under a guard a few DBU different.
//
// So each side is pushed out to a power-of-two distance from the anchor --
// the one point that cannot move during a repair:
//
//     window grows:  |--|  |---|  |----|  |------|  |-------|
//     guard snaps:   |------------|       |----------------------|
//                    (one guard for several steps, then a jump)
//
// 33 steps became 5 distinct guards. Rounding OUTWARD is always safe: a
// bigger region can only remove edge artifacts from the checker's snapshot,
// never create them.
XInterval quantizeGuard(XInterval guard, DbCoord anchorX, DbCoord siteWidth)
{
  const DbCoord unit = std::max<DbCoord>(siteWidth, 1);
  const DbCoord left = std::max<DbCoord>(anchorX - guard.xl, 0);
  const DbCoord right = std::max<DbCoord>(guard.xh - anchorX, 0);
  // x is core-left-relative, so 0 is a known edge: snapping must not walk off
  // it. Overshooting the right edge is harmless (those columns are empty and
  // Grid::gridPixel bounds-checks), and the view exposes no core width to
  // clamp against.
  return XInterval{
      std::max<DbCoord>(anchorX - snapUpPow2(left, unit), 0),
      anchorX + snapUpPow2(right, unit)};
}

RepairWindow finalizeWindow(int level,
                            const std::set<RowId>& rowSet,
                            const std::set<InstanceId>& editable,
                            const std::set<InstanceId>& bridge,
                            XInterval x,
                            DbCoord anchorX,
                            const PlacementView& view,
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

  // [PORT-ADAPT] Vertical reach of the guard, and an assumption worth
  // checking against your rule deck: inter-row rules reach ONE row boundary,
  // so a violation our edit could cause lives at most one row outside the
  // window, and two rows of guard covers it with a margin. The horizontal
  // reach is not guessed like this -- it comes from the checker's own
  // getMaxRuleValue() (see FillerRepairEngine.cpp). If any implant rule of
  // yours spans more than one row boundary, this must grow to match, and
  // nothing will tell you: the checker would simply never be shown the row
  // where the new violation appeared.
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
  window.guardRegion = Region{quantizeGuard(guardX, anchorX, view.siteWidth()),
                              guardRows.front(),
                              guardRows.back()};

  log.msg("window",
          cat(level == 0 ? cat("start") : cat("grown x", level),
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

RepairWindow buildWindow(const TargetPlace& anchor,
                         const std::vector<NormalizedViolation>& violations,
                         const PlacementView& view,
                         DbCoord ruleDistance,
                         const DebugLog& log)
{
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

  // --- L0 seed set: violation filler participants, plus the
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

  // --- Bridge fillers (default-mandatory): fillers touching the
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

  return finalizeWindow(0, rowSet, editable, bridge, x, anchor.x, view, log);
}

RepairWindow expandWindowAdaptive(const RepairWindow& current,
                                  const TargetPlace& anchor,
                                  const std::vector<Violation>& blocking,
                                  const PlacementView& view,
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
                        anchor.x,
                        view,
                        log);
}

// --------------------------------------------------------------------------
// Trying order. The bridge filler first, then the anchor's own VT, then the
// neighbourhood majority -- most repairs are found in the first few tries.
// --------------------------------------------------------------------------

namespace {

// Which VT do this filler's neighbours mostly have? A good guess at what to
// recolour it to, since matching your neighbours is what merges runs.
//
// Votes are weighted by how much implant actually faces the filler. A
// neighbour beside it in the same row shares both half-row bands, so it gets
// two votes; a neighbour one row up or down only meets it across a single
// band boundary, so it gets one. Ties go to the lower VT id, for determinism.
VtId neighborMajorityVt(const PlacementView& view, const PlacedInstance& inst)
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

// Filler-level ordering key: which fillers the searcher combines
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
    // stays reachable in every subset the filler joins.
    const PlacedInstance* filler = view.instance(id);
    const VtId majorityVt =
        filler != nullptr ? neighborMajorityVt(view, *filler) : kUnknownVt;
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
// Candidates: one swap, then two at a time, then three, ... bounded by the
// member caps so a wide window cannot explode.
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
                                  const DebugLog& log,
                                  const std::vector<InstanceId>& freshFillers)
{
  EnumerationPlan plan;
  if (ranked.empty() || budget <= 0) {
    log.msg("enumerate",
            cat("skip enumeration: domains=", ranked.size(),
                " budget=", budget));
    return plan;
  }

  // Rank-indexed mirror of `freshFillers`, so the leaf test is one array read
  // rather than a search. See the header for why skipping the rest is exact.
  const bool incremental = !freshFillers.empty();
  std::vector<char> rankIsFresh;
  if (incremental) {
    rankIsFresh.assign(ranked.size(), 0);
    for (size_t i = 0; i < ranked.size(); ++i) {
      rankIsFresh[i] = std::binary_search(freshFillers.begin(),
                                          freshFillers.end(),
                                          ranked[i].instanceId)
                           ? 1
                           : 0;
    }
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
  // Member cap counts FILLERS: size-s subsets draw from the first
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
  // The emitted count is bounded by the budget, so the candidate list is
  // sized once instead of doubling its way there.
  plan.overlays.reserve(static_cast<size_t>(
      std::min<long long>(space, static_cast<long long>(budget))));
  bool budgetHit = false;
  int comboFresh = 0;      // fresh members in `combo`, maintained incrementally
  long long skippedAsAsked = 0;  // candidates the previous level already asked

  // Both recursions pass themselves as `self` rather than going through
  // std::function: this is the innermost loop of enumeration (millions of
  // calls on a wide window), and type erasure there costs an indirect call
  // per step for no benefit.

  // Cartesian product over the chosen fillers' domains, last filler's option
  // varying fastest, so the all-first-choice assignment (anchor-follow) leads.
  const auto emitProducts = [&](const auto& self, size_t k) -> void {
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
      self(self, k + 1);
      current.pop_back();
      if (budgetHit) {
        return;
      }
    }
  };

  // Lexicographic filler combinations of `size` within the rank prefix `cap`.
  const auto choose =
      [&](const auto& self, int size, int from, int cap) -> void {
    if (budgetHit) {
      return;
    }
    if (static_cast<int>(combo.size()) == size) {
      // Already emitted -- and answered -- at the previous level: its whole
      // Cartesian product is skipped, so no key is built and no oracle
      // question is repeated.
      if (incremental && comboFresh == 0) {
        // Count what its Cartesian product WOULD have been: those candidates
        // are covered (by the previous level), so completeness accounting
        // must not treat the level as truncated.
        long long product = 1;
        for (const int rank : combo) {
          product *= static_cast<long long>(ranked[rank].options.size());
        }
        skippedAsAsked += product;
        return;
      }
      emitProducts(emitProducts, 0);
      return;
    }
    for (int i = from; i < cap; ++i) {
      combo.push_back(i);
      if (incremental) {
        comboFresh += rankIsFresh[i];
      }
      self(self, size, i + 1, cap);
      if (incremental) {
        comboFresh -= rankIsFresh[i];
      }
      combo.pop_back();
      if (budgetHit) {
        return;
      }
    }
  };

  for (int size = 1; size <= maxSize && !budgetHit; ++size) {
    const size_t before = plan.overlays.size();
    const int cap = memberCap(size);
    choose(choose, size, 0, cap);
    // Per-size accounting makes it obvious when a large-window member cap or
    // the checker budget, rather than the legality oracle, removed candidates.
    log.msg("enumerate",
            cat("subsetSize=", size, " memberPrefix=", cap, '/',
                fillerTotal, " -> emitted=", plan.overlays.size() - before,
                budgetHit ? " (budget reached)" : ""));
  }
  // Reaching the budget on the final element is still a complete search.
  // Derive completeness from what was actually emitted so space == budget
  // cannot be mislabeled as truncated. Incrementally skipped candidates count
  // as covered: the previous level asked them under this same guard and the
  // answer was not clean, which cannot change (see the header).
  plan.complete
      = space <= budget
        && static_cast<long long>(plan.overlays.size()) + skippedAsAsked
               == space;

  if (incremental) {
    log.msg("enumerate",
            cat("incremental: fresh filler(s)=", freshFillers.size(), '/',
                fillerTotal, " -> skipped ", skippedAsAsked,
                " candidate(s) already answered at the previous level"));
  }
  log.msg("enumerate",
          cat(fillerTotal, " filler domain(s), ", optionTotal,
              " option(s), space=", space,
              plan.complete ? " (complete)" : " (truncated)", " -> ",
              plan.overlays.size(), " overlay candidate(s), maxSize=", maxSize));
  return plan;
}

// --------------------------------------------------------------------------
// Asking the checker. Accept only a candidate that removes every violation we
// were given and introduces none of its own -- measured against a baseline of
// the same region with nothing changed.
// --------------------------------------------------------------------------

namespace {

// A violation falls inside the repair window when its x overlaps the editable
// span and at least one of its rows is editable. Empty rows
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

OracleGate::OracleGate(const PlacementView& dataSource,
                       RepairOracle& oracle,
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
  original_classes_.reserve(originals_.size());
  for (const Violation& original : originals_) {
    original_classes_.push_back(signatureClass(original));
  }
}

bool OracleGate::runBaseline(const RepairWindow& window, int& budget)
{
  const Region& guard = window.guardRegion;
  const OverlayKey key = overlayKey(guard, {});  // empty overlay = baseline
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
  // Classes follow the baseline: it changes only when the guard does, so this
  // is paid once per distinct guard rather than once per candidate.
  baseline_classes_.clear();
  baseline_classes_.reserve(baseline_->violations.size());
  for (const Violation& v : baseline_->violations) {
    baseline_classes_.push_back(signatureClass(v));
  }
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
  // snapshot.
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
  //     input snapshot is assumed clean apart from the originals,
  //     so an unmatched baseline finding inside the repair window signals an
  //     inconsistent snapshot. Unmatched findings OUTSIDE the window are the
  //     allowed unrelated pre-existing halo.
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
  // Self-consistency both ways: isLegal must agree with whether the
  // result reports violations. `isLegal && violations non-empty` AND
  // `!isLegal && violations empty` (an unexplained illegal result, which the
  // real checker returns for blocking overlaps / off-grid / polarity) are both
  // rejected as not clean.
  summary.inconsistent = (result.isLegal != result.violations.empty());

  // Signature classes of this result, computed once instead of once per
  // (original, finding) and (finding, baseline) pair below.
  const size_t resultCount = result.violations.size();
  result_classes_scratch_.clear();
  result_classes_scratch_.reserve(resultCount);
  for (const Violation& v : result.violations) {
    result_classes_scratch_.push_back(signatureClass(v));
  }

  // Residual originals: match each original to a DISTINCT result finding, so
  // two originals cannot both claim the same one (one-to-one).
  {
    consumed_scratch_.assign(resultCount, 0);
    std::vector<char>& consumed = consumed_scratch_;
    for (size_t o = 0; o < originals_.size(); ++o) {
      const Violation& original = originals_[o];
      const SignatureClass& originalClass = original_classes_[o];
      for (size_t i = 0; i < resultCount; ++i) {
        if (!consumed[i] && originalClass == result_classes_scratch_[i]
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
  // baseline: one baseline finding absorbs at most one candidate
  // finding, so a second same-signature finding is correctly counted as new
  // (P/N bands + the one-site signature tolerance make duplicates real). Inside
  // the repair window a new violation always rejects; in the guard halo only
  // when related to this overlay.
  baseline_consumed_scratch_.assign(baseline_->violations.size(), 0);
  std::vector<char>& baselineConsumed = baseline_consumed_scratch_;
  for (size_t r = 0; r < resultCount; ++r) {
    const Violation& v = result.violations[r];
    const SignatureClass& vClass = result_classes_scratch_[r];
    bool preExisting = false;
    for (size_t i = 0; i < baseline_->violations.size(); ++i) {
      if (!baselineConsumed[i] && vClass == baseline_classes_[i]
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
      // Per-violation rule distance: a new violation from a
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

// [PORT-TUNE] Everything not already cached in this chunk goes out as ONE
// batch. Measured here, the alternative -- topping a short batch up with
// candidates from the next chunk so every call is full -- was evaluated and
// not done: it trades a fixed per-batch cost (one region scan) against
// speculatively checking candidates that an earlier answer may make
// unnecessary, and which way that lands depends on your thread count and on
// what one candidate actually costs. If batches show up in a real profile,
// that is the experiment to run. See fillerRepair/README.md "Search cost".
bool OracleGate::resolve(const Overlay* chunk,
                         const OverlayKey* chunkKeys,
                         std::size_t count,
                         const Region& guard,
                         int& budget,
                         std::vector<const OracleResult*>& out)
{
  out.assign(count, nullptr);

  // Send everything in the chunk that is not cached yet as one batch. The
  // single lookup per candidate here is the only one the search needs: hits
  // land in `out` directly, misses are filled in from the batch below.
  std::vector<OracleRequest> requests;
  std::vector<std::size_t> pending;  // chunk indices, parallel to `requests`
  requests.reserve(count);
  pending.reserve(count);
  int cachedInChunk = 0;
  int skippedForBudget = 0;
  const int budgetBefore = budget;
  for (std::size_t index = 0; index < count; ++index) {
    const auto cached = cache_.find(chunkKeys[index]);
    if (cached != cache_.end()) {
      ++cache_hits_;
      ++cachedInChunk;
      out[index] = &cached->second;
      continue;
    }
    if (budget <= 0) {
      ++skippedForBudget;  // stays nullptr: not evaluated
      continue;
    }
    OracleRequest request;
    request.requestId = next_request_id_++;
    request.targetPlace = anchor_;
    request.guardRegion = guard;
    request.fillerChanges = toFillerChanges(chunk[index], data_source_);
    requests.push_back(std::move(request));
    pending.push_back(index);
    --budget;
  }
  if (requests.empty()) {
    log_.msg("gate", [&] {
      return cat("batch avoided: chunk=", count, " cached=", cachedInChunk,
                 " skippedForBudget=", skippedForBudget);
    });
    return true;
  }

  log_.msg("gate", [&] {
    return cat("batch send: chunk=", count, " uncached=", requests.size(),
               " cached=", cachedInChunk,
               " skippedForBudget=", skippedForBudget, " guard=", show(guard),
               " budget ", budgetBefore, " -> ", budget);
  });
  std::vector<OracleResult> results =
      oracle_.checkPlaceWithOverlays(requests);
  ++batches_sent_;
  requests_sent_ += static_cast<int>(requests.size());

  // Protocol validation: one result per request, ids echo exactly once, no
  // unknown ids. Order must NOT matter -- map back by id.
  if (results.size() != requests.size()) {
    diagnostics_.push_back(makeDiag(
        Severity::Fatal, "CheckerProtocolError",
        cat("batch returned ", results.size(), " result(s) for ",
            requests.size(), " request(s)")));
    log_.msg("gate",
             cat("batch protocol error: results=", results.size(),
                 " requests=", requests.size()));
    return false;
  }
  // Non-const: each id is validated to appear exactly once, so the result it
  // names is MOVED into the cache rather than deep-copied (a result carries
  // its violation and diagnostic vectors).
  std::map<OracleRequestId, OracleResult*> byId;
  for (OracleResult& result : results) {
    if (!byId.emplace(result.requestId, &result).second) {
      diagnostics_.push_back(makeDiag(
          Severity::Fatal, "CheckerProtocolError",
          cat("duplicate requestId ", result.requestId)));
      log_.msg("gate",
               cat("batch protocol error: duplicate id=", result.requestId));
      return false;
    }
  }
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const auto it = byId.find(requests[i].requestId);
    if (it == byId.end()) {
      diagnostics_.push_back(makeDiag(
          Severity::Fatal, "CheckerProtocolError",
          cat("missing result for requestId ", requests[i].requestId)));
      log_.msg("gate",
               cat("batch protocol error: missing id=",
                   requests[i].requestId));
      return false;
    }
    const std::size_t index = pending[i];
    out[index] =
        &cache_.emplace(chunkKeys[index], std::move(*it->second))
             .first->second;
  }
  log_.msg("gate", [&] {
    return cat("batch recv: results=", results.size(),
               " cacheSize=", cache_.size(),
               " requestsTotal=", requests_sent_,
               " batchesTotal=", batches_sent_);
  });
  return true;
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
  std::vector<OverlayKey> chunkKeys;          // reused across chunks
  std::vector<const OracleResult*> answers;   // ditto
  while (next < candidates.size()) {
    const size_t chunkEnd =
        std::min(candidates.size(),
                 next + static_cast<size_t>(config_.batchSize));
    const size_t count = chunkEnd - next;
    // Identity is built ONCE per candidate, and resolve() hands back the
    // answers so this loop never repeats its cache lookup.
    chunkKeys.clear();
    chunkKeys.reserve(count);
    for (size_t i = next; i < chunkEnd; ++i) {
      chunkKeys.push_back(overlayKey(guard, candidates[i]));
    }
    if (!resolve(&candidates[next], chunkKeys.data(), count, guard, budget,
                 answers)) {
      sr.protocolError = true;
      return sr;
    }

    // Evaluate the chunk in enumeration order; first delta-clean wins.
    for (size_t i = next; i < chunkEnd; ++i) {
      const OracleResult* answer = answers[i - next];
      if (answer == nullptr) {
        sr.budgetExhausted = true;  // was not evaluated: out of budget
        continue;
      }
      const DeltaSummary summary = classify(*answer, candidates[i], window);
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
// The driver: search the window, and when nothing in it works, grow and
// search again.
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
  return window.level == 0 ? cat("start")
                           : cat("grown x", window.level);
}

}  // namespace

namespace internal {

RepairPlanner::RepairPlanner(
    const PlacementView& view,
    RepairOracle& oracle,
    RepairConfig config)
    : view_(view),
      oracle_(oracle),
      config_(config),
      log_(config.verbose)
{
}

FillerRepairResult RepairPlanner::repair(
    const FillerRepairRequest& request)
{
  FillerRepairResult result;

  // The overlay API is a pure query and must never call back into
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

  const std::vector<RowId>& viewRows = view_.rows();
  if (viewRows.empty() || view_.siteWidth() <= 0) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal,
        "InvalidPlacementView",
        cat("placement view has no usable grid: rows=", viewRows.size(),
            " siteWidth=", view_.siteWidth())));
    return result;
  }
  const PlacedInstance* currentTarget =
      view_.instance(request.targetPlace.instanceId);
  if (currentTarget == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal,
        "UnknownTarget",
        cat("target instance ", request.targetPlace.instanceId,
            " is absent from the placement view")));
    return result;
  }
  if (view_.masterInfo(currentTarget->masterId) == nullptr
      || view_.masterInfo(request.targetPlace.masterId) == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal,
        "UnknownTargetMaster",
        cat("target instance ", request.targetPlace.instanceId,
            " references unavailable current/replacement master metadata")));
    return result;
  }
  if (std::find(viewRows.begin(), viewRows.end(), request.targetPlace.rowId)
          == viewRows.end()
      || request.targetPlace.x < 0) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal,
        "InvalidTargetPlacement",
        cat("target placement is outside the view: row=",
            request.targetPlace.rowId, " x=", request.targetPlace.x)));
    return result;
  }

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
               " maxAdaptiveLevels=", config_.maxAdaptiveLevels,
               " budget/repair=", config_.checkerCallBudgetPerRepair));

  // Placement coverage is intentionally not checked here: the engine's
  // regional gate runs before this, and whole-design legality is
  // infrastructure's own gate.
  // Empty snapshot: nothing to repair is a success with no changes.
  if (request.violations.empty()) {
    result.hasSolution = true;
    result.diagnostics.push_back(makeDiag(
        Severity::Info, "EmptySnapshot", "no violations in initial snapshot"));
    log_.msg("planner",
             "empty violation snapshot -> hasSolution=true, 0 changes");
    return result;
  }

  // Stage 2: normalize the snapshot into signatures/footprints.
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

  // The adaptive window loop: search L0, then grow K fillers toward the
  // best non-clean candidate's blocking side until clean or an expansion
  // cutoff.
  OracleGate::SearchResult best;  // best non-clean across windows (diagnostics)
  // Definitive iff the LAST window we actually searched was fully enumerated
  // an earlier smaller window being complete does not prove the
  // later truncated window has no solution.
  bool lastSearchedDefinitive = false;
  RepairWindow window = buildWindow(
                                    request.targetPlace,
                                    violations,
                                    view_,
                                    ruleDistance,
                                    log_);

  // Previous level's search question, so enumeration can be incremental: the
  // guard is quantized and therefore repeats across most levels, and under a
  // repeated guard every combination without a newly editable filler is one
  // the previous level already asked. See enumerateOverlays in the header.
  Region searchedGuard;
  std::vector<InstanceId> searchedEditable;
  bool haveSearchedLevel = false;

  for (;;) {
    const std::string label = windowLabel(window);
    std::vector<Violation> blockingForExpansion = request.violations;
    bool currentDefinitive = false;

    // Per-repair ceiling across all adaptive levels. Checked before the
    // window is searched so exhaustion ends the search the same way an
    // expansion cutoff does -- truncated, not a baseline-gate failure.
    if (config_.checkerCallBudgetPerRepair > 0
        && gate.requestsSent() >= config_.checkerCallBudgetPerRepair) {
      result.diagnostics.push_back(makeDiag(
          Severity::Info, "RepairBudgetExhausted",
          cat("window ", label, " not searched: checker requests=",
              gate.requestsSent(), " reached checkerCallBudgetPerRepair=",
              config_.checkerCallBudgetPerRepair, " -> truncated")));
      log_.msg("planner",
               cat("per-repair checker budget ",
                   config_.checkerCallBudgetPerRepair,
                   " exhausted before ", label, " -> truncated"));
      lastSearchedDefinitive = false;
      break;
    }

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
        if (config_.checkerCallBudgetPerRepair > 0) {
          // Never let one window spend past the per-repair ceiling. The
          // remainder is > 0 here: the loop head just checked it.
          budget = std::min(budget,
                            config_.checkerCallBudgetPerRepair
                                - gate.requestsSent());
        }
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

        // Incremental only when the quantized guard did not move: a new guard
        // makes every candidate a new checker question again.
        std::vector<InstanceId> freshFillers;
        if (haveSearchedLevel && window.guardRegion == searchedGuard) {
          std::set_difference(window.editableFillers.begin(),
                              window.editableFillers.end(),
                              searchedEditable.begin(),
                              searchedEditable.end(),
                              std::back_inserter(freshFillers));
        }
        const EnumerationPlan plan =
            enumerateOverlays(ranked, config_, budget, log_, freshFillers);
        searchedGuard = window.guardRegion;
        searchedEditable = window.editableFillers;
        haveSearchedLevel = true;
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

  // No clean overlay anywhere: empty changes, explain why.
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
