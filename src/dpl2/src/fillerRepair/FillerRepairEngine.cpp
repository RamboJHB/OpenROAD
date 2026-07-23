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
#include "PlannerDataSource.h"
#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"
#include "infrastructure/fillerSetting.h"
#include "infrastructure/network.h"

namespace dpl2 {
namespace fillerRepair {

struct PrecheckDomains;

class FillerRepairEngine::Impl final : private PlannerDataSource,
                                       private PlannerOracle
{
 public:
  Impl(Grid* grid, Network* network);
  ~Impl();

  bool init(eUNL::PhysDesMgr* desMgr,
            const fillerSetting& fillerSettings);
  void setDebugLogging(bool enabled);
  ipl::CheckResult precheck() const;
  // Gap/overlap coverage restricted to selected repair rows. repair() checks
  // the initial target influence before registration; adaptive candidates
  // that edit farther rows are checked before entering the checker batch.
  // This keeps the safety gate proportional to touched rows instead of the
  // whole placed design.
  ipl::CheckResult localPrecheck(const Region& influence) const;
  RepairOutcome repair(const ipl::CheckRequest& request);
  RepairOutcome repair(eUNL::LeafCellID targetCell,
                       const eLIB::PhysLibCell& newMaster);

 private:
  struct Config
  {
    RepairConfig repair;
    DbCoord snapshotHaloX = 0;
    int snapshotHaloRows = 1;
    bool verbose = false;
  };

  void buildPlannerData();
  bool isReady() const;
  bool isNonBlockingCheckerInitDiagnostic(
      const ipl::Diagnostic& diagnostic) const;
  bool bindInfrastructure(eUNL::PhysDesMgr* desMgr,
                          const fillerSetting& fillerSettings);
  bool ensureMasterRegistered(const eLIB::PhysLibCell& master);
  bool rebuildOracle();
  void failInit(const std::string& status, const std::string& message);
  const std::vector<RowId>& rows() const override { return row_list_; }
  DbCoord siteWidth() const override { return site_width_; }
  const std::vector<PlacedInstance>& instancesInRow(RowId rowId) const override;
  const PlacedInstance* instance(InstanceId id) const override;
  const MasterInfo* masterInfo(MasterId id) const override;
  const std::vector<MasterId>& fillerMasterIds() const override
  {
    return filler_master_ids_;
  }
  FillerCellRecord fillerCellRecord(InstanceId instanceId,
                                    MasterId newMasterId) const override;
  OracleResult checkPlaceWithOverlay(const OracleRequest& request) override;
  std::vector<OracleResult> checkPlaceWithOverlays(
      const std::vector<OracleRequest>& requests) override;

  void addProblem(Severity severity,
                  const std::string& code,
                  const std::string& message);
  ipl::CheckRequest toCheckRequest(const TargetPlace& place) const;
  ::Rect toGuardRect(const Region& region) const;
  Violation toPlannerViolation(const ipl::Violation& violation,
                               InstanceId targetInstance) const;
  Region snapshotGuard(const TargetPlace& target) const;
  Region snapshotGuard(RowId rowId,
                       DbCoord x,
                       DbCoord width,
                       DbCoord heightRows) const;
  RepairOutcome repairImpl(
      std::optional<ipl::CheckRequest> checkerRequest,
      std::optional<eUNL::LeafCellID> targetCell,
      const eLIB::PhysLibCell* newMaster);

  // Grows `table` so `id` is a valid index (ids can exceed the presized
  // container counts only if the Network id spaces are not dense).
  template <typename T>
  static void ensureSlot(std::vector<T>& table, size_t id)
  {
    if (id >= table.size()) {
      table.resize(id + 1);
    }
  }

  Grid* grid_ = nullptr;
  Network* network_ = nullptr;
  eUNL::PhysDesMgr* des_mgr_ = nullptr;
  std::vector<const eLIB::PhysLibCell*> filler_masters_;
  std::unique_ptr<ipl::ImplantLayerChecker> checker_;
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
  std::vector<ipl::Diagnostic> init_diagnostics_;
  std::vector<ipl::Diagnostic> oracle_diagnostics_;
  std::unique_ptr<PrecheckDomains> precheck_domains_;
  bool debug_logging_ = false;
  bool initialized_ = false;
  bool init_attempted_ = false;
  std::atomic<bool> repair_active_{false};
  mutable std::mutex checker_mutex_;
};

namespace {

ipl::Diagnostic toPublicDiagnostic(const Diagnostic& diagnostic);

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

std::string masterDebug(const eLIB::PhysLibCell& cell)
{
  const eLIB::PhysMacroType& type = cell.getType();
  return cat("libCell=", cell.getLibCellId().getIndexValue(),
             " width=", cell.getWidth().getStorage(),
             " height=", cell.getHeight().getStorage(),
             " macroType=", static_cast<int>(type.getType()),
             " typeFlags{core=", type.isCore(),
             ",coreFiller=", type.isCoreFiller(),
             ",padFiller=", type.isPadFiller(),
             ",block=", type.isBlock(),
             ",endcap=", type.isEndcap(), "}");
}

bool isStandardCellMaster(const eLIB::PhysLibCell& cell)
{
  return cell.getType().isCore() && !cell.getType().isBlock()
         && !isFillerMaster(cell);
}

ViolationKind toKind(ipl::RuleSource source)
{
  return (source == ipl::RuleSource::Width
          || source == ipl::RuleSource::Lef58Width)
             ? ViolationKind::MinWidth
             : ViolationKind::MinSpacing;
}

VtId toPlannerVt(ipl::Layer::Vt vt)
{
  switch (vt) {
    case ipl::Layer::Vt::S: return 0;
    case ipl::Layer::Vt::L: return 1;
    case ipl::Layer::Vt::H: return 2;
    case ipl::Layer::Vt::UL: return 3;
    case ipl::Layer::Vt::Unknown: break;
  }
  return kUnknownVt;
}

// Final checker: Relationship is exactly {IntraRow, InterRow}.
ViolationRelation toRelation(ipl::Relationship relationship)
{
  return relationship == ipl::Relationship::InterRow
             ? ViolationRelation::InterRow
             : ViolationRelation::IntraRow;
}

}  // namespace

void FillerRepairEngine::Impl::buildPlannerData()
{
  eUNL::PhysDesMgr* desMgr = des_mgr_;
  Grid* grid = grid_;
  Network* network = network_;
  const ipl::ImplantLayerChecker* checker = checker_.get();
  const auto& fillerMasters = filler_masters_;
  site_width_ = 0;
  row_height_ = 0;
  default_halo_x_ = 0;
  row_list_.clear();
  masters_.clear();
  instances_.clear();
  udm_refs_.clear();
  master_lib_ids_.clear();
  by_row_.clear();
  filler_master_ids_.clear();
  setup_diagnostics_.clear();
  config_.repair.verbose = config_.repair.verbose || config_.verbose;
  if (desMgr == nullptr || grid == nullptr || network == nullptr
      || checker == nullptr) {
    addProblem(Severity::Fatal, "MissingDependency",
               cat("engine initialization dependency is missing: desMgr=",
                   desMgr != nullptr, " grid=", grid != nullptr,
                   " network=", network != nullptr,
                   " checker=", checker != nullptr));
    return;
  }

  // --- rows: RowId = PhysRow iteration index over ALL rows (the checker's
  // convention); legal spans and uniformity checks cover non-pad rows only.
  struct RowFrame
  {
    DbCoord originX = 0;
    DbCoord originY = 0;
    DbCoord yLo = 0;
    DbCoord yHi = 0;
    DbCoord siteWidth = 0;
    DbCoord siteHeight = 0;
    DbCoord bboxXl = 0;
    DbCoord bboxYl = 0;
    DbCoord bboxXh = 0;
    DbCoord bboxYh = 0;
    int siteCount = 0;
    std::string siteName;
    bool isPad = false;
  };
  std::vector<RowFrame> frames;
  RowId referenceRowId = -1;
  const auto showRow = [](RowId rowId, const RowFrame& frame) {
    return cat("row=", rowId,
               " site=\"", frame.siteName, "\"",
               " isPad=", frame.isPad,
               " siteWidth=", frame.siteWidth,
               " siteHeight=", frame.siteHeight,
               " siteCount=", frame.siteCount,
               " origin=(", frame.originX, ",", frame.originY, ")",
               " bbox=[", frame.bboxXl, ",", frame.bboxYl, ",",
               frame.bboxXh, ",", frame.bboxYh, ")");
  };
  {
    RowId rowId = 0;
    for (const eUNL::PhysRow& row : desMgr->getPhysRowIter()) {
      RowFrame frame;
      frame.isPad = row.getSite().getIsPad();
      frame.originX = row.getOrigin().getX().getStorage();
      frame.originY = row.getOrigin().getY().getStorage();
      frame.yLo = row.getOrigin().getY().getStorage();
      frame.yHi = (row.getOrigin().getY() + row.getSite().getHeight())
                      .getStorage();
      frame.siteWidth = row.getSite().getWidth().getStorage();
      frame.siteHeight = row.getSite().getHeight().getStorage();
      frame.siteCount = row.getSiteCnt();
      frame.siteName = row.getSite().getName();
      const eUTL::Rect bbox = row.getBbox();
      frame.bboxXl = bbox.getXL().getStorage();
      frame.bboxYl = bbox.getYL().getStorage();
      frame.bboxXh = bbox.getXH().getStorage();
      frame.bboxYh = bbox.getYH().getStorage();
      if (!frame.isPad) {
        const DbCoord width = frame.siteWidth;
        const DbCoord height = frame.siteHeight;
        if (referenceRowId < 0) {
          referenceRowId = rowId;
        }
        if (site_width_ == 0) {
          site_width_ = width;
        } else if (site_width_ != width) {
          addProblem(Severity::Fatal, "NonUniformSiteWidth",
                     cat("non-pad rows do not share one site width: "
                         "reference={",
                         showRow(referenceRowId,
                                 frames[static_cast<size_t>(referenceRowId)]),
                         "} observed={", showRow(rowId, frame),
                         "} engineSiteWidth=", site_width_,
                         " gridSiteWidth=", grid->getSiteWidth().v,
                         " checkerSiteWidth=", checker->siteWidth()));
        }
        if (row_height_ == 0) {
          row_height_ = height;
        } else if (row_height_ != height) {
          addProblem(Severity::Fatal, "NonUniformRowHeight",
                     cat("non-pad rows do not share one row height: "
                         "reference={",
                         showRow(referenceRowId,
                                 frames[static_cast<size_t>(referenceRowId)]),
                         "} observed={", showRow(rowId, frame),
                         "} engineRowHeight=", row_height_));
        }
        const DbCoord spanWidth = (bbox.getXH() - bbox.getXL()).getStorage();
        if (spanWidth <= 0 || width <= 0 || spanWidth % width != 0) {
          addProblem(Severity::Fatal, "InvalidRowSpan",
                     cat("invalid legal row span: {", showRow(rowId, frame),
                         "} spanWidth=", spanWidth,
                         " spanModuloSiteWidth=",
                         width > 0 ? spanWidth % width : spanWidth));
        }
        row_list_.push_back(rowId);
      }
      frames.push_back(frame);
      ++rowId;
    }
    by_row_.resize(frames.size());
  }
  if (site_width_ <= 0 || row_height_ <= 0 || row_list_.empty()) {
    addProblem(Severity::Fatal, "MissingRowGeometry",
               cat("no usable standard-cell row geometry: physRows=",
                   frames.size(), " nonPadRows=", row_list_.size(),
                   " engineSiteWidth=", site_width_,
                   " engineRowHeight=", row_height_,
                   " gridSiteWidth=", grid->getSiteWidth().v,
                   " gridRows=", grid->getRowCount().v,
                   " gridSitesPerRow=", grid->getRowSiteCount().v,
                   " checkerSiteWidth=", checker->siteWidth()));
  }
  if (checker->siteWidth() != site_width_) {
    addProblem(Severity::Fatal, "CheckerSiteWidthMismatch",
               cat("checker site width ", checker->siteWidth(),
                   " differs from engine reference ", site_width_,
                   "; gridSiteWidth=", grid->getSiteWidth().v,
                   " reference={",
                   referenceRowId >= 0
                       ? showRow(referenceRowId,
                                 frames[static_cast<size_t>(referenceRowId)])
                       : std::string("none"),
                   "}"));
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
                   cat("non-pad row origins do not share one X frame: "
                       "reference={",
                       showRow(static_cast<RowId>(
                                   std::distance(frames.begin(), firstNonPad)),
                               *firstNonPad),
                       "} observed={",
                       showRow(static_cast<RowId>(i), frames[i]),
                       "} gridCoreXl=",
                       grid->getCore().getXL().getStorage(),
                       " gridSiteWidth=", grid->getSiteWidth().v));
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
  const auto isConfiguredFiller =
      [&fillerMasters](eLIB::LibCellID libCellId) {
        return std::any_of(
            fillerMasters.begin(), fillerMasters.end(),
            [libCellId](const eLIB::PhysLibCell* candidate) {
              return candidate != nullptr
                     && candidate->getLibCellId() == libCellId;
            });
      };

  // --- implant metadata: VT family / band polarity per master, derived from
  // the master's implant shapes exactly like the checker (layer identity via
  // the checker's TechLayerRelativeID, band anchored at the bottommost
  // implant rect -- the rebuildMasterShapes rule).
  const eLIB::TechLib& tech = desMgr->getTopTech();
  const auto implantLayerOf =
      [&](eLIB::TechLayerRelativeID relId) -> const ipl::Layer* {
    for (const ipl::Layer& layer : checker->getLayers()) {
      if (layer.getTechLayerId() == relId) {
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
        const ipl::Layer* layer = implantLayerOf(layerRelId);
        if (layer == nullptr) {
          continue;
        }
        for (const auto& techShape : shapeVec) {
          if (techShape.getType() != eLIB::TechShape::RECT) {
            continue;
          }
          if (info.vt == kUnknownVt
              && layer->getVt() != ipl::Layer::Vt::Unknown) {
            info.vt = toPlannerVt(layer->getVt());
          }
          const DbCoord yl = techShape.getRect().getYL().getStorage();
          if (!haveBottom || yl < bottomYl) {
            haveBottom = true;
            bottomYl = yl;
            info.bottomBandPolarity
                = layer->getPolar() == ipl::Layer::Polar::P
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
    size_t configuredIndex = 0;
    for (const eLIB::PhysLibCell* cell : fillerMasters) {
      if (cell == nullptr) {
        ++configuredIndex;
        continue;
      }
      const int id = network->getMasterId(cell->getLibCellId());
      if (id < 0) {
        // Fatal: the checker validates candidates against Network masters, so
        // a configured master the Network never imported means the snapshot
        // was built against different inputs -- refuse instead of silently
        // shrinking the candidate universe.
        addProblem(Severity::Fatal, "ConfiguredMasterNotInNetwork",
                   cat("configured filler master is not in Network: "
                       "configuredIndex=",
                       configuredIndex, " {", masterDebug(*cell),
                       "} networkMasters=", network->getMasters().size(),
                       " networkMasterId=", id));
        ++configuredIndex;
        continue;
      }
      const MasterInfo* info = masterInfo(static_cast<MasterId>(id));
      if (info == nullptr || !info->isFiller) {
        addProblem(Severity::Fatal, "ConfiguredMasterNotFiller",
                   cat("configured master failed filler classification: "
                       "configuredIndex=",
                       configuredIndex, " networkMasterId=", id,
                       " masterInfoPresent=", info != nullptr,
                       " masterInfoIsFiller=",
                       info != nullptr ? info->isFiller : false,
                       " typePredicate=", isFillerMaster(*cell),
                       " {", masterDebug(*cell), "}"));
        ++configuredIndex;
        continue;
      }
      filler_master_ids_.push_back(static_cast<MasterId>(id));
      ++configuredIndex;
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
               cat("fillerSetting::getFillerPhysCells() yields no usable "
                   "filler master: configuredCount=",
                   fillerMasters.size(),
                   " acceptedCount=", filler_master_ids_.size(),
                   " networkMasters=", network->getMasters().size()));
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
                 cat("node has unsupported orientation: node=",
                     node->getId(), " leafCell=",
                     lcId.getIndexValue(), " master=",
                     node->getMaster()->getId(), " orient=",
                     static_cast<int>(physCell.getOrient().getValue()),
                     " status=", static_cast<int>(status), " {",
                     masterDebug(*cell), "}"));
      continue;
    }
    const eUTL::Point2D origin = physCell.getOrigin();
    const RowId rowId = rowContaining(origin.getY().getStorage());
    if (rowId < 0) {
      addProblem(Severity::Fatal, "NodeOutsideRows",
                 cat("node origin is not in any PhysRow: node=",
                     node->getId(), " leafCell=", lcId.getIndexValue(),
                     " origin=(", origin.getX().getStorage(), ",",
                     origin.getY().getStorage(), ") master=",
                     node->getMaster()->getId(), " physRows=", frames.size(),
                     " firstRowY=[",
                     frames.empty() ? 0 : frames.front().yLo, ",",
                     frames.empty() ? 0 : frames.front().yHi, ") lastRowY=[",
                     frames.empty() ? 0 : frames.back().yLo, ",",
                     frames.empty() ? 0 : frames.back().yHi, ") {",
                     masterDebug(*cell), "}"));
      continue;
    }
    const DbCoord xOffset =
        origin.getX().getStorage() - frames[static_cast<size_t>(rowId)].originX;
    if (xOffset < 0) {
      addProblem(Severity::Fatal, "NodeLeftOfRowOrigin",
                 cat("node lies left of its row origin: node=",
                     node->getId(), " leafCell=", lcId.getIndexValue(),
                     " originX=", origin.getX().getStorage(),
                     " xOffset=", xOffset, " {",
                     showRow(rowId, frames[static_cast<size_t>(rowId)]),
                     "} gridCoreXl=",
                     grid->getCore().getXL().getStorage(), " {",
                     masterDebug(*cell), "}"));
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
                 cat("PhysRow/Grid row frames disagree: node=",
                     node->getId(), " leafCell=", lcId.getIndexValue(),
                     " origin=(", origin.getX().getStorage(), ",",
                     origin.getY().getStorage(), ") physRow=", rowId,
                     " gridRow=", gridRow, " gridRows=",
                     grid->getRowCount().v, " physRows=", frames.size(),
                     " physRowData={",
                     showRow(rowId, frames[static_cast<size_t>(rowId)]),
                     "}; pad rows before standard rows or non-y-sorted row "
                     "iteration is not supported"));
      continue;
    }
    const DbCoord gridCol = static_cast<DbCoord>(grid->gridX(node).v);
    const DbCoord physCol = site_width_ > 0 ? xOffset / site_width_ : -1;
    if (site_width_ > 0 && gridCol != physCol) {
      addProblem(Severity::Fatal, "ColFrameMismatch",
                 cat("PhysRow/Grid column frames disagree: node=",
                     node->getId(), " leafCell=", lcId.getIndexValue(),
                     " originX=", origin.getX().getStorage(),
                     " rowOriginX=",
                     frames[static_cast<size_t>(rowId)].originX,
                     " gridCoreXl=",
                     grid->getCore().getXL().getStorage(),
                     " xOffset=", xOffset,
                     " engineSiteWidth=", site_width_,
                     " gridSiteWidth=", grid->getSiteWidth().v,
                     " physRowColumn=", physCol,
                     " gridColumn=", gridCol, " {",
                     showRow(rowId, frames[static_cast<size_t>(rowId)]), "}"));
      continue;
    }

    const MasterId masterId = static_cast<MasterId>(node->getMaster()->getId());
    const MasterInfo* info = masterInfo(masterId);
    if (info == nullptr) {
      addProblem(Severity::Fatal, "UnknownMaster",
                 cat("node references a master absent from planner snapshot: "
                     "node=",
                     node->getId(), " leafCell=", lcId.getIndexValue(),
                     " masterId=", masterId,
                     " plannerMasterSlots=", masters_.size(),
                     " networkMasters=", network->getMasters().size(),
                     " {", masterDebug(*cell), "}"));
      continue;
    }
    const bool isFiller = isFillerMaster(*cell);
    if (node->isFiller() != isFiller) {
      addProblem(Severity::Fatal, "FillerClassificationMismatch",
                 cat("node filler flag disagrees with physical master: node=",
                     node->getId(), " leafCell=", lcId.getIndexValue(),
                     " nodeType=", static_cast<int>(node->getType()),
                     " nodeIsFiller=", node->isFiller(),
                     " nodeIsStdCell=", node->isStdCell(),
                     " masterId=", masterId,
                     " masterInfoIsFiller=", info->isFiller,
                     " typePredicate=", isFiller,
                     " inConfiguredFillerList=",
                     isConfiguredFiller(cell->getLibCellId()), " {",
                     masterDebug(*cell), "}"));
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
                   cat("multi-row node extends outside PhysRow inventory: "
                       "node=",
                       node->getId(), " leafCell=", lcId.getIndexValue(),
                       " startRow=", rowId,
                       " masterHeightRows=", info->height,
                       " failingOffset=", offset,
                       " requestedRow=", rowCopy.rowId,
                       " physRows=", frames.size(), " origin=(",
                       origin.getX().getStorage(), ",",
                       origin.getY().getStorage(), ") {",
                       masterDebug(*cell), "}"));
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

bool FillerRepairEngine::Impl::isReady() const
{
  return std::none_of(
      setup_diagnostics_.begin(), setup_diagnostics_.end(),
      [](const Diagnostic& d) { return d.severity == Severity::Fatal; });
}

bool FillerRepairEngine::Impl::isNonBlockingCheckerInitDiagnostic(
    const ipl::Diagnostic& diagnostic) const
{
  // A non-placed Network node is outside both the checker snapshot and the
  // planner view. The checker reports the skip persistently, but it does not
  // make the placed design's oracle incomplete.
  if (diagnostic.status == "skipped_phys_status") {
    return true;
  }

  // Missing rules are safe only for an implant layer unused by EVERY master
  // in the shared Network. This preserves informational diagnostics for spare
  // technology layers without allowing a used layer to silently lose WIDTH or
  // SPACING coverage. All other checker-init statuses fail closed below.
  if (diagnostic.status != "missing_rule_parameter"
      && diagnostic.status != "skipped_missing_rule_parameter") {
    return false;
  }
  if (des_mgr_ == nullptr || network_ == nullptr || checker_ == nullptr) {
    return false;
  }

  std::set<std::string> usedLayerNames;
  const eLIB::TechLib& tech = des_mgr_->getTopTech();
  for (const auto& masterPtr : network_->getMasters()) {
    if (masterPtr == nullptr || masterPtr->getPhysLibCell() == nullptr) {
      continue;
    }
    for (const auto& obstruction :
         masterPtr->getPhysLibCell()->getObstruction()) {
      for (const auto& [layerRelId, shapes] :
           obstruction.getShapes(eUTL::PhysOrientationE::R0)) {
        if (!shapes.empty()) {
          usedLayerNames.insert(tech.getTechLayer(layerRelId).getName());
        }
      }
    }
  }

  for (const ipl::Layer& layer : checker_->getLayers()) {
    const std::string marker = cat("layer ", layer.getName(), " ");
    if (diagnostic.message.find(marker) != std::string::npos) {
      return usedLayerNames.count(layer.getName()) == 0;
    }
  }
  return false;
}

void FillerRepairEngine::Impl::addProblem(Severity severity,
                               const std::string& code,
                               const std::string& message)
{
  setup_diagnostics_.push_back(makeDiag(severity, code, message));
  log_.msg("engine", cat(code, ": ", message));
}

const std::vector<PlacedInstance>& FillerRepairEngine::Impl::instancesInRow(
    RowId rowId) const
{
  return rowId >= 0 && static_cast<size_t>(rowId) < by_row_.size()
             ? by_row_[rowId]
             : emptyInstances();
}

const PlacedInstance* FillerRepairEngine::Impl::instance(InstanceId id) const
{
  return id >= 0 && static_cast<size_t>(id) < instances_.size()
                 && instances_[id].has_value()
             ? &*instances_[id]
             : nullptr;
}

const MasterInfo* FillerRepairEngine::Impl::masterInfo(MasterId id) const
{
  return id >= 0 && static_cast<size_t>(id) < masters_.size()
                 && masters_[id].has_value()
             ? &*masters_[id]
             : nullptr;
}

// --- oracle: direct calls into the final ipl checker -----------------------

ipl::CheckRequest FillerRepairEngine::Impl::toCheckRequest(const TargetPlace& place) const
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

::Rect FillerRepairEngine::Impl::toGuardRect(const Region& region) const
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

Violation FillerRepairEngine::Impl::toPlannerViolation(const ipl::Violation& v,
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

FillerCellRecord FillerRepairEngine::Impl::fillerCellRecord(
    InstanceId instanceId,
    MasterId newMasterId) const
{
  FillerCellRecord record{dpl2::OpType::Replace,
                          eUNL::LeafCellID(0, 0),
                          eUTL::UvDist(static_cast<int64_t>(0)),
                          eUTL::UvDist(static_cast<int64_t>(0)),
                          eLIB::LibCellID(0, 0),
                          eLIB::LibCellID(0, 0)};
  const bool haveRef = instanceId >= 0
                       && static_cast<size_t>(instanceId) < udm_refs_.size()
                       && udm_refs_[instanceId].has_value();
  if (haveRef) {
    const UdmRef& ref = *udm_refs_[instanceId];
    record.cell_id_ = ref.cellId;
    record.orig_lib_cell_ = ref.libCellId;
    record.origin_x_ = ref.originX;
    record.origin_y_ = ref.originY;
  }
  if (masterInfo(newMasterId) != nullptr) {
    record.new_lib_cell_ = master_lib_ids_[newMasterId];
  }
  return record;
}

OracleResult FillerRepairEngine::Impl::checkPlaceWithOverlay(
    const OracleRequest& request)
{
  std::vector<OracleResult> results = checkPlaceWithOverlays({request});
  if (results.size() == 1) {
    return std::move(results.front());
  }
  OracleResult failure;
  failure.requestId = request.requestId;
  failure.status = OracleStatus::CheckerError;
  failure.diagnostics.push_back(makeDiag(
      Severity::Fatal, "CheckerProtocolError",
      "checker result count does not match the single oracle request"));
  return failure;
}

std::vector<OracleResult> FillerRepairEngine::Impl::checkPlaceWithOverlays(
    const std::vector<OracleRequest>& requests)
{
  std::vector<OracleResult> results(requests.size());
  if (requests.empty()) {
    return results;
  }

  // One (target, guard) + N candidates per batch, by engine construction; a
  // mixed batch is a protocol error.
  const OracleRequest& first = requests.front();
  for (const OracleRequest& request : requests) {
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
        results[i].status = OracleStatus::CheckerError;
        results[i].diagnostics.push_back(makeDiag(
            Severity::Fatal, "CheckerProtocolError",
            "mixed (targetPlace, guardRegion) in one overlay batch"));
      }
      return results;
    }
  }

  // The initial target influence was checked before any master registration.
  // Adaptive windows can later introduce fillers from additional rows. Check
  // only requests that actually edit outside the initial influence, and keep
  // illegal requests out of the checker batch without rejecting legal peers.
  const Region initialInfluence = snapshotGuard(first.targetPlace);
  std::vector<size_t> legalIndices;
  legalIndices.reserve(requests.size());
  bool precheckFiltered = false;
  for (size_t i = 0; i < requests.size(); ++i) {
    Region influence = initialInfluence;
    for (const FillerCellRecord& change : requests[i].fillerChanges) {
      const Node* node = network_->getNode(change.cell_id_);
      const PlacedInstance* placed
          = node != nullptr ? instance(node->getId()) : nullptr;
      if (placed != nullptr) {
        const MasterInfo* master = masterInfo(placed->masterId);
        const DbCoord heightRows
            = master != nullptr ? std::max<DbCoord>(master->height, 1) : 1;
        influence.rowLo = std::min(influence.rowLo, placed->rowId);
        influence.rowHi = std::max(
            influence.rowHi,
            placed->rowId + static_cast<RowId>(heightRows) - 1);
      }
    }
    if (influence.rowLo == initialInfluence.rowLo
        && influence.rowHi == initialInfluence.rowHi) {
      legalIndices.push_back(i);
      continue;
    }

    const ipl::CheckResult placement = localPrecheck(influence);
    if (placement.isLegal) {
      legalIndices.push_back(i);
      continue;
    }

    precheckFiltered = true;
    OracleResult& out = results[i];
    out.requestId = requests[i].requestId;
    out.status = OracleStatus::InvalidOverlay;
    out.isLegal = false;
    for (const ipl::Diagnostic& diagnostic : placement.diagnostics) {
      out.diagnostics.push_back(makeDiag(
          Severity::Warning, diagnostic.status, diagnostic.message));
    }
    out.diagnostics.push_back(makeDiag(
        Severity::Warning,
        "PrecheckFailed",
        "placement precheck failed in an adaptive filler row"));
  }
  if (legalIndices.empty()) {
    return results;
  }

  const ipl::CheckRequest target = toCheckRequest(first.targetPlace);
  const ::Rect guard = toGuardRect(first.guardRegion);
  std::vector<ipl::FillerChanges> changes;
  changes.reserve(legalIndices.size());
  for (const size_t index : legalIndices) {
    changes.push_back(requests[index].fillerChanges);
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

  // Ordered correlation is the final checker's entire batch protocol. Any
  // missing OR extra result invalidates the whole batch. Preserve only the
  // returned cardinality so OracleGate can diagnose the exact mismatch; no
  // checker finding from a mis-correlated batch is consumed.
  if (raw.size() != legalIndices.size()) {
    log_.msg("engine",
             cat("overlay batch protocol error: expected ", legalIndices.size(),
                 " result(s), received ", raw.size()));
    if (!precheckFiltered) {
      return std::vector<OracleResult>(raw.size());
    }
    for (const size_t index : legalIndices) {
      OracleResult& out = results[index];
      out.requestId = requests[index].requestId;
      out.status = OracleStatus::CheckerError;
      out.isLegal = false;
      out.diagnostics.push_back(makeDiag(
          Severity::Fatal,
          "CheckerProtocolError",
          "checker result count does not match the filtered overlay batch"));
    }
    return results;
  }

  // The final checker copies its approved, non-blocking PERSISTENT init
  // diagnostics into every result -- once directly (checkPlaceWithOverlay)
  // and once more inside the
  // embedded region result (checkOverlayRegion) -- and folds them into
  // isLegal. Strip every leading repetition of that sequence so only
  // request-specific findings drive the candidate status; otherwise a single
  // benign init diagnostic (e.g. missing_rule_parameter on an unused layer)
  // would make every candidate permanently illegal. Structural init
  // diagnostics never reach this path because rebuildOracle() fails closed.
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

  for (size_t rawIndex = 0; rawIndex < raw.size(); ++rawIndex) {
    const size_t requestIndex = legalIndices[rawIndex];
    OracleResult& out = results[requestIndex];
    out.requestId = requests[requestIndex].requestId;
    const ipl::CheckResult& r = raw[rawIndex];
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
      out.status = OracleStatus::InvalidOverlay;
      out.isLegal = false;
    } else {
      out.status = OracleStatus::Checked;
      out.isLegal = out.violations.empty() && out.diagnostics.empty();
    }
  }
  return results;
}

// --- repair entry -----------------------------------------------------------

Region FillerRepairEngine::Impl::snapshotGuard(const TargetPlace& target) const
{
  const MasterInfo* master = masterInfo(target.masterId);
  const DbCoord width = master != nullptr ? master->width : 0;
  const DbCoord heightRows =
      master != nullptr ? std::max<DbCoord>(master->height, 1) : 1;
  return snapshotGuard(target.rowId, target.x, width, heightRows);
}

Region FillerRepairEngine::Impl::snapshotGuard(RowId rowId,
                                               DbCoord x,
                                               DbCoord width,
                                               DbCoord heightRows) const
{
  const DbCoord halo =
      config_.snapshotHaloX > 0 ? config_.snapshotHaloX : default_halo_x_;

  Region guard;
  guard.x = XInterval{x - halo, x + width + halo};
  const RowId minRow = row_list_.empty() ? 0 : row_list_.front();
  const RowId maxRow = row_list_.empty() ? 0 : row_list_.back();
  guard.rowLo = std::max<RowId>(
      minRow, rowId - static_cast<RowId>(config_.snapshotHaloRows));
  guard.rowHi = std::min<RowId>(
      maxRow,
      rowId + static_cast<RowId>(heightRows) - 1
          + static_cast<RowId>(config_.snapshotHaloRows));
  return guard;
}

RepairOutcome FillerRepairEngine::Impl::repair(
    const ipl::CheckRequest& request)
{
  return repairImpl(request, std::nullopt, nullptr);
}

RepairOutcome FillerRepairEngine::Impl::repair(
    eUNL::LeafCellID targetCell,
    const eLIB::PhysLibCell& newMaster)
{
  return repairImpl(std::nullopt, targetCell, &newMaster);
}

RepairOutcome FillerRepairEngine::Impl::repairImpl(
    std::optional<ipl::CheckRequest> checkerRequest,
    std::optional<eUNL::LeafCellID> targetCell,
    const eLIB::PhysLibCell* newMaster)
{
  RepairOutcome result;
  const auto addDiagnostic = [&result](Severity severity,
                                       const std::string& code,
                                       const std::string& message) {
    result.diagnostics.push_back(
        toPublicDiagnostic(makeDiag(severity, code, message)));
  };

  if (repair_active_.exchange(true, std::memory_order_acq_rel)) {
    addDiagnostic(Severity::Fatal,
                  "ReentrantRepair",
                  "repair() re-entered on one FillerRepairEngine");
    return result;
  }
  struct ActiveGuard
  {
    std::atomic<bool>& flag;
    ~ActiveGuard() { flag.store(false, std::memory_order_release); }
  } activeGuard{repair_active_};

  if (!initialized_) {
    result.diagnostics = init_diagnostics_;
    addDiagnostic(Severity::Fatal,
                  "engine_not_initialized",
                  "init() must succeed before repair()");
    return result;
  }

  if (!isReady()) {
    for (const Diagnostic& diagnostic : setup_diagnostics_) {
      result.diagnostics.push_back(toPublicDiagnostic(diagnostic));
    }
    addDiagnostic(Severity::Fatal,
                  "EngineNotReady",
                  "infrastructure snapshot failed validation; repair refused");
    return result;
  }

  // 1) Resolve the checker request (preferred) or the direct UDM request to
  // one immutable engine-snapshot target before touching the shared Network
  // master registry. The checker path deliberately does not reread candidate
  // placement from the live Node: DePlace may already have changed that Node
  // while the UDM commit is still pending.
  int targetId = -1;
  if (checkerRequest.has_value()) {
    targetId = checkerRequest->instanceId;
  } else if (targetCell.has_value()) {
    targetId = network_->getNodeId(*targetCell);
  }
  const PlacedInstance* inst =
      targetId >= 0 ? instance(static_cast<InstanceId>(targetId)) : nullptr;
  if (inst == nullptr) {
    addDiagnostic(Severity::Fatal,
                  "UnknownTarget",
                  "target is not a placed node in this engine snapshot");
    return result;
  }
  if (checkerRequest.has_value()) {
    if (static_cast<InstanceId>(checkerRequest->instanceId) != inst->id
        || checkerRequest->masterId < 0 || checkerRequest->rowId < 0
        || checkerRequest->colId < 0
        || !supportedOrientation(checkerRequest->orientation)) {
      addDiagnostic(Severity::Fatal,
                    "InvalidCheckRequest",
                    "checker supplied an invalid target placement request");
      return result;
    }
    if (static_cast<size_t>(inst->id) >= udm_refs_.size()
        || !udm_refs_[inst->id].has_value()) {
      addDiagnostic(Severity::Fatal,
                    "TargetMappingMissing",
                    "target has no physical cell mapping in the snapshot");
      return result;
    }
    targetCell = udm_refs_[inst->id]->cellId;
    const Master* requestedMaster = network_->getMaster(
        static_cast<int>(checkerRequest->masterId));
    newMaster = requestedMaster != nullptr
                    ? requestedMaster->getPhysLibCell()
                    : nullptr;
  }
  if (!targetCell.has_value() || newMaster == nullptr) {
    addDiagnostic(Severity::Fatal,
                  "TargetMasterUnknown",
                  "target replacement master is absent from Network");
    return result;
  }
  const MasterInfo* oldMaster = masterInfo(inst->masterId);
  if (oldMaster == nullptr) {
    addDiagnostic(Severity::Fatal,
                  "TargetMasterUnknown",
                  "the target's current master is absent from the snapshot");
    return result;
  }
  const Node* targetNode = network_->getNode(targetId);
  if (targetNode == nullptr || !targetNode->isStdCell() || inst->isFiller
      || oldMaster->isFiller
      || !isStandardCellMaster(*newMaster)) {
    addDiagnostic(Severity::Fatal,
                  "TargetNotStdCell",
                  "target and replacement master must both be standard cells");
    return result;
  }
  const DbCoord replacementWidth = newMaster->getWidth().getStorage();
  const DbCoord replacementHeight = grid_->gridHeight(*newMaster).v;
  if (oldMaster->width != replacementWidth
      || oldMaster->height != replacementHeight) {
    addDiagnostic(Severity::Fatal,
                  "TargetSizeMismatch",
                  "target VT replacement must preserve width and height");
    return result;
  }

  const InstanceId targetInstanceId = inst->id;
  const RowId targetRowId = inst->rowId;
  const DbCoord targetX = inst->x;
  const Orient targetOrientation = inst->orientation;

  const RowId requestedRowId = checkerRequest.has_value()
                                   ? static_cast<RowId>(checkerRequest->rowId)
                                   : targetRowId;
  const DbCoord requestedX = checkerRequest.has_value()
                                 ? static_cast<DbCoord>(checkerRequest->colId)
                                       * site_width_
                                 : targetX;
  const Orient requestedOrientation
      = checkerRequest.has_value()
            ? toPlannerOrient(checkerRequest->orientation)
            : targetOrientation;
  const Region initialInfluence = snapshotGuard(
      requestedRowId, requestedX, replacementWidth, replacementHeight);

  // Fail before registering an uninstantiated replacement master. The
  // rejected-request contract covers the in-memory Network registry too.
  const ipl::CheckResult placement = localPrecheck(initialInfluence);
  if (!placement.isLegal) {
    result.diagnostics = placement.diagnostics;
    addDiagnostic(Severity::Warning,
                  "PrecheckFailed",
                  "placement precheck failed in the target influence rows; "
                  "filler repair was skipped");
    return result;
  }

  // DePlace may not have imported an uninstantiated target replacement yet.
  // Register it only after request and placement validation, then rebuild the
  // checker and planner snapshot so all id tables share the expanded Network
  // universe.
  if (network_->getMaster(newMaster->getLibCellId()) == nullptr) {
    const bool registered = ensureMasterRegistered(*newMaster);
    const bool rebuilt = registered && rebuildOracle();
    if (!rebuilt) {
      initialized_ = false;
      result.diagnostics = oracle_diagnostics_;
      addDiagnostic(Severity::Fatal,
                    "TargetMasterRegistrationFailed",
                    "target master could not be added to the repair oracle");
      return result;
    }
  }

  const int newMasterId = network_->getMasterId(newMaster->getLibCellId());
  // The checker entry may receive a master that DePlace registered after the
  // engine snapshot was built. Rebuild the private oracle exactly once so the
  // new Network master id is understood by both checker and planner.
  if (newMasterId >= 0
      && masterInfo(static_cast<MasterId>(newMasterId)) == nullptr
      && !rebuildOracle()) {
    initialized_ = false;
    result.diagnostics = oracle_diagnostics_;
    addDiagnostic(Severity::Fatal,
                  "TargetMasterRegistrationFailed",
                  "target master could not be added to the repair oracle");
    return result;
  }
  const MasterInfo* replacement = newMasterId >= 0
                                      ? masterInfo(static_cast<MasterId>(
                                            newMasterId))
                                      : nullptr;
  if (replacement == nullptr || replacement->isFiller
      || replacement->width != replacementWidth
      || replacement->height != replacementHeight) {
    addDiagnostic(Severity::Fatal,
                  "TargetMasterUnknown",
                  "the validated target master is absent from the rebuilt "
                  "snapshot");
    return result;
  }

  TargetPlace target;
  target.instanceId = targetInstanceId;
  target.masterId = static_cast<MasterId>(newMasterId);
  target.rowId = requestedRowId;
  target.x = requestedX;
  target.orientation = requestedOrientation;

  const bool samePlacement = target.rowId == targetRowId
                             && target.x == targetX
                             && target.orientation == targetOrientation;

  // 2) Initial snapshot: the new target place with ZERO filler changes.
  // Snapshot and every later engine baseline/candidate go through this same
  // object -> one consistent oracle worldview.
  OracleRequest snapshotRequest;
  snapshotRequest.requestId = 0;
  snapshotRequest.targetPlace = target;
  snapshotRequest.guardRegion = initialInfluence;
  log_.msg("engine",
           cat("snapshot: target inst=", target.instanceId, " newMaster=",
               target.masterId, " guard=",
               show(snapshotRequest.guardRegion)));

  const OracleResult snapshot = checkPlaceWithOverlay(snapshotRequest);
  if (snapshot.status != OracleStatus::Checked) {
    for (const Diagnostic& diagnostic : snapshot.diagnostics) {
      result.diagnostics.push_back(toPublicDiagnostic(diagnostic));
    }
    addDiagnostic(Severity::Fatal,
                  "SnapshotFailed",
                  "checker rejected the target-place snapshot request");
    return result;
  }
  if (snapshot.violations.empty()) {
    result.hasSolution = true;  // legal as-is: empty change list
    addDiagnostic(Severity::Info,
                  "NoRepairNeeded",
                  "the new target place is already legal; no filler changes");
    return result;
  }
  if (!samePlacement) {
    addDiagnostic(Severity::Warning,
                  "UnsupportedTargetMove",
                  "filler repair supports same-position target master swaps "
                  "only");
    return result;
  }

  // 3) Pure search over this object's data-source and oracle interfaces.
  FillerRepairRequest request;
  request.targetPlace = target;
  request.violations = snapshot.violations;
  internal::FillerRepairPlanner planner(*this, *this, config_.repair);
  const FillerRepairResult planned = planner.repair(request);

  result.hasSolution = planned.hasSolution;
  for (const Diagnostic& diagnostic : planned.diagnostics) {
    result.diagnostics.push_back(toPublicDiagnostic(diagnostic));
  }
  if (!planned.hasSolution) {
    // Keep the public boundary atomic even if an internal search path ever
    // reports exploratory records together with failure.
    return result;
  }

  // The planner request, checker request and public result all use this same
  // FillerCellRecord wire. Validate the accepted records, then copy directly.
  for (const FillerCellRecord& change : planned.changes) {
    if (!change.cell_id_.isValid() || !change.new_lib_cell_.isValid()) {
      result.hasSolution = false;
      result.changes.clear();
      addDiagnostic(Severity::Fatal,
                    "MappingLost",
                    "accepted filler record has an invalid UDM id mapping");
      return result;
    }
  }
  result.changes = planned.changes;
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

ipl::Diagnostic toPublicDiagnostic(const Diagnostic& diagnostic)
{
  return ipl::Diagnostic{
      diagnostic.code,
      cat(severityName(diagnostic.severity), ": ", diagnostic.message)};
}

}  // namespace

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

namespace {

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

FillerRepairEngine::Impl::Impl(Grid* grid, Network* network)
    : grid_(grid), network_(network), log_(false)
{
}

FillerRepairEngine::Impl::~Impl() = default;

bool FillerRepairEngine::Impl::init(eUNL::PhysDesMgr* desMgr,
                                    const fillerSetting& fillerSettings)
{
  if (init_attempted_) {
    return false;
  }
  init_attempted_ = true;
  des_mgr_ = desMgr;
  if (!bindInfrastructure(desMgr, fillerSettings)) {
    return false;
  }

  initialized_ = rebuildOracle();
  if (!initialized_) {
    init_diagnostics_.insert(init_diagnostics_.end(),
                             oracle_diagnostics_.begin(),
                             oracle_diagnostics_.end());
    return false;
  }
  precheck_domains_ =
      std::make_unique<PrecheckDomains>(buildPrecheckDomains(grid_));
  return true;
}

void FillerRepairEngine::Impl::setDebugLogging(bool enabled)
{
  debug_logging_ = enabled;
  config_.verbose = enabled;
  config_.repair.verbose = enabled;
  log_.setEnabled(enabled);
}

ipl::CheckResult FillerRepairEngine::Impl::precheck() const
{
  ipl::CheckResult result;
  if (!initialized_ || precheck_domains_ == nullptr) {
    result.isLegal = false;
    result.diagnostics = init_diagnostics_;
    result.diagnostics.push_back(
        {"precheck_not_initialized",
         "warning: init() must succeed before placement precheck"});
    return result;
  }
  const std::vector<internal::CoverageFinding> findings
      = findGapAndOverlap(des_mgr_, network_, *precheck_domains_);
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

ipl::CheckResult FillerRepairEngine::Impl::localPrecheck(
    const Region& influence) const
{
  ipl::CheckResult result;
  result.isLegal = true;
  if (!initialized_ || precheck_domains_ == nullptr) {
    result.isLegal = false;
    result.diagnostics.push_back(
        {"precheck_not_initialized",
         "warning: init() must succeed before placement precheck"});
    return result;
  }

  // Legal spans (absolute DBU) come from the cached Grid domain. The by-row
  // snapshot identifies the nodes to inspect, while status, origin and master
  // size are read live from PhysDesMgr. The caller must update() after any
  // placement/master commit: live reads catch changes to indexed nodes but do
  // not discover a node moved in from a different snapshot row.
  std::vector<internal::PlacementCoverageRow> coverageRows;
  for (const PrecheckDomains::Row& domainRow : precheck_domains_->rows) {
    if (domainRow.id < influence.rowLo || domainRow.id > influence.rowHi) {
      continue;
    }
    internal::PlacementCoverageRow coverageRow;
    coverageRow.rowId = domainRow.id;
    coverageRow.legalSpans = domainRow.legalSpans;
    for (const PlacedInstance& inst : instancesInRow(domainRow.id)) {
      if (static_cast<size_t>(inst.id) >= udm_refs_.size()
          || !udm_refs_[inst.id].has_value()) {
        continue;
      }
      const eUNL::PhysCell cell =
          des_mgr_->getPhysCell(udm_refs_[inst.id]->cellId);
      if (!cell.isValid()) {
        continue;
      }
      const eUNL::PhysObjStatus status = cell.getStatus();
      if (status != eUNL::PhysObjStatus::PLACED
          && status != eUNL::PhysObjStatus::LOC_FIXED) {
        continue;
      }
      const eUTL::Point2D origin = cell.getOrigin();
      const int64_t cellYl = origin.getY().getStorage();
      const int64_t cellYh
          = cellYl + cell.getPhysMaster().getHeight().getStorage();
      if (cellYl >= domainRow.yh || cellYh <= domainRow.yl) {
        continue;  // node no longer overlaps this row since the snapshot
      }
      const int64_t cellXl = origin.getX().getStorage();
      const int64_t cellXh =
          cellXl + cell.getPhysMaster().getWidth().getStorage();
      coverageRow.placedSpans.push_back({cellXl, cellXh});
    }
    coverageRows.push_back(std::move(coverageRow));
  }

  const std::vector<internal::CoverageFinding> findings
      = internal::findCoverageFindings(std::move(coverageRows));
  result.isLegal = findings.empty();
  for (const internal::CoverageFinding& finding : findings) {
    const char* status = internal::coverageFindingStatus(finding.kind);
    result.diagnostics.push_back(
        {status,
         cat("warning: placement ", status, " row=", finding.rowId,
             " x=[", finding.span.xl, ",", finding.span.xh,
             ") -> filler repair blocked in target influence rows")});
  }
  return result;
}

void FillerRepairEngine::Impl::failInit(const std::string& status,
                                        const std::string& message)
{
  init_diagnostics_.push_back({status, message});
}

bool FillerRepairEngine::Impl::bindInfrastructure(
    eUNL::PhysDesMgr* desMgr,
    const fillerSetting& fillerSettings)
{
  if (grid_ == nullptr || network_ == nullptr) {
    failInit("missing_infrastructure",
             cat("fatal: missing initialized Grid or Network: grid=",
                 grid_ != nullptr, " network=", network_ != nullptr));
    return false;
  }
  if (desMgr == nullptr) {
    failInit("missing_phys_des_mgr",
             cat("fatal: missing PhysDesMgr: gridSiteWidth=",
                 grid_->getSiteWidth().v,
                 " gridRows=", grid_->getRowCount().v,
                 " networkNodes=", network_->getNodes().size(),
                 " networkMasters=", network_->getMasters().size()));
    return false;
  }
  eUNL::Design* settingDesign = fillerSettings.getDesign();
  eUNL::PhysDesMgr* settingDesMgr
      = settingDesign != nullptr ? settingDesign->getPhysDesMgr() : nullptr;
  if (settingDesign == nullptr || settingDesMgr != desMgr) {
    failInit("design_mismatch",
             cat("fatal: fillerSetting and PhysDesMgr describe different "
                 "designs: settingDesign=",
                 static_cast<const void*>(settingDesign),
                 " settingPhysDesMgr=",
                 static_cast<const void*>(settingDesMgr),
                 " requestedPhysDesMgr=", static_cast<const void*>(desMgr)));
    return false;
  }
  eUNL::Design* activeDesign
      = eUNL::Session::getSession().getCurrentDesign();
  eUNL::PhysDesMgr* activeDesMgr
      = activeDesign != nullptr ? activeDesign->getPhysDesMgr() : nullptr;
  if (activeDesign == nullptr || activeDesMgr != desMgr) {
    failInit("active_design_mismatch",
             cat("fatal: PhysDesMgr is not the Session current design: "
                 "activeDesign=",
                 static_cast<const void*>(activeDesign),
                 " activePhysDesMgr=",
                 static_cast<const void*>(activeDesMgr),
                 " requestedPhysDesMgr=", static_cast<const void*>(desMgr),
                 " settingDesign=", static_cast<const void*>(settingDesign)));
    return false;
  }
  if (fillerSettings.getFillerPhysCells().empty()) {
    failInit("empty_filler_allow_list",
             cat("fatal: fillerSetting::getFillerPhysCells() is empty: "
                 "settingDesign=",
                 static_cast<const void*>(settingDesign),
                 " networkNodes=", network_->getNodes().size(),
                 " networkMasters=", network_->getMasters().size()));
    return false;
  }
  if (network_->getNodes().empty() || network_->getMasters().empty()) {
    failInit("empty_infrastructure",
             cat("fatal: Grid/Network must be initialized by dpl2 before "
                 "repair: networkNodes=",
                 network_->getNodes().size(),
                 " networkMasters=", network_->getMasters().size(),
                 " gridSiteWidth=", grid_->getSiteWidth().v,
                 " gridRows=", grid_->getRowCount().v,
                 " gridSitesPerRow=", grid_->getRowSiteCount().v,
                 " configuredFillers=",
                 fillerSettings.getFillerPhysCells().size()));
    return false;
  }
  size_t nodeIndex = 0;
  for (const auto& node : network_->getNodes()) {
    if (node == nullptr || node->getMaster() == nullptr
        || node->getMaster()->getPhysLibCell() == nullptr) {
      failInit("invalid_network_node",
               cat("fatal: Network contains an incomplete node/master "
                   "mapping: nodeVectorIndex=",
                   nodeIndex, " nodePresent=", node != nullptr,
                   " masterPresent=",
                   node != nullptr && node->getMaster() != nullptr,
                   " physMasterPresent=",
                   node != nullptr && node->getMaster() != nullptr
                       && node->getMaster()->getPhysLibCell() != nullptr,
                   " networkNodes=", network_->getNodes().size(),
                   " networkMasters=", network_->getMasters().size()));
      return false;
    }
    const eUNL::PhysCell cell = desMgr->getPhysCell(node->getDbInst());
    if (!cell.isValid()
        || cell.getPhysMaster().getLibCellId()
               != node->getMaster()->getDbMaster()) {
      const int expectedLibCell
          = node->getMaster()->getDbMaster().getIndexValue();
      const int actualLibCell = cell.isValid()
                                    ? cell.getPhysMaster()
                                          .getLibCellId()
                                          .getIndexValue()
                                    : -1;
      failInit("infrastructure_design_mismatch",
               cat("fatal: Network node does not match active PhysDesMgr: "
                   "nodeVectorIndex=",
                   nodeIndex, " node=", node->getId(),
                   " leafCell=", node->getDbInst().getIndexValue(),
                   " physCellValid=", cell.isValid(),
                   " networkMaster=", node->getMaster()->getId(),
                   " expectedLibCell=", expectedLibCell,
                   " actualLibCell=", actualLibCell,
                   " nodeType=", static_cast<int>(node->getType()),
                   " nodePlaced=", node->isPlaced(),
                   " nodeFixed=", node->isFixed()));
      return false;
    }
    ++nodeIndex;
  }

  filler_masters_ = fillerSettings.getFillerPhysCells();
  size_t configuredIndex = 0;
  for (const eLIB::PhysLibCell* master : filler_masters_) {
    if (master == nullptr) {
      failInit("null_filler_master",
               cat("fatal: getFillerPhysCells() returned null: "
                   "configuredIndex=",
                   configuredIndex,
                   " configuredCount=", filler_masters_.size()));
      ++configuredIndex;
      continue;
    }
    if (!ensureMasterRegistered(*master)) {
      failInit("filler_master_registration_failed",
               cat("fatal: configured filler master could not be registered "
                   "in Network: configuredIndex=",
                   configuredIndex, " {", masterDebug(*master),
                   "} networkMasters=", network_->getMasters().size(),
                   " gridSiteWidth=", grid_->getSiteWidth().v,
                   " gridRows=", grid_->getRowCount().v));
    }
    ++configuredIndex;
  }
  return init_diagnostics_.empty();
}

bool FillerRepairEngine::Impl::ensureMasterRegistered(
    const eLIB::PhysLibCell& master)
{
  if (network_ == nullptr || grid_ == nullptr) {
    return false;
  }
  if (network_->getMaster(master.getLibCellId()) != nullptr) {
    return true;
  }
  // This is the only infrastructure-version-sensitive registration call in
  // fillerRepair. A destination with a different addMaster signature adapts
  // this one private seam; planner/oracle code remains unchanged.
  return network_->addMaster(master, grid_) != nullptr;
}

bool FillerRepairEngine::Impl::rebuildOracle()
{
  checker_.reset();
  oracle_diagnostics_.clear();
  config_.verbose = debug_logging_;
  config_.repair.verbose = debug_logging_;
  log_.setEnabled(debug_logging_);
  checker_ = std::make_unique<ipl::ImplantLayerChecker>(grid_, network_);
  buildPlannerData();
  bool checkerReady = true;
  for (const ipl::Diagnostic& diagnostic : checker_->getDiags()) {
    if (isNonBlockingCheckerInitDiagnostic(diagnostic)) {
      log_.msg("engine",
               cat("non-blocking checker init diagnostic: ",
                   diagnostic.status, " ", diagnostic.message));
      continue;
    }
    checkerReady = false;
    oracle_diagnostics_.push_back(
        {diagnostic.status,
         cat("fatal: checker initialization: ", diagnostic.message,
             "; context{networkNodes=", network_->getNodes().size(),
             " networkMasters=", network_->getMasters().size(),
             " configuredFillers=", filler_masters_.size(),
             " nonPadRows=", row_list_.size(),
             " engineSiteWidth=", site_width_,
             " gridSiteWidth=", grid_->getSiteWidth().v,
             " checkerSiteWidth=", checker_->siteWidth(), "}")});
    log_.msg("engine",
             cat("blocking checker init diagnostic: ", diagnostic.status,
                 " ", diagnostic.message));
  }
  if (checkerReady && isReady()) {
    return true;
  }
  for (const Diagnostic& diagnostic : setup_diagnostics_) {
    oracle_diagnostics_.push_back(toPublicDiagnostic(diagnostic));
  }
  return false;
}

FillerRepairEngine::FillerRepairEngine(Grid* grid, Network* network)
    : grid_(grid),
      network_(network),
      impl_(std::make_unique<Impl>(grid, network))
{
}

FillerRepairEngine::~FillerRepairEngine() = default;

void FillerRepairEngine::setDebugLogging(bool enabled)
{
  debug_logging_ = enabled;
  impl_->setDebugLogging(enabled);
}

bool FillerRepairEngine::init(eUNL::PhysDesMgr* desMgr,
                              const fillerSetting& fillerSettings)
{
  return impl_->init(desMgr, fillerSettings);
}

bool FillerRepairEngine::update(eUNL::PhysDesMgr* desMgr,
                                const fillerSetting& fillerSettings)
{
  auto replacement = std::make_unique<Impl>(grid_, network_);
  replacement->setDebugLogging(debug_logging_);
  const bool initialized = replacement->init(desMgr, fillerSettings);
  impl_ = std::move(replacement);
  return initialized;
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

RepairOutcome FillerRepairEngine::repair(const ipl::CheckRequest& request)
{
  return impl_->repair(request);
}

}  // namespace fillerRepair
}  // namespace dpl2
