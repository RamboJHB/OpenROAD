// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "FillerRepairEngine.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

#include "Log.h"
#include "OracleGate.h"
#include "PlacementView.h"
#include "PlannerEngine.h"
#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"
#include "infrastructure/fillerSetting.h"
#include "infrastructure/network.h"

namespace dpl2 {
namespace fillerRepair {

namespace {

struct PlannedOutcome
{
  bool hasSolution = false;
  ipl::FillerChanges changes;
  std::vector<Diagnostic> diagnostics;
};

class ProductionView final : public PlacementView,
                             public ImplantOverlayChecker
{
 public:
  struct Config
  {
    RepairConfig repair;
    DbCoord snapshotHaloX = 0;
    int snapshotHaloRows = 1;
    bool verbose = false;
  };

  ProductionView(eUNL::PhysDesMgr* desMgr,
                 Grid* grid,
                 Network* network,
                 const ipl::ImplantLayerChecker* checker,
                 const fillerSetting* fillerSetting);
  ProductionView(eUNL::PhysDesMgr* desMgr,
                 Grid* grid,
                 Network* network,
                 const ipl::ImplantLayerChecker* checker,
                 const fillerSetting* fillerSetting,
                 Config config);

  bool isReady() const;
  const std::vector<Diagnostic>& setupDiagnostics() const
  {
    return setup_diagnostics_;
  }
  PlannedOutcome repair(eUNL::LeafCellID targetCell,
                        const eLIB::PhysLibCell& newMaster);

  const std::vector<RowId>& rows() const override { return row_list_; }
  XInterval rowLegalSpan(RowId rowId) const override;
  DbCoord siteWidth() const override { return site_width_; }
  const std::vector<PlacedInstance>& instancesInRow(RowId rowId) const override;
  const PlacedInstance* instance(InstanceId id) const override;
  const MasterInfo* masterInfo(MasterId id) const override;
  const std::vector<MasterId>& fillerMasterIds() const override
  {
    return filler_master_ids_;
  }
  CheckResult checkPlaceWithOverlay(const OverlayCheckRequest& request) override;
  std::vector<CheckResult> checkPlaceWithOverlays(
      const std::vector<OverlayCheckRequest>& requests) override;

 private:
  void addProblem(Severity severity,
                  const std::string& code,
                  const std::string& message);
  ipl::CheckRequest toCheckRequest(const TargetPlace& place) const;
  ::Rect toGuardRect(const Region& region) const;
  Violation toPlannerViolation(const ipl::Violation& violation,
                               InstanceId targetInstance) const;
  FillerCellRecord toFillerCellRecord(const FillerChange& change) const;
  Region snapshotGuard(const TargetPlace& target) const;

  Network* network_ = nullptr;
  const ipl::ImplantLayerChecker* checker_ = nullptr;
  Config config_;
  DebugLog log_;
  DbCoord site_width_ = 0;
  DbCoord row_height_ = 0;
  DbCoord default_halo_x_ = 0;
  std::map<RowId, XInterval> row_spans_;
  std::vector<RowId> row_list_;
  std::map<MasterId, MasterInfo> masters_;
  std::map<InstanceId, PlacedInstance> instances_;
  struct UdmRef
  {
    eUNL::LeafCellID cellId;
    eLIB::LibCellID libCellId;
    eUTL::UvDist originX;
    eUTL::UvDist originY;
  };
  std::map<InstanceId, UdmRef> udm_refs_;
  std::map<MasterId, eLIB::LibCellID> master_lib_ids_;
  std::map<RowId, std::vector<PlacedInstance>> by_row_;
  std::vector<MasterId> filler_master_ids_;
  std::vector<Diagnostic> setup_diagnostics_;
  mutable std::mutex checker_mutex_;
};


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

ProductionView::ProductionView(eUNL::PhysDesMgr* desMgr,
                               Grid* grid,
                               Network* network,
                               const ipl::ImplantLayerChecker* checker,
                               const dpl2::fillerSetting* fillerSetting)
    : ProductionView(desMgr,
                     grid,
                     network,
                     checker,
                     fillerSetting,
                     Config())
{
}

ProductionView::ProductionView(eUNL::PhysDesMgr* desMgr,
                             Grid* grid,
                             Network* network,
                             const ipl::ImplantLayerChecker* checker,
                             const dpl2::fillerSetting* fillerSetting,
                             Config config)
    : network_(network),
      checker_(checker),
      config_(config),
      log_(config.verbose)
{
  config_.repair.verbose = config_.repair.verbose || config_.verbose;
  if (desMgr == nullptr || grid == nullptr || network == nullptr
      || checker == nullptr) {
    addProblem(Severity::Fatal, "MissingDependency",
               "engine initialization needs desMgr, grid, network and checker");
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
  // ONE x frame across rows; refuse non-pad rows with different origin X.
  // The baseline is the FIRST NON-PAD row -- pad rows may sit anywhere and
  // must neither serve as the baseline nor be checked themselves.
  const auto firstNonPad =
      std::find_if(frames.begin(), frames.end(),
                   [](const RowFrame& frame) { return !frame.isPad; });
  if (firstNonPad != frames.end()) {
    for (size_t i = 0; i < frames.size(); ++i) {
      if (!frames[i].isPad && frames[i].originX != firstNonPad->originX) {
        addProblem(Severity::Fatal, "RowOriginMisaligned",
                   cat("row ", i, " origin X ", frames[i].originX,
                       " differs from first non-pad row origin X ",
                       firstNonPad->originX));
        break;
      }
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
        // Fatal: the checker validates candidates against Network masters, so
        // a configured master the Network never imported means the snapshot
        // was built against different inputs -- refuse instead of silently
        // shrinking the candidate universe.
        addProblem(Severity::Fatal, "ConfiguredMasterNotInNetwork",
                   cat("configured filler master libCell ",
                       static_cast<int>(cell->getLibCellId().getIndexValue()),
                       " is not in the Network"));
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

  log_.msg("engine",
           cat("placement view: ", instances_.size(), " node(s), ",
               masters_.size(), " master(s), ", filler_master_ids_.size(),
               " configured filler master(s), siteWidth=", site_width_,
               " defaultHaloX=", default_halo_x_));
}

bool ProductionView::isReady() const
{
  return std::none_of(
      setup_diagnostics_.begin(), setup_diagnostics_.end(),
      [](const Diagnostic& d) { return d.severity == Severity::Fatal; });
}

void ProductionView::addProblem(Severity severity,
                               const std::string& code,
                               const std::string& message)
{
  setup_diagnostics_.push_back(makeDiag(severity, code, message));
  log_.msg("engine", cat(code, ": ", message));
}

XInterval ProductionView::rowLegalSpan(RowId rowId) const
{
  const auto it = row_spans_.find(rowId);
  return it == row_spans_.end() ? XInterval{} : it->second;
}

const std::vector<PlacedInstance>& ProductionView::instancesInRow(
    RowId rowId) const
{
  const auto it = by_row_.find(rowId);
  return it == by_row_.end() ? emptyInstances() : it->second;
}

const PlacedInstance* ProductionView::instance(InstanceId id) const
{
  const auto it = instances_.find(id);
  return it == instances_.end() ? nullptr : &it->second;
}

const MasterInfo* ProductionView::masterInfo(MasterId id) const
{
  const auto it = masters_.find(id);
  return it == masters_.end() ? nullptr : &it->second;
}

// --- oracle: direct calls into the final ipl checker -----------------------

ipl::CheckRequest ProductionView::toCheckRequest(const TargetPlace& place) const
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

::Rect ProductionView::toGuardRect(const Region& region) const
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

Violation ProductionView::toPlannerViolation(const ipl::Violation& v,
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

FillerCellRecord ProductionView::toFillerCellRecord(
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

CheckResult ProductionView::checkPlaceWithOverlay(
    const OverlayCheckRequest& request)
{
  std::vector<CheckResult> results = checkPlaceWithOverlays({request});
  return results.empty() ? CheckResult{} : std::move(results.front());
}

std::vector<CheckResult> ProductionView::checkPlaceWithOverlays(
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
  log_.msg("engine",
           cat("overlay batch: ", changes.size(), " candidate(s) -> ",
               raw.size(), " result(s)"));

  // The final checker copies its PERSISTENT init diagnostics into every
  // result -- once directly (checkPlaceWithOverlay) and once more inside the
  // embedded region result (checkOverlayRegion) -- and folds them into
  // isLegal. Strip every leading repetition of that sequence so only
  // request-specific findings drive the candidate status; otherwise a single
  // benign init diagnostic (e.g. missing_rule_parameter on an unused layer)
  // would make every candidate permanently illegal.
  const auto& initDiags = checker_->getDiags();
  const auto requestDiagOffset =
      [&initDiags](const std::vector<ipl::Diagnostic>& diagnostics) {
        size_t offset = 0;
        while (!initDiags.empty()
               && offset + initDiags.size() <= diagnostics.size()) {
          bool matches = true;
          for (size_t k = 0; k < initDiags.size(); ++k) {
            if (diagnostics[offset + k].status != initDiags[k].status
                || diagnostics[offset + k].message != initDiags[k].message) {
              matches = false;
              break;
            }
          }
          if (!matches) {
            break;
          }
          offset += initDiags.size();
        }
        return offset;
      };

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
    for (size_t d = requestDiagOffset(r.diagnostics); d < r.diagnostics.size();
         ++d) {
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

Region ProductionView::snapshotGuard(const TargetPlace& target) const
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

PlannedOutcome ProductionView::repair(eUNL::LeafCellID targetCell,
                                      const eLIB::PhysLibCell& newMaster)
{
  PlannedOutcome result;
  if (!isReady()) {
    result.diagnostics = setup_diagnostics_;
    result.diagnostics.push_back(makeDiag(
        Severity::Fatal, "EngineNotReady",
        "infrastructure snapshot failed validation -> repair refused"));
    return result;
  }

  // 1) Resolve the target into the shared id space. Placement gap/overlap is
  // intentionally not checked here; opto owns precheck() sequencing.
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

  // 2) Initial snapshot: the new target place with ZERO filler changes.
  // Snapshot and every later engine baseline/candidate go through this same
  // object -> one consistent oracle worldview.
  OverlayCheckRequest snapshotRequest;
  snapshotRequest.requestId = 0;
  snapshotRequest.targetPlace = target;
  snapshotRequest.guardRegion = snapshotGuard(target);
  log_.msg("engine",
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

  // 3) Pure planner over this object (view AND oracle).
  FillerRepairRequest request;
  request.targetPlace = target;
  request.violations = snapshot.violations;
  internal::PlannerEngine planner(*this, *this, config_.repair);
  const FillerRepairResult planned = planner.repair(request);

  result.hasSolution = planned.hasSolution;
  result.diagnostics.insert(result.diagnostics.end(),
                            planned.diagnostics.begin(),
                            planned.diagnostics.end());

  // 4) Checker/commit wire form.
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

namespace {

const char* severityName(Severity severity)
{
  switch (severity) {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    case Severity::Fatal: return "fatal";
  }
  return "unknown";
}

ipl::Diagnostic toProductionDiagnostic(const Diagnostic& diagnostic)
{
  return ipl::Diagnostic{
      diagnostic.code,
      cat(severityName(diagnostic.severity), ": ", diagnostic.message)};
}

struct CoverageFinding
{
  const char* status = "Gap";
  int rowId = 0;
  int64_t xl = 0;
  int64_t xh = 0;
};

std::vector<CoverageFinding> findGapAndOverlap(eUNL::PhysDesMgr* desMgr,
                                               const Network* network)
{
  struct RowData
  {
    int id = 0;
    int64_t xl = 0;
    int64_t xh = 0;
    int64_t yl = 0;
    int64_t yh = 0;
    std::vector<std::pair<int64_t, int64_t>> spans;
  };

  std::vector<RowData> rows;
  int rowId = 0;
  for (const eUNL::PhysRow& row : desMgr->getPhysRowIter()) {
    if (!row.getSite().getIsPad()) {
      const eUTL::Rect bbox = row.getBbox();
      rows.push_back(RowData{rowId,
                             bbox.getXL().getStorage(),
                             bbox.getXH().getStorage(),
                             bbox.getYL().getStorage(),
                             bbox.getYH().getStorage(),
                             {}});
    }
    ++rowId;
  }

  for (const auto& nodePtr : network->getNodes()) {
    if (nodePtr == nullptr) {
      continue;
    }
    const eUNL::PhysCell cell = desMgr->getPhysCell(nodePtr->getDbInst());
    if (!cell.isValid()) {
      continue;
    }
    const eUNL::PhysObjStatus status = cell.getStatus();
    if (status != eUNL::PhysObjStatus::PLACED
        && status != eUNL::PhysObjStatus::LOC_FIXED) {
      continue;
    }
    const eUTL::Point2D origin = cell.getOrigin();
    const int64_t cellXl = origin.getX().getStorage();
    const int64_t cellXh = cellXl + cell.getPhysMaster().getWidth().getStorage();
    const int64_t cellYl = origin.getY().getStorage();
    const int64_t cellYh = cellYl + cell.getPhysMaster().getHeight().getStorage();
    for (RowData& row : rows) {
      if (cellYl >= row.yh || cellYh <= row.yl) {
        continue;
      }
      const int64_t clippedXl = std::max(cellXl, row.xl);
      const int64_t clippedXh = std::min(cellXh, row.xh);
      if (clippedXh > clippedXl) {
        row.spans.emplace_back(clippedXl, clippedXh);
      }
    }
  }

  std::vector<CoverageFinding> findings;
  for (RowData& row : rows) {
    std::vector<int64_t> cuts{row.xl, row.xh};
    for (const auto& span : row.spans) {
      cuts.push_back(span.first);
      cuts.push_back(span.second);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    std::vector<int64_t> starts;
    std::vector<int64_t> ends;
    starts.reserve(row.spans.size());
    ends.reserve(row.spans.size());
    for (const auto& span : row.spans) {
      starts.push_back(span.first);
      ends.push_back(span.second);
    }
    std::sort(starts.begin(), starts.end());
    std::sort(ends.begin(), ends.end());

    size_t nextStart = 0;
    size_t nextEnd = 0;
    int active = 0;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
      const int64_t segmentXl = cuts[i];
      const int64_t segmentXh = cuts[i + 1];
      while (nextEnd < ends.size() && ends[nextEnd] <= segmentXl) {
        --active;
        ++nextEnd;
      }
      while (nextStart < starts.size() && starts[nextStart] <= segmentXl) {
        ++active;
        ++nextStart;
      }
      if (segmentXh <= segmentXl || active == 1) {
        continue;
      }
      const char* findingStatus = active == 0 ? "Gap" : "Overlap";
      if (!findings.empty()
          && std::string(findings.back().status) == findingStatus
          && findings.back().rowId == row.id
          && findings.back().xh == segmentXl) {
        findings.back().xh = segmentXh;
      } else {
        findings.push_back(
            CoverageFinding{findingStatus, row.id, segmentXl, segmentXh});
      }
    }
  }
  return findings;
}

}  // namespace

class FillerRepairEngine::Impl
{
 public:
  Impl(Grid* grid, Network* network) : grid_(grid), network_(network) {}

  bool init(eUNL::PhysDesMgr* desMgr,
            const ipl::ImplantLayerChecker* checker,
            const fillerSetting* fillerSetting)
  {
    des_mgr_ = desMgr;
    checker_ = checker;
    filler_setting_ = fillerSetting;
    view_.reset();
    initialized_ = false;
    if (grid_ == nullptr || network_ == nullptr || des_mgr_ == nullptr
        || checker_ == nullptr || filler_setting_ == nullptr) {
      return false;
    }
    view_ = std::make_unique<ProductionView>(
        des_mgr_, grid_, network_, checker_, filler_setting_);
    initialized_ = view_->isReady();
    return initialized_;
  }

  ipl::CheckResult precheck() const
  {
    ipl::CheckResult result;
    if (!initialized_) {
      result.isLegal = false;
      result.diagnostics.push_back(
          {"precheck_not_initialized",
           "warning: init() must succeed before placement precheck"});
      return result;
    }
    const std::vector<CoverageFinding> findings
        = findGapAndOverlap(des_mgr_, network_);
    result.isLegal = findings.empty();
    for (const CoverageFinding& finding : findings) {
      result.diagnostics.push_back(
          {finding.status,
           cat("warning: placement ", finding.status, " row=", finding.rowId,
               " x=[", finding.xl, ",", finding.xh,
               ") -> opto must block mutation")});
    }
    return result;
  }

  RepairOutcome repair(eUNL::LeafCellID targetCell,
                       const eLIB::PhysLibCell& newMaster)
  {
    RepairOutcome result;
    if (repair_active_.exchange(true, std::memory_order_acq_rel)) {
      result.diagnostics.push_back(
          {"ReentrantRepair",
           "fatal: repair() re-entered on one FillerRepairEngine"});
      return result;
    }
    struct ActiveGuard
    {
      std::atomic<bool>& flag;
      ~ActiveGuard() { flag.store(false, std::memory_order_release); }
    } activeGuard{repair_active_};

    if (!initialized_) {
      result.diagnostics.push_back(
          {"engine_not_initialized",
           "fatal: init() must succeed before repair()"});
      return result;
    }
    const PlannedOutcome planned = view_->repair(targetCell, newMaster);
    result.hasSolution = planned.hasSolution;
    result.changes = planned.changes;
    result.diagnostics.reserve(planned.diagnostics.size());
    for (const Diagnostic& diagnostic : planned.diagnostics) {
      result.diagnostics.push_back(toProductionDiagnostic(diagnostic));
    }
    return result;
  }

 private:
  Grid* grid_ = nullptr;
  Network* network_ = nullptr;
  eUNL::PhysDesMgr* des_mgr_ = nullptr;
  const ipl::ImplantLayerChecker* checker_ = nullptr;
  const fillerSetting* filler_setting_ = nullptr;
  std::unique_ptr<ProductionView> view_;
  // True only after a fully successful init(); every public API fails closed
  // until then (a half-built snapshot must never answer queries).
  bool initialized_ = false;
  std::atomic<bool> repair_active_{false};
};

FillerRepairEngine::FillerRepairEngine(Grid* grid, Network* network)
    : impl_(std::make_unique<Impl>(grid, network))
{
}

FillerRepairEngine::~FillerRepairEngine() = default;

bool FillerRepairEngine::init(eUNL::PhysDesMgr* desMgr,
                              const ipl::ImplantLayerChecker* checker,
                              const fillerSetting* fillerSetting)
{
  return impl_->init(desMgr, checker, fillerSetting);
}

ipl::CheckResult FillerRepairEngine::precheck() const
{
  return impl_->precheck();
}

RepairOutcome FillerRepairEngine::repair(eUNL::LeafCellID targetCell,
                                         const eLIB::PhysLibCell& newMaster)
{
  return impl_->repair(targetCell, newMaster);
}

}  // namespace fillerRepair
}  // namespace dpl2
