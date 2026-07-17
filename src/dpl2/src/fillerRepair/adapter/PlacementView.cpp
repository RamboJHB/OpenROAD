// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "PlacementView.h"

#include <algorithm>

#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

namespace {

Orient toPlannerOrient(eUTL::PhysOrientation orientation)
{
  if (orientation == eUTL::PhysOrientationE::R180) return Orient::R180;
  if (orientation == eUTL::PhysOrientationE::MX) return Orient::MX;
  if (orientation == eUTL::PhysOrientationE::MY) return Orient::MY;
  return Orient::R0;
}

eUTL::PhysOrientation toUdmOrient(Orient orient)
{
  switch (orient) {
    case Orient::R180: return eUTL::PhysOrientationE::R180;
    case Orient::MX: return eUTL::PhysOrientationE::MX;
    case Orient::MY: return eUTL::PhysOrientationE::MY;
    case Orient::R0: break;
  }
  return eUTL::PhysOrientationE::R0;
}

bool supportedOrientation(eUTL::PhysOrientation orientation)
{
  return orientation == eUTL::PhysOrientationE::R0
         || orientation == eUTL::PhysOrientationE::R180
         || orientation == eUTL::PhysOrientationE::MX
         || orientation == eUTL::PhysOrientationE::MY;
}

// MUST match the checker's buildMasters predicate
// (isCoreFiller() || isPadFiller()).
bool isFillerMaster(const eLIB::PhysLibCell& cell)
{
  return cell.getType().isCoreFiller() || cell.getType().isPadFiller();
}

ViolationKind toKind(ipl::RuleSource source)
{
  return (source == ipl::RuleSource::Width
          || source == ipl::RuleSource::Lef58Width)
             ? ViolationKind::MinWidth
             : ViolationKind::MinSpacing;
}

// Final checker: Relationship is exactly {IntraRow, InterRow}.
ViolationRelation toRelation(ipl::Relationship relationship)
{
  return relationship == ipl::Relationship::InterRow
             ? ViolationRelation::InterRow
             : ViolationRelation::IntraRow;
}

}  // namespace

PlacementView::PlacementView(eUNL::PhysDesMgr* desMgr,
                             Grid* grid,
                             Network* network,
                             const ipl::ImplantLayerChecker* checker,
                             const dpl2::fillerSetting* fillerSetting)
    : PlacementView(desMgr, grid, network, checker, fillerSetting, Config())
{
}

PlacementView::PlacementView(eUNL::PhysDesMgr* desMgr,
                             Grid* grid,
                             Network* network,
                             const ipl::ImplantLayerChecker* checker,
                             const dpl2::fillerSetting* fillerSetting,
                             Config config)
    : des_mgr_(desMgr),
      grid_(grid),
      network_(network),
      checker_(checker),
      config_(config),
      log_(config.verbose)
{
  config_.repair.verbose = config_.repair.verbose || config_.verbose;
  if (desMgr == nullptr || network == nullptr || checker == nullptr) {
    addProblem(Severity::Fatal, "MissingDependency",
               "PlacementView needs desMgr, network and checker");
    return;
  }

  // --- rows: RowId = PhysRow iteration index over ALL rows (the checker's
  // convention); legal spans and uniformity checks cover non-pad rows only.
  struct RowFrame
  {
    DbCoord originX = 0;
    DbCoord yLo = 0;
    DbCoord yHi = 0;
    bool isPad = false;
  };
  std::vector<RowFrame> frames;
  {
    RowId rowId = 0;
    for (const eUNL::PhysRow& row : desMgr->getPhysRowIter()) {
      RowFrame frame;
      frame.isPad = row.getSite().getIsPad();
      frame.originX = row.getOrigin().getX().getStorage();
      frame.yLo = row.getOrigin().getY().getStorage();
      frame.yHi = (row.getOrigin().getY() + row.getSite().getHeight())
                      .getStorage();
      if (!frame.isPad) {
        const DbCoord width = row.getSite().getWidth().getStorage();
        const DbCoord height = row.getSite().getHeight().getStorage();
        if (site_width_ == 0) site_width_ = width;
        else if (site_width_ != width) {
          addProblem(Severity::Fatal, "NonUniformSiteWidth",
                     "non-pad rows do not share one site width");
        }
        if (row_height_ == 0) row_height_ = height;
        else if (row_height_ != height) {
          addProblem(Severity::Fatal, "NonUniformRowHeight",
                     "non-pad rows do not share one row height");
        }
        const eUTL::Rect bbox = row.getBbox();
        const DbCoord spanWidth = (bbox.getXH() - bbox.getXL()).getStorage();
        if (spanWidth <= 0 || width <= 0 || spanWidth % width != 0) {
          addProblem(Severity::Fatal, "InvalidRowSpan",
                     cat("row ", rowId, " has an invalid legal span"));
        }
        row_spans_[rowId] = XInterval{0, spanWidth};
      }
      frames.push_back(frame);
      ++rowId;
    }
  }
  if (site_width_ <= 0 || row_height_ <= 0 || row_spans_.empty()) {
    addProblem(Severity::Fatal, "MissingRowGeometry",
               "no usable standard-cell row geometry");
  }
  if (checker->siteWidth() != site_width_) {
    addProblem(Severity::Fatal, "CheckerSiteWidthMismatch",
               cat("checker site width ", checker->siteWidth(),
                   " differs from infrastructure ", site_width_));
  }
  // Planner windows/guards and the checker's inter-row comparisons both use
  // ONE x frame across rows; refuse rows with different origin X.
  for (size_t i = 0; i < frames.size(); ++i) {
    if (!frames[i].isPad && frames[i].originX != frames.front().originX
        && !frames.front().isPad) {
      addProblem(Severity::Fatal, "RowOriginMisaligned",
                 cat("row ", i, " origin X ", frames[i].originX,
                     " differs from row 0 origin X ",
                     frames.front().originX));
      break;
    }
  }
  row_list_.reserve(row_spans_.size());
  for (const auto& [id, span] : row_spans_) {
    row_list_.push_back(id);
  }

  // y -> row lookup; ties on yLo resolve to the FIRST row in row order,
  // mirroring the checker's linear first-match scan.
  struct RowRange
  {
    DbCoord yLo = 0;
    DbCoord yHi = 0;
    RowId rowId = 0;
  };
  std::vector<RowRange> rowRanges;
  rowRanges.reserve(frames.size());
  for (size_t i = 0; i < frames.size(); ++i) {
    rowRanges.push_back(
        RowRange{frames[i].yLo, frames[i].yHi, static_cast<RowId>(i)});
  }
  std::sort(rowRanges.begin(), rowRanges.end(),
            [](const RowRange& a, const RowRange& b) {
              return a.yLo != b.yLo ? a.yLo < b.yLo : a.rowId < b.rowId;
            });
  const auto rowContaining = [&rowRanges](DbCoord y) -> RowId {
    auto it = std::upper_bound(
        rowRanges.begin(), rowRanges.end(), y,
        [](DbCoord value, const RowRange& range) { return value < range.yLo; });
    if (it == rowRanges.begin()) {
      return -1;
    }
    --it;
    while (it != rowRanges.begin() && std::prev(it)->yLo == it->yLo) {
      --it;
    }
    return (y >= it->yLo && y < it->yHi) ? it->rowId : -1;
  };

  const auto heightInRows = [this](DbCoord height) {
    return row_height_ > 0
               ? std::max<DbCoord>((height + row_height_ - 1) / row_height_, 1)
               : 1;
  };

  // --- implant metadata: VT family / band polarity per master, derived from
  // the master's implant shapes exactly like the checker (layer identity via
  // tech layer NAME matched against checker->getLayers(), band anchored at
  // the bottommost implant rect -- the rebuildMasterShapes rule).
  const eLIB::TechLib& tech = desMgr->getTopTech();
  const auto implantLayerOf =
      [&](eLIB::TechLayerRelativeID relId) -> const ipl::ImplantLayer* {
    const std::string name = tech.getTechLayer(relId).getName();
    for (const ipl::ImplantLayer& layer : checker->getLayers()) {
      if (layer.name == name) {
        return &layer;
      }
    }
    return nullptr;
  };

  for (const auto& masterPtr : network->getMasters()) {
    const Master* nm = masterPtr.get();
    if (nm == nullptr || nm->getPhysLibCell() == nullptr) {
      continue;
    }
    const eLIB::PhysLibCell* cell = nm->getPhysLibCell();
    const MasterId id = static_cast<MasterId>(nm->getId());

    MasterInfo info;
    info.id = id;
    info.width = cell->getWidth().getStorage();
    info.height = heightInRows(cell->getHeight().getStorage());
    info.isFiller = isFillerMaster(*cell);

    // VT/polarity from implant RECT shapes (R0 frame).
    DbCoord bottomYl = 0;
    bool haveBottom = false;
    for (const auto& obs : cell->getObstruction()) {
      const auto& shapes = obs.getShapes(eUTL::PhysOrientationE::R0);
      for (const auto& [layerRelId, shapeVec] : shapes) {
        const ipl::ImplantLayer* layer = implantLayerOf(layerRelId);
        if (layer == nullptr) {
          continue;
        }
        for (const auto& techShape : shapeVec) {
          if (techShape.getType() != eLIB::TechShape::RECT) {
            continue;
          }
          if (info.vt == kUnknownVt
              && layer->family != ipl::Family::Unknown) {
            info.vt = static_cast<VtId>(layer->family);
          }
          const DbCoord yl = techShape.getRect().getYL().getStorage();
          if (!haveBottom || yl < bottomYl) {
            haveBottom = true;
            bottomYl = yl;
            info.bottomBandPolarity = layer->polarity == ipl::Polarity::P
                                          ? BandPolarity::P
                                          : BandPolarity::N;
          }
        }
      }
    }

    masters_[id] = info;
    master_lib_ids_[id] = cell->getLibCellId();
  }

  // --- candidate universe: fillerSetting only, resolved to Network master
  // ids. Entries the Network does not know cannot be validated by the
  // checker either (it builds masters from the Network) -> Warning + skip.
  if (fillerSetting != nullptr) {
    for (const eLIB::PhysLibCell* cell : fillerSetting->getFillerMasters()) {
      if (cell == nullptr) {
        continue;
      }
      const int id = network->getMasterId(cell->getLibCellId());
      if (id < 0) {
        addProblem(Severity::Warning, "ConfiguredMasterNotInNetwork",
                   cat("configured filler master libCell ",
                       static_cast<int>(cell->getLibCellId().getIndexValue()),
                       " is not in the Network -> excluded from candidates"));
        continue;
      }
      const auto it = masters_.find(static_cast<MasterId>(id));
      if (it == masters_.end() || !it->second.isFiller) {
        addProblem(Severity::Fatal, "ConfiguredMasterNotFiller",
                   cat("configured master ", id, " is not a filler master"));
        continue;
      }
      filler_master_ids_.push_back(static_cast<MasterId>(id));
    }
    std::sort(filler_master_ids_.begin(), filler_master_ids_.end());
    filler_master_ids_.erase(
        std::unique(filler_master_ids_.begin(), filler_master_ids_.end()),
        filler_master_ids_.end());
  }

  // --- placed instances: Network nodes with the checker's exact filters and
  // frame (state from the PhysCell, x relative to the row origin).
  for (const auto& nodePtr : network->getNodes()) {
    const Node* node = nodePtr.get();
    if (node == nullptr || node->getMaster() == nullptr
        || node->getMaster()->getPhysLibCell() == nullptr) {
      continue;
    }
    const eLIB::PhysLibCell* cell = node->getMaster()->getPhysLibCell();
    const eUNL::LeafCellID lcId = node->getDbInst();
    const eUNL::PhysCell physCell = desMgr->getPhysCell(lcId);
    if (!physCell.isValid()) {
      continue;
    }
    const eUNL::PhysObjStatus status = physCell.getStatus();
    if (status != eUNL::PhysObjStatus::PLACED
        && status != eUNL::PhysObjStatus::LOC_FIXED) {
      continue;
    }
    if (!supportedOrientation(physCell.getOrient())) {
      addProblem(Severity::Fatal, "UnsupportedOrientation",
                 cat("node ", node->getId(), " has unsupported orientation"));
      continue;
    }
    const eUTL::Point2D origin = physCell.getOrigin();
    const RowId rowId = rowContaining(origin.getY().getStorage());
    if (rowId < 0) {
      addProblem(Severity::Fatal, "NodeOutsideRows",
                 cat("node ", node->getId(), " is not in any row"));
      continue;
    }
    const DbCoord xOffset =
        origin.getX().getStorage() - frames[static_cast<size_t>(rowId)].originX;
    if (xOffset < 0) {
      addProblem(Severity::Fatal, "NodeLeftOfRowOrigin",
                 cat("node ", node->getId(), " lies left of its row origin"));
      continue;
    }

    const MasterId masterId = static_cast<MasterId>(node->getMaster()->getId());
    const MasterInfo* info = masterInfo(masterId);
    if (info == nullptr) {
      addProblem(Severity::Fatal, "UnknownMaster",
                 cat("node ", node->getId(), " references master ", masterId));
      continue;
    }
    const bool isFiller = isFillerMaster(*cell);
    if (node->isFiller() != isFiller) {
      addProblem(Severity::Fatal, "FillerClassificationMismatch",
                 cat("node ", node->getId(),
                     " filler flag disagrees with master"));
      continue;
    }

    const InstanceId id = static_cast<InstanceId>(node->getId());
    PlacedInstance placed{id,
                          masterId,
                          rowId,
                          xOffset,
                          toPlannerOrient(physCell.getOrient()),
                          isFiller};
    if (placed.isFiller && info->vt == kUnknownVt) {
      addProblem(Severity::Warning, "FillerWithoutVt",
                 cat("placed filler ", id, " uses master ", masterId,
                     " without implant VT metadata -> not swappable"));
    }
    instances_[id] = placed;
    udm_refs_[id] = UdmRef{lcId, cell->getLibCellId(), origin.getX(),
                           origin.getY()};
    for (DbCoord offset = 0; offset < std::max<DbCoord>(info->height, 1);
         ++offset) {
      PlacedInstance rowCopy = placed;
      rowCopy.rowId = rowId + static_cast<RowId>(offset);
      if (rowCopy.rowId >= static_cast<RowId>(frames.size())) {
        addProblem(Severity::Fatal, "MultiRowOutsideRows",
                   cat("node ", node->getId(),
                       " extends outside legal rows"));
        break;
      }
      rowCopy.x = origin.getX().getStorage()
                  - frames[static_cast<size_t>(rowCopy.rowId)].originX;
      by_row_[rowCopy.rowId].push_back(rowCopy);
    }
  }
  for (auto& [rowId, list] : by_row_) {
    std::sort(list.begin(), list.end(),
              [](const PlacedInstance& a, const PlacedInstance& b) {
                return a.x != b.x ? a.x < b.x : a.id < b.id;
              });
  }

  // --- default snapshot halo: 2x the max implant WIDTH/SPACING from the
  // tech -- the same values the checker builds its rules from.
  {
    DbCoord maxRule = 0;
    for (const eLIB::TechLayer& layer : tech.getLayerIter()) {
      if (!layer.isImplant()) {
        continue;
      }
      maxRule = std::max<DbCoord>(maxRule, layer.getWidth().getStorage());
      maxRule = std::max<DbCoord>(maxRule, layer.getMinSpacing().getStorage());
    }
    default_halo_x_ = 2 * maxRule;
  }

  log_.msg("adapter",
           cat("placement view: ", instances_.size(), " node(s), ",
               masters_.size(), " master(s), ", filler_master_ids_.size(),
               " configured filler master(s), siteWidth=", site_width_,
               " defaultHaloX=", default_halo_x_));
}

bool PlacementView::isReady() const
{
  return std::none_of(
      setup_diagnostics_.begin(), setup_diagnostics_.end(),
      [](const Diagnostic& d) { return d.severity == Severity::Fatal; });
}

void PlacementView::addProblem(Severity severity,
                               const std::string& code,
                               const std::string& message)
{
  setup_diagnostics_.push_back(makeDiag(severity, code, message));
  log_.msg("adapter", cat(code, ": ", message));
}

XInterval PlacementView::rowLegalSpan(RowId rowId) const
{
  const auto it = row_spans_.find(rowId);
  return it == row_spans_.end() ? XInterval{} : it->second;
}

const std::vector<PlacedInstance>& PlacementView::instancesInRow(
    RowId rowId) const
{
  const auto it = by_row_.find(rowId);
  return it == by_row_.end() ? emptyInstances() : it->second;
}

const PlacedInstance* PlacementView::instance(InstanceId id) const
{
  const auto it = instances_.find(id);
  return it == instances_.end() ? nullptr : &it->second;
}

const MasterInfo* PlacementView::masterInfo(MasterId id) const
{
  const auto it = masters_.find(id);
  return it == masters_.end() ? nullptr : &it->second;
}

SiteCoverageResult PlacementView::checkSiteCoverage(const DebugLog& log) const
{
  std::call_once(coverage_once_, [&] {
    coverage_cache_ = fillerRepair::PlacementView::checkSiteCoverage(log);
  });
  return coverage_cache_;
}

// --- oracle: direct calls into the final ipl checker -----------------------

ipl::CheckRequest PlacementView::toCheckRequest(const TargetPlace& place) const
{
  ipl::CheckRequest request;
  request.instanceId = static_cast<ipl::InstanceId>(place.instanceId);
  request.masterId = static_cast<ipl::MasterId>(place.masterId);
  request.rowId = static_cast<ipl::RowId>(place.rowId);
  request.colId = static_cast<ipl::ColId>(
      site_width_ > 0 ? place.x / site_width_ : 0);
  request.orientation = toUdmOrient(place.orientation);
  return request;
}

::Rect PlacementView::toGuardRect(const Region& region) const
{
  // The checker's isInGuard uses the synthetic frame y = rowId * rowHeight;
  // (rowHi+1)*rowHeight - 1 keeps a touching adjacent row out.
  const DbCoord yl = static_cast<DbCoord>(region.rowLo) * row_height_;
  const DbCoord yh =
      static_cast<DbCoord>(region.rowHi + 1) * row_height_ - 1;
  return ::Rect(eUTL::UvDist(static_cast<int64_t>(region.x.xl)),
                eUTL::UvDist(static_cast<int64_t>(yl)),
                eUTL::UvDist(static_cast<int64_t>(region.x.xh)),
                eUTL::UvDist(static_cast<int64_t>(yh)));
}

Violation PlacementView::toPlannerViolation(const ipl::Violation& v,
                                            InstanceId targetInstance) const
{
  Violation out;
  out.ruleId = v.ruleId;
  out.kind = toKind(v.ruleSource);
  out.relation = toRelation(v.relationship);
  out.primaryLayer = static_cast<LayerId>(v.primaryLayer);
  if (v.secondaryLayer) {
    out.secondaryLayer = static_cast<LayerId>(*v.secondaryLayer);
  }
  out.xWindow = XInterval{static_cast<DbCoord>(v.xWindow.xl),
                          static_cast<DbCoord>(v.xWindow.xh)};
  out.measuredValue = static_cast<DbCoord>(v.measuredValue);
  out.requiredValue = static_cast<DbCoord>(v.requiredValue);
  out.rowIds.reserve(v.rowIds.size());
  for (const ipl::RowId rowId : v.rowIds) {
    out.rowIds.push_back(static_cast<RowId>(rowId));
  }
  // Participants synthesized from the instance ids (Node ids) via this view.
  for (const ipl::InstanceId id : v.instances) {
    ViolationParticipant p;
    p.instanceId = static_cast<InstanceId>(id);
    p.isTarget = p.instanceId == targetInstance;
    if (const PlacedInstance* inst = instance(p.instanceId)) {
      p.masterId = inst->masterId;
      p.rowId = inst->rowId;
      const MasterInfo* master = masterInfo(inst->masterId);
      const DbCoord width = master != nullptr ? master->width : 0;
      p.xRange = XInterval{inst->x, inst->x + width};
      p.isFiller = inst->isFiller;
    }
    out.participants.push_back(p);
  }
  return out;
}

FillerCellRecord PlacementView::toFillerCellRecord(
    const FillerChange& change) const
{
  FillerCellRecord record{};
  record.op_ = dpl2::OpType::Replace;
  const auto refIt = udm_refs_.find(change.instanceId);
  if (refIt != udm_refs_.end()) {
    record.cell_id_ = refIt->second.cellId;
    record.orig_lib_cell_ = refIt->second.libCellId;
    record.origin_x_ = refIt->second.originX;
    record.origin_y_ = refIt->second.originY;
  }
  const auto libIt = master_lib_ids_.find(change.newMasterId);
  if (libIt != master_lib_ids_.end()) {
    record.new_lib_cell_ = libIt->second;
  }
  return record;
}

CheckResult PlacementView::checkPlaceWithOverlay(
    const OverlayCheckRequest& request)
{
  std::vector<CheckResult> results = checkPlaceWithOverlays({request});
  return results.empty() ? CheckResult{} : std::move(results.front());
}

std::vector<CheckResult> PlacementView::checkPlaceWithOverlays(
    const std::vector<OverlayCheckRequest>& requests)
{
  std::vector<CheckResult> results(requests.size());
  if (requests.empty()) {
    return results;
  }

  // One (target, guard) + N candidates per batch, by engine construction; a
  // mixed batch is a protocol error.
  const OverlayCheckRequest& first = requests.front();
  for (const OverlayCheckRequest& request : requests) {
    const bool same =
        request.targetPlace.instanceId == first.targetPlace.instanceId
        && request.targetPlace.masterId == first.targetPlace.masterId
        && request.targetPlace.rowId == first.targetPlace.rowId
        && request.targetPlace.x == first.targetPlace.x
        && request.targetPlace.orientation == first.targetPlace.orientation
        && request.guardRegion.x.xl == first.guardRegion.x.xl
        && request.guardRegion.x.xh == first.guardRegion.x.xh
        && request.guardRegion.rowLo == first.guardRegion.rowLo
        && request.guardRegion.rowHi == first.guardRegion.rowHi;
    if (!same) {
      for (size_t i = 0; i < requests.size(); ++i) {
        results[i].requestId = requests[i].requestId;
        results[i].status = CheckStatus::CheckerError;
        results[i].diagnostics.push_back(makeDiag(
            Severity::Fatal, "CheckerProtocolError",
            "mixed (targetPlace, guardRegion) in one overlay batch"));
      }
      return results;
    }
  }

  const ipl::CheckRequest target = toCheckRequest(first.targetPlace);
  const ::Rect guard = toGuardRect(first.guardRegion);
  std::vector<ipl::FillerChanges> changes;
  changes.reserve(requests.size());
  for (const OverlayCheckRequest& request : requests) {
    ipl::FillerChanges list;
    list.reserve(request.fillerChanges.size());
    for (const FillerChange& change : request.fillerChanges) {
      list.push_back(toFillerCellRecord(change));
    }
    changes.push_back(std::move(list));
  }

  std::vector<ipl::CheckResult> raw;
  {
    // Serialize the checker (its const overlay path mutates internal ids).
    std::lock_guard<std::mutex> lock(checker_mutex_);
    raw = checker_->checkPlaceWithOverlays(target, guard, changes);
  }
  log_.msg("adapter",
           cat("overlay batch: ", changes.size(), " candidate(s) -> ",
               raw.size(), " result(s)"));

  // The final checker prepends its PERSISTENT init diagnostics to every
  // result and folds them into isLegal. Strip that prefix so only
  // request-specific findings drive the candidate status; otherwise a single
  // benign init diagnostic (e.g. missing_rule_parameter on an unused layer)
  // would make every candidate permanently illegal.
  const size_t initDiagCount = checker_->getDiags().size();

  for (size_t i = 0; i < requests.size(); ++i) {
    CheckResult& out = results[i];
    out.requestId = requests[i].requestId;  // order IS the correlation
    if (i >= raw.size()) {
      out.status = CheckStatus::CheckerError;
      out.diagnostics.push_back(makeDiag(
          Severity::Fatal, "CheckerProtocolError",
          cat("checker returned ", raw.size(), " result(s) for ",
              changes.size(), " candidate(s)")));
      continue;
    }
    const ipl::CheckResult& r = raw[i];
    for (size_t d = initDiagCount; d < r.diagnostics.size(); ++d) {
      out.diagnostics.push_back(makeDiag(
          Severity::Warning, r.diagnostics[d].status,
          r.diagnostics[d].message));
    }
    out.violations.reserve(r.violations.size());
    for (const ipl::Violation& v : r.violations) {
      out.violations.push_back(
          toPlannerViolation(v, first.targetPlace.instanceId));
    }
    // Candidate-shape classification: request-level validation failures come
    // back as extra diagnostics with NO violations -> InvalidOverlay.
    if (out.violations.empty() && !r.isLegal && !out.diagnostics.empty()) {
      out.status = CheckStatus::InvalidOverlay;
      out.isLegal = false;
    } else {
      out.status = CheckStatus::Checked;
      out.isLegal = out.violations.empty() && out.diagnostics.empty();
    }
  }
  return results;
}

// --- repair entry -----------------------------------------------------------

Region PlacementView::snapshotGuard(const TargetPlace& target) const
{
  const MasterInfo* master = masterInfo(target.masterId);
  const DbCoord width = master != nullptr ? master->width : 0;
  const DbCoord heightRows =
      master != nullptr ? std::max<DbCoord>(master->height, 1) : 1;
  const DbCoord halo =
      config_.snapshotHaloX > 0 ? config_.snapshotHaloX : default_halo_x_;

  Region guard;
  guard.x = XInterval{target.x - halo, target.x + width + halo};
  const RowId minRow = row_list_.empty() ? 0 : row_list_.front();
  const RowId maxRow = row_list_.empty() ? 0 : row_list_.back();
  guard.rowLo = std::max<RowId>(
      minRow, target.rowId - static_cast<RowId>(config_.snapshotHaloRows));
  guard.rowHi = std::min<RowId>(
      maxRow,
      target.rowId + static_cast<RowId>(heightRows) - 1
          + static_cast<RowId>(config_.snapshotHaloRows));
  return guard;
}

RepairOutcome PlacementView::repair(eUNL::LeafCellID targetCell,
                                    const eLIB::PhysLibCell& newMaster)
{
  RepairOutcome result;
  if (!isReady()) {
    result.diagnostics = setup_diagnostics_;
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "AdapterNotReady",
        "infrastructure snapshot failed validation -> repair refused"));
    return result;
  }

  // 1) 100%-utility gate over this immutable snapshot.
  const SiteCoverageResult coverage = checkSiteCoverage(log_);
  if (!coverage.isFullUtility) {
    result.diagnostics = coverage.diagnostics;
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "NonFullUtility",
        cat(coverage.issues.size(),
            " coverage issue(s) -> placement precondition failed")));
    return result;
  }

  // 2) Resolve the target into the shared id space.
  const int targetId = network_->getNodeId(targetCell);
  const PlacedInstance* inst =
      targetId >= 0 ? instance(static_cast<InstanceId>(targetId)) : nullptr;
  if (inst == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "UnknownTarget",
        cat("leaf cell ", static_cast<int>(targetCell.getIndexValue()),
            " is not a placed node in this view")));
    return result;
  }
  const int newMasterId = network_->getMasterId(newMaster.getLibCellId());
  const MasterInfo* replacement =
      newMasterId >= 0 ? masterInfo(static_cast<MasterId>(newMasterId))
                       : nullptr;
  if (replacement == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "TargetMasterUnknown",
        "the target's new master is not in the Network master table"));
    return result;
  }
  const MasterInfo* oldMaster = masterInfo(inst->masterId);
  if (oldMaster == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "TargetMasterUnknown",
        "the target's current master is absent from the view"));
    return result;
  }
  if (inst->isFiller || oldMaster->isFiller || replacement->isFiller) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "TargetNotStdCell",
        "target and replacement master must both be standard cells"));
    return result;
  }
  if (oldMaster->width != replacement->width
      || oldMaster->height != replacement->height) {
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "TargetSizeMismatch",
        "target VT replacement must preserve width and height"));
    return result;
  }

  TargetPlace target;
  target.instanceId = inst->id;
  target.masterId = static_cast<MasterId>(newMasterId);
  target.rowId = inst->rowId;
  target.x = inst->x;
  target.orientation = inst->orientation;

  // 3) Initial snapshot: the new target place with ZERO filler changes.
  // Snapshot and every later engine baseline/candidate go through this same
  // object -> one consistent oracle worldview.
  OverlayCheckRequest snapshotRequest;
  snapshotRequest.requestId = 0;
  snapshotRequest.targetPlace = target;
  snapshotRequest.guardRegion = snapshotGuard(target);
  log_.msg("adapter",
           cat("snapshot: target inst=", target.instanceId, " newMaster=",
               target.masterId, " guard=",
               show(snapshotRequest.guardRegion)));
  const CheckResult snapshot = checkPlaceWithOverlay(snapshotRequest);
  if (snapshot.status != CheckStatus::Checked) {
    result.diagnostics = snapshot.diagnostics;
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "SnapshotFailed",
        "checker rejected the target-place snapshot request"));
    return result;
  }
  if (snapshot.violations.empty()) {
    result.hasSolution = true;  // legal as-is: empty change list
    result.diagnostics.push_back(makeDiag(
        Severity::Info, "NoRepairNeeded",
        "the new target place is already legal -> no filler changes"));
    return result;
  }

  // 4) Pure planner over this object (view AND oracle).
  FillerRepairRequest request;
  request.targetPlace = target;
  request.violations = snapshot.violations;
  FillerRepairEngine engine(*this, *this, config_.repair);
  const FillerRepairResult planned = engine.repair(request);

  result.hasSolution = planned.hasSolution;
  result.diagnostics.insert(result.diagnostics.end(),
                            planned.diagnostics.begin(),
                            planned.diagnostics.end());

  // 5) Checker/commit wire form.
  for (const FillerChange& change : planned.changes) {
    const FillerCellRecord record = toFillerCellRecord(change);
    if (!record.cell_id_.isValid()) {
      result.hasSolution = false;
      result.changes.clear();
      result.diagnostics.push_back(makeDiag(
          Severity::Fatal, "MappingLost",
          cat("accepted change (inst=", change.instanceId, " -> master=",
              change.newMasterId, ") has no UDM mapping")));
      return result;
    }
    result.changes.push_back(record);
  }
  return result;
}

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
