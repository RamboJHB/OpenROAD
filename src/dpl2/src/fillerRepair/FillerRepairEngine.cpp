// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "FillerRepairEngine.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "FillerRepairPlanner.h"
#include "Log.h"
#include "OracleGate.h"
#include "PlacementPrecheck.h"
#include "PlacementView.h"
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
                 const std::vector<const eLIB::PhysLibCell*>& fillerMasters,
                 Config config);

  bool isReady() const;
  void setDebugLogging(bool enabled)
  {
    config_.verbose = enabled;
    config_.repair.verbose = enabled;
    log_.setEnabled(enabled);
  }
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

  // Grows `table` so `id` is a valid index (ids can exceed the presized
  // container counts only if the Network id spaces are not dense).
  template <typename T>
  static void ensureSlot(std::vector<T>& table, size_t id)
  {
    if (id >= table.size()) {
      table.resize(id + 1);
    }
  }

  Network* network_ = nullptr;
  const ipl::ImplantLayerChecker* checker_ = nullptr;
  Config config_;
  DebugLog log_;
  DbCoord site_width_ = 0;
  DbCoord row_height_ = 0;
  DbCoord default_halo_x_ = 0;
  // Dense tables over the dense id spaces (RowId = PhysRow iteration index,
  // InstanceId = Node::getId(), MasterId = Master::getId()); planner window
  // building and ranking hit these on every step, and std::map lookups were
  // measurable on large snapshots. All tables are immutable after
  // construction, so returned pointers stay valid for the view's lifetime.
  std::vector<XInterval> row_spans_;  // empty interval = pad row / no span
  std::vector<RowId> row_list_;
  std::vector<std::optional<MasterInfo>> masters_;
  std::vector<std::optional<PlacedInstance>> instances_;
  struct UdmRef
  {
    eUNL::LeafCellID cellId;
    eLIB::LibCellID libCellId;
    eUTL::UvDist originX;
    eUTL::UvDist originY;
  };
  std::vector<std::optional<UdmRef>> udm_refs_;
  std::vector<eLIB::LibCellID> master_lib_ids_;  // valid iff masters_[id]
  std::vector<std::vector<PlacedInstance>> by_row_;
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
                             const std::vector<const eLIB::PhysLibCell*>&
                                 fillerMasters,
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
        ensureSlot(row_spans_, static_cast<size_t>(rowId));
        row_spans_[rowId] = XInterval{0, spanWidth};
        row_list_.push_back(rowId);
      }
      frames.push_back(frame);
      ++rowId;
    }
    row_spans_.resize(frames.size());  // trailing pad rows -> empty spans
    by_row_.resize(frames.size());
  }
  if (site_width_ <= 0 || row_height_ <= 0 || row_list_.empty()) {
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

  masters_.resize(network->getMasters().size());
  master_lib_ids_.resize(network->getMasters().size());
  for (const auto& masterPtr : network->getMasters()) {
    const Master* nm = masterPtr.get();
    if (nm == nullptr || nm->getPhysLibCell() == nullptr) {
      continue;
    }
    const eLIB::PhysLibCell* cell = nm->getPhysLibCell();
    const MasterId id = static_cast<MasterId>(nm->getId());
    if (id < 0) {
      continue;
    }

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

    ensureSlot(masters_, static_cast<size_t>(id));
    ensureSlot(master_lib_ids_, static_cast<size_t>(id));
    masters_[id] = info;
    master_lib_ids_[id] = cell->getLibCellId();
  }

  // --- candidate universe: fillerSetting only, resolved to Network master
  // ids. Entries the Network does not know cannot be validated by the
  // checker either (it builds masters from the Network) -> Warning + skip.
  {
    for (const eLIB::PhysLibCell* cell : fillerMasters) {
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
      const MasterInfo* info = masterInfo(static_cast<MasterId>(id));
      if (info == nullptr || !info->isFiller) {
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
  if (filler_master_ids_.empty()) {
    // Empty allow list (or nothing usable in it) means repair could never
    // offer a swap -- fail init instead of failing every later repair.
    addProblem(Severity::Fatal, "NoConfiguredFillerMaster",
               "fillerSetting::getFillerMasters() yields no usable filler "
               "master");
  }

  // --- placed instances: Network nodes with the checker's exact filters and
  // frame (state from the PhysCell, x relative to the row origin).
  instances_.resize(network->getNodes().size());
  udm_refs_.resize(network->getNodes().size());
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

    // Frame-coherence gate. The checker mixes two frames: its init/track
    // pattern and our request wire use PhysRow ITERATION order with x
    // relative to the row origin, while its overlay scan resolves committed
    // neighbours and swapped fillers through Grid (gridSnapDownY/gridX:
    // non-pad rows by y, core-relative). The chain is only correct when the
    // two coincide for every placed node -- i.e. pad rows do not precede
    // standard rows, iteration order is y-sorted, and the shared row origin
    // is the core edge. On any other design the checker would compare mixed
    // frames SILENTLY; refuse the snapshot loudly here instead.
    const RowId gridRow = static_cast<RowId>(grid->gridSnapDownY(node).v);
    if (gridRow != rowId) {
      addProblem(Severity::Fatal, "RowFrameMismatch",
                 cat("node ", node->getId(), " is row ", rowId,
                     " by PhysRow iteration but row ", gridRow,
                     " by Grid y-snap; pad rows before standard rows or "
                     "non-y-sorted row iteration is not supported"));
      continue;
    }
    if (site_width_ > 0
        && static_cast<DbCoord>(grid->gridX(node).v)
               != xOffset / site_width_) {
      addProblem(Severity::Fatal, "ColFrameMismatch",
                 cat("node ", node->getId(), " is column ",
                     xOffset / site_width_, " by its row origin but column ",
                     static_cast<DbCoord>(grid->gridX(node).v),
                     " by the Grid core frame; the row origin X must equal "
                     "the core left edge"));
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
    ensureSlot(instances_, static_cast<size_t>(id));
    ensureSlot(udm_refs_, static_cast<size_t>(id));
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
  for (std::vector<PlacedInstance>& list : by_row_) {
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

  const auto placedCount = std::count_if(
      instances_.begin(), instances_.end(),
      [](const std::optional<PlacedInstance>& slot) {
        return slot.has_value();
      });
  const auto masterCount = std::count_if(
      masters_.begin(), masters_.end(),
      [](const std::optional<MasterInfo>& slot) { return slot.has_value(); });
  log_.msg("engine",
           cat("placement view: ", placedCount, " node(s), ",
               masterCount, " master(s), ", filler_master_ids_.size(),
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
  return rowId >= 0 && static_cast<size_t>(rowId) < row_spans_.size()
             ? row_spans_[rowId]
             : XInterval{};
}

const std::vector<PlacedInstance>& ProductionView::instancesInRow(
    RowId rowId) const
{
  return rowId >= 0 && static_cast<size_t>(rowId) < by_row_.size()
             ? by_row_[rowId]
             : emptyInstances();
}

const PlacedInstance* ProductionView::instance(InstanceId id) const
{
  return id >= 0 && static_cast<size_t>(id) < instances_.size()
                 && instances_[id].has_value()
             ? &*instances_[id]
             : nullptr;
}

const MasterInfo* ProductionView::masterInfo(MasterId id) const
{
  return id >= 0 && static_cast<size_t>(id) < masters_.size()
                 && masters_[id].has_value()
             ? &*masters_[id]
             : nullptr;
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
  const bool haveRef = change.instanceId >= 0
                       && static_cast<size_t>(change.instanceId)
                              < udm_refs_.size()
                       && udm_refs_[change.instanceId].has_value();
  if (haveRef) {
    const UdmRef& ref = *udm_refs_[change.instanceId];
    record.cell_id_ = ref.cellId;
    record.orig_lib_cell_ = ref.libCellId;
    record.origin_x_ = ref.originX;
    record.origin_y_ = ref.originY;
  }
  if (masterInfo(change.newMasterId) != nullptr) {
    record.new_lib_cell_ = master_lib_ids_[change.newMasterId];
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
  internal::FillerRepairPlanner planner(*this, *this, config_.repair);
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

// Grid legal domain for one precheck row plus the y-sorted lookup index.
// The Grid legal domain (rows, blockages, padding reservations) is immutable
// within one engine snapshot -- only PLACED cells move between precheck
// calls -- so this is computed once at init and reused per call.
struct PrecheckDomains
{
  struct Row
  {
    int id = 0;
    int64_t yl = 0;
    int64_t yh = 0;
    std::vector<XInterval> legalSpans;
  };
  std::vector<Row> rows;
  std::vector<size_t> order;      // row indices sorted by yl (ties by id)
  std::vector<int64_t> sortedYl;  // rows[order[i]].yl, for binary search
};

PrecheckDomains buildPrecheckDomains(const Grid* grid)
{
  // Grid is the infrastructure authority for placeable row sites. A valid
  // pixel belongs to a physical row and is not cut by a hard blockage/group
  // boundary; padding_reserved_by marks a halo/padding site where whitespace
  // is intentional. Only maximal runs satisfying both conditions require
  // exactly one placed-cell cover.
  PrecheckDomains domains;
  const eUTL::Rect core = grid->getCore();
  const int64_t coreXl = core.getXL().getStorage();
  const int64_t coreYl = core.getYL().getStorage();
  const int64_t siteWidth = grid->getSiteWidth().v;
  if (siteWidth <= 0) {
    return domains;
  }
  for (GridY y{0}; y < grid->getRowCount(); ++y) {
    PrecheckDomains::Row row;
    row.id = y.v;
    row.yl = coreYl + grid->gridYToDbu(y).v;
    row.yh = coreYl + grid->gridYToDbu(y + 1).v;

    bool inLegalSpan = false;
    int legalStart = 0;
    for (GridX x{0}; x < grid->getRowSiteCount(); ++x) {
      const Pixel* pixel = grid->gridPixel(x, y);
      const bool requiresCoverage
          = pixel != nullptr && pixel->is_valid
            && pixel->padding_reserved_by == nullptr;
      if (requiresCoverage && !inLegalSpan) {
        inLegalSpan = true;
        legalStart = x.v;
      } else if (!requiresCoverage && inLegalSpan) {
        row.legalSpans.push_back({coreXl + legalStart * siteWidth,
                                  coreXl + x.v * siteWidth});
        inLegalSpan = false;
      }
    }
    if (inLegalSpan) {
      row.legalSpans.push_back(
          {coreXl + legalStart * siteWidth,
           coreXl + grid->getRowSiteCount().v * siteWidth});
    }
    if (!row.legalSpans.empty() && row.yh > row.yl) {
      domains.rows.push_back(std::move(row));
    }
  }

  // y-sorted index over the rows so each node binary-searches its overlapped
  // rows instead of scanning all of them (real designs: 1e5..1e6 nodes x 1e3
  // rows made the full scan the dominant precheck cost).
  domains.order.resize(domains.rows.size());
  for (size_t i = 0; i < domains.order.size(); ++i) {
    domains.order[i] = i;
  }
  const std::vector<PrecheckDomains::Row>& rows = domains.rows;
  std::sort(domains.order.begin(), domains.order.end(),
            [&rows](size_t a, size_t b) {
              return rows[a].yl != rows[b].yl ? rows[a].yl < rows[b].yl
                                              : rows[a].id < rows[b].id;
            });
  domains.sortedYl.reserve(domains.order.size());
  for (const size_t idx : domains.order) {
    domains.sortedYl.push_back(rows[idx].yl);
  }
  return domains;
}

std::vector<internal::CoverageFinding> findGapAndOverlap(
    eUNL::PhysDesMgr* desMgr,
    const Network* network,
    const PrecheckDomains& domains)
{
  std::vector<std::vector<XInterval>> placedSpans(domains.rows.size());

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

    // First y-sorted position whose row could still overlap [cellYl, cellYh):
    // start at the first row with yl > cellYl and walk back over rows whose
    // span still crosses cellYl (at most one for non-overlapping rows).
    size_t pos = static_cast<size_t>(
        std::upper_bound(domains.sortedYl.begin(), domains.sortedYl.end(),
                         cellYl)
        - domains.sortedYl.begin());
    while (pos > 0 && domains.rows[domains.order[pos - 1]].yh > cellYl) {
      --pos;
    }
    for (; pos < domains.order.size()
           && domains.rows[domains.order[pos]].yl < cellYh;
         ++pos) {
      const size_t rowIdx = domains.order[pos];
      const PrecheckDomains::Row& row = domains.rows[rowIdx];
      if (cellYl >= row.yh || cellYh <= row.yl) {
        continue;
      }
      placedSpans[rowIdx].push_back({cellXl, cellXh});
    }
  }

  std::vector<internal::PlacementCoverageRow> coverageRows;
  coverageRows.reserve(domains.rows.size());
  for (size_t i = 0; i < domains.rows.size(); ++i) {
    internal::PlacementCoverageRow coverageRow;
    coverageRow.rowId = domains.rows[i].id;
    coverageRow.legalSpans = domains.rows[i].legalSpans;
    coverageRow.placedSpans = std::move(placedSpans[i]);
    coverageRows.push_back(std::move(coverageRow));
  }
  return internal::findCoverageFindings(std::move(coverageRows));
}

}  // namespace

class FillerRepairEngine::Impl
{
 public:
  Impl(Grid* grid, Network* network) : grid_(grid), network_(network) {}

  bool init(eUNL::PhysDesMgr* desMgr,
            const fillerSetting& fillerSettings)
  {
    // One engine represents exactly one design revision. A second init could
    // otherwise leave the private checker/view bound to mixed infrastructure.
    if (init_attempted_) {
      return false;
    }
    init_attempted_ = true;
    des_mgr_ = desMgr;
    if (!bindInfrastructure(desMgr, fillerSettings)) {
      return false;
    }

    // Build once here to validate the borrowed infrastructure. repair() may
    // rebuild after lazily registering an uninstantiated target master.
    initialized_ = rebuildOracle();
    if (!initialized_) {
      init_diagnostics_.insert(init_diagnostics_.end(),
                               oracle_diagnostics_.begin(),
                               oracle_diagnostics_.end());
      return false;
    }
    // The Grid legal domain is immutable within this snapshot (only PLACED
    // cells move between precheck calls), so walk the pixels once instead of
    // on every precheck.
    precheck_domains_ = buildPrecheckDomains(grid_);
    return initialized_;
  }

  void setDebugLogging(bool enabled)
  {
    debug_logging_ = enabled;
    if (view_ != nullptr) {
      view_->setDebugLogging(enabled);
    }
  }

  ipl::CheckResult precheck() const
  {
    ipl::CheckResult result;
    if (!initialized_) {
      result.isLegal = false;
      result.diagnostics = init_diagnostics_;
      result.diagnostics.push_back(
          {"precheck_not_initialized",
           "warning: init() must succeed before placement precheck"});
      return result;
    }
    const std::vector<internal::CoverageFinding> findings
        = findGapAndOverlap(des_mgr_, network_, precheck_domains_);
    result.isLegal = findings.empty();
    for (const internal::CoverageFinding& finding : findings) {
      const char* status = internal::coverageFindingStatus(finding.kind);
      result.diagnostics.push_back(
          {status,
           cat("warning: placement ", status, " row=", finding.rowId,
               " x=[", finding.span.xl, ",", finding.span.xh,
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
      result.diagnostics = init_diagnostics_;
      result.diagnostics.push_back(
          {"engine_not_initialized",
           "fatal: init() must succeed before repair()"});
      return result;
    }

    // DePlace normally imports masters from instantiated cells only. Register
    // an uninstantiated target replacement on demand, then rebuild the private
    // checker/view so both share the expanded Master::getId() universe.
    if (network_->getMaster(newMaster.getLibCellId()) == nullptr) {
      if (network_->addMaster(newMaster, grid_) == nullptr || !rebuildOracle()) {
        result.diagnostics = oracle_diagnostics_;
        result.diagnostics.push_back(
            {"target_master_registration_failed",
             "fatal: target master could not be added to the repair oracle"});
        return result;
      }
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
  void failInit(const std::string& status, const std::string& message)
  {
    init_diagnostics_.push_back({status, message});
  }

  bool bindInfrastructure(eUNL::PhysDesMgr* desMgr,
                          const fillerSetting& fillerSettings)
  {
    if (grid_ == nullptr || network_ == nullptr) {
      failInit("missing_infrastructure",
               "fatal: missing initialized Grid or Network");
      return false;
    }
    if (desMgr == nullptr) {
      failInit("missing_phys_des_mgr", "fatal: missing PhysDesMgr");
      return false;
    }
    if (fillerSettings.getDesign() == nullptr
        || fillerSettings.getDesign()->getPhysDesMgr() != desMgr) {
      failInit("design_mismatch",
               "fatal: fillerSetting and PhysDesMgr describe different designs");
      return false;
    }
    // ImplantLayerChecker currently extracts UDM through Session in its
    // constructor. Validate that implicit source before constructing it so a
    // caller cannot accidentally combine an explicit desMgr with another
    // active design.
    eUNL::Design* activeDesign
        = eUNL::Session::getSession().getCurrentDesign();
    if (activeDesign == nullptr || activeDesign->getPhysDesMgr() != desMgr) {
      failInit("active_design_mismatch",
               "fatal: PhysDesMgr is not the Session current design");
      return false;
    }
    if (fillerSettings.getFillerMasters().empty()) {
      failInit("empty_filler_allow_list",
               "fatal: fillerSetting::getFillerMasters() is empty");
      return false;
    }
    if (network_->getNodes().empty() || network_->getMasters().empty()) {
      failInit("empty_infrastructure",
               "fatal: Grid/Network must be initialized by dpl2 before repair");
      return false;
    }
    // Reject a Network borrowed from another design even when its LeafCellID
    // values happen to collide with the active design's IDs.
    for (const auto& node : network_->getNodes()) {
      if (node == nullptr || node->getMaster() == nullptr
          || node->getMaster()->getPhysLibCell() == nullptr) {
        failInit("invalid_network_node",
                 "fatal: Network contains an incomplete node/master mapping");
        return false;
      }
      const eUNL::PhysCell cell = desMgr->getPhysCell(node->getDbInst());
      if (!cell.isValid()
          || cell.getPhysMaster().getLibCellId()
                 != node->getMaster()->getDbMaster()) {
        failInit("infrastructure_design_mismatch",
                 cat("fatal: Network node ", node->getId(),
                     " does not match the active PhysDesMgr"));
        return false;
      }
    }

    // Candidate masters may be uninstantiated. Extending the infrastructure
    // master registry is idempotent and does not mutate UDM placement.
    filler_masters_ = fillerSettings.getFillerMasters();
    for (const eLIB::PhysLibCell* master : filler_masters_) {
      if (master == nullptr) {
        failInit("null_filler_master",
                 "fatal: getFillerMasters() returned null");
        continue;
      }
      network_->addMaster(*master, grid_);
    }
    return init_diagnostics_.empty();
  }

  bool rebuildOracle()
  {
    view_.reset();
    checker_.reset();
    oracle_diagnostics_.clear();

    checker_ = std::make_unique<ipl::ImplantLayerChecker>(grid_, network_);
    ProductionView::Config config;
    config.verbose = debug_logging_;
    config.repair.verbose = debug_logging_;
    view_ = std::make_unique<ProductionView>(des_mgr_,
                                             grid_,
                                             network_,
                                             checker_.get(),
                                             filler_masters_,
                                             config);
    if (view_->isReady()) {
      return true;
    }
    for (const Diagnostic& diagnostic : view_->setupDiagnostics()) {
      oracle_diagnostics_.push_back(toProductionDiagnostic(diagnostic));
    }
    return false;
  }

  Grid* grid_ = nullptr;
  Network* network_ = nullptr;
  eUNL::PhysDesMgr* des_mgr_ = nullptr;
  std::vector<const eLIB::PhysLibCell*> filler_masters_;
  std::unique_ptr<ipl::ImplantLayerChecker> checker_;
  std::unique_ptr<ProductionView> view_;
  std::vector<ipl::Diagnostic> init_diagnostics_;
  std::vector<ipl::Diagnostic> oracle_diagnostics_;
  // Grid legal domain cached at init; precheck only refreshes placed spans.
  PrecheckDomains precheck_domains_;
  bool debug_logging_ = false;
  // True only after a fully successful init(); every public API fails closed
  // until then (a half-built snapshot must never answer queries).
  bool initialized_ = false;
  bool init_attempted_ = false;
  std::atomic<bool> repair_active_{false};
};

FillerRepairEngine::FillerRepairEngine(Grid* grid, Network* network)
    : impl_(std::make_unique<Impl>(grid, network))
{
}

FillerRepairEngine::~FillerRepairEngine() = default;

void FillerRepairEngine::setDebugLogging(bool enabled)
{
  impl_->setDebugLogging(enabled);
}

bool FillerRepairEngine::init(eUNL::PhysDesMgr* desMgr,
                              const fillerSetting& fillerSettings)
{
  return impl_->init(desMgr, fillerSettings);
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
