// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerRepair/FillerRepairEngine.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fillerRepair/RepairPlanner.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

namespace dpl2 {
namespace fillerRepair {

namespace {

// Immutable planner-facing placement state. Engine rebuilds this component
// when the Network master universe changes; the planner only sees it through
// PlacementView accessors.
struct PlacementSnapshot
{
  struct MasterRef
  {
    MasterInfo info;
    eLIB::LibCellID libCellId;
  };
  struct UdmRef
  {
    eUNL::LeafCellID cellId;
    eLIB::LibCellID libCellId;
    eUTL::UvDist originX;
    eUTL::UvDist originY;
    eUTL::PhysOrientation orientation;
  };
  struct InstanceRef
  {
    PlacedInstance placed;
    UdmRef udm;
  };
  struct RowFrame
  {
    DbCoord yLo = 0;
    DbCoord yHi = 0;
  };

  void clear()
  {
    siteWidth = 0;
    rowHeight = 0;
    defaultHaloX = 0;
    coreXl = 0;
    rows.clear();
    rowFrames.clear();
    legalSpans.clear();
    masters.clear();
    instances.clear();
    byRow.clear();
    fillerMasterIds.clear();
  }

  DbCoord siteWidth = 0;
  DbCoord rowHeight = 0;
  DbCoord defaultHaloX = 0;
  DbCoord coreXl = 0;
  std::vector<RowId> rows;
  std::vector<RowFrame> rowFrames;
  mutable std::vector<std::optional<std::vector<XInterval>>> legalSpans;
  std::vector<std::optional<MasterRef>> masters;
  std::vector<std::optional<InstanceRef>> instances;
  std::vector<std::vector<PlacedInstance>> byRow;
  std::vector<MasterId> fillerMasterIds;
};

struct CandidateStats
{
  size_t checked = 0;
  size_t notFiller = 0;
  size_t unknownVt = 0;
  size_t sameVt = 0;
  size_t widthMismatch = 0;
  size_t heightMismatch = 0;
  size_t polarityMismatch = 0;
  size_t compatible = 0;
};

// Precomputed compatibility catalog. This turns an O(configured masters)
// scan for every filler in every window into one initialization pass and,
// more importantly, proves up front when swap-only repair has no move.
class FillerCandidateCatalog
{
 public:
  void build(const PlacementSnapshot& placement, const DebugLog& log)
  {
    candidatesByMaster_.clear();
    statsByMaster_.clear();
    hasPlacedCandidate_ = false;
    aggregate_ = CandidateStats{};
    candidatesByMaster_.resize(placement.masters.size());
    statsByMaster_.resize(placement.masters.size());

    for (size_t sourceIndex = 0; sourceIndex < placement.masters.size();
         ++sourceIndex) {
      if (!placement.masters[sourceIndex].has_value()) {
        continue;
      }
      const MasterInfo& source = placement.masters[sourceIndex]->info;
      if (!source.isFiller) {
        continue;
      }
      CandidateStats& stats = statsByMaster_[sourceIndex];
      for (const MasterId candidateId : placement.fillerMasterIds) {
        if (candidateId < 0
            || static_cast<size_t>(candidateId) >= placement.masters.size()
            || !placement.masters[candidateId].has_value()) {
          continue;
        }
        const MasterInfo& candidate
            = placement.masters[candidateId]->info;
        if (candidate.id == source.id) {
          continue;
        }
        ++stats.checked;
        ++aggregate_.checked;
        const bool notFiller = !candidate.isFiller;
        const bool unknownVt = source.vt == kUnknownVt
                               || candidate.vt == kUnknownVt;
        const bool sameVt = !unknownVt && source.vt == candidate.vt;
        const bool widthMismatch = source.width != candidate.width;
        const bool heightMismatch = source.height != candidate.height;
        const bool polarityMismatch
            = source.bottomBandPolarity != candidate.bottomBandPolarity;
        stats.notFiller += notFiller;
        stats.unknownVt += unknownVt;
        stats.sameVt += sameVt;
        stats.widthMismatch += widthMismatch;
        stats.heightMismatch += heightMismatch;
        stats.polarityMismatch += polarityMismatch;
        aggregate_.notFiller += notFiller;
        aggregate_.unknownVt += unknownVt;
        aggregate_.sameVt += sameVt;
        aggregate_.widthMismatch += widthMismatch;
        aggregate_.heightMismatch += heightMismatch;
        aggregate_.polarityMismatch += polarityMismatch;
        if (notFiller || unknownVt || sameVt || widthMismatch
            || heightMismatch || polarityMismatch) {
          continue;
        }
        candidatesByMaster_[sourceIndex].push_back(candidate.id);
        ++stats.compatible;
        ++aggregate_.compatible;
      }
    }

    for (const auto& slot : placement.instances) {
      if (slot.has_value() && slot->placed.isFiller
          && !candidates(slot->placed.masterId).empty()) {
        hasPlacedCandidate_ = true;
        break;
      }
    }
    log.msg("candidate", [&] {
      return cat("catalog ready: configured=",
                 placement.fillerMasterIds.size(),
                 " masterPairChecks=", aggregate_.checked,
                 " compatiblePairs=", aggregate_.compatible,
                 " placedCandidate=", hasPlacedCandidate_,
                 " rejects{notFiller=", aggregate_.notFiller,
                 " unknownVt=", aggregate_.unknownVt,
                 " sameVt=", aggregate_.sameVt,
                 " widthMismatch=", aggregate_.widthMismatch,
                 " heightMismatch=", aggregate_.heightMismatch,
                 " polarityMismatch=", aggregate_.polarityMismatch, '}');
    });
  }

  const std::vector<MasterId>& candidates(MasterId sourceMaster) const
  {
    static const std::vector<MasterId> kEmpty;
    return sourceMaster >= 0
                   && static_cast<size_t>(sourceMaster)
                          < candidatesByMaster_.size()
               ? candidatesByMaster_[sourceMaster]
               : kEmpty;
  }

  const CandidateStats* stats(MasterId sourceMaster) const
  {
    return sourceMaster >= 0
                   && static_cast<size_t>(sourceMaster) < statsByMaster_.size()
               ? &statsByMaster_[sourceMaster]
               : nullptr;
  }

  bool hasPlacedCandidate() const { return hasPlacedCandidate_; }
  const CandidateStats& aggregateStats() const { return aggregate_; }

 private:
  std::vector<std::vector<MasterId>> candidatesByMaster_;
  std::vector<CandidateStats> statsByMaster_;
  CandidateStats aggregate_;
  bool hasPlacedCandidate_ = false;
};

// Owns the private checker and serializes its mutable-const overlay path.
class CheckerOverlayClient
{
 public:
  void clear() { checker_.reset(); }
  void reset(Grid* grid, eUNL::Design* design, Network* network)
  {
    checker_ = std::make_unique<ipl::ImplantLayerChecker>(
        grid, design, network);
  }
  ipl::ImplantLayerChecker* get() { return checker_.get(); }
  const ipl::ImplantLayerChecker* get() const { return checker_.get(); }
  std::vector<ipl::CheckResult> check(
      const ipl::CheckRequest& target,
      const ::Rect& guard,
      const std::vector<ipl::FillerChanges>& changes)
  {
    if (checker_ == nullptr) {
      return std::vector<ipl::CheckResult>();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return checker_->checkPlaceWithOverlays(target, guard, changes);
  }

 private:
  std::unique_ptr<ipl::ImplantLayerChecker> checker_;
  std::mutex mutex_;
};

}  // namespace

class FillerRepairEngine::Impl final : private PlacementView,
                                       private RepairOracle
{
 public:
  Impl(Grid* grid, Network* network)
      : grid_(grid), network_(network), log_(debugLoggingDefault())
  {
  }

  bool init(eUNL::PhysDesMgr* desMgr,
            const fillerSetting& fillerSettings);
  void setDebugLogging(bool enabled);
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
  static constexpr int kSnapshotHaloRows = 1;

  void buildPlannerData();
  bool isReady() const;
  bool isNonBlockingCheckerInitDiagnostic(
      const ipl::Diagnostic& diagnostic) const;
  bool bindInfrastructure(eUNL::PhysDesMgr* desMgr,
                          const fillerSetting& fillerSettings);
  bool ensureMasterRegistered(const eLIB::PhysLibCell& master);
  bool rebuildOracle();
  void failInit(const std::string& status, const std::string& message);
  const std::vector<RowId>& rows() const override { return placement_.rows; }
  DbCoord siteWidth() const override { return placement_.siteWidth; }
  const std::vector<PlacedInstance>& instancesInRow(RowId rowId) const override;
  const PlacedInstance* instance(InstanceId id) const override;
  const MasterInfo* masterInfo(MasterId id) const override;
  MasterCandidateResult getUsableMasterCandidates(
      InstanceId fillerInstanceId) const override;
  const std::vector<MasterId>& fillerMasterIds() const override
  {
    return placement_.fillerMasterIds;
  }
  CellChangeRecord cellChangeRecord(InstanceId instanceId,
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
  eUNL::Design* design_ = nullptr;
  eUNL::PhysDesMgr* des_mgr_ = nullptr;
  const fillerSetting* filler_settings_ = nullptr;
  std::vector<const eLIB::PhysLibCell*> filler_masters_;
  PlacementSnapshot placement_;
  FillerCandidateCatalog candidate_catalog_;
  CheckerOverlayClient checker_overlay_;
  RepairConfig repair_config_;
  DebugLog log_;
  std::vector<Diagnostic> setup_diagnostics_;
  std::vector<ipl::Diagnostic> init_diagnostics_;
  std::vector<ipl::Diagnostic> oracle_diagnostics_;
  const std::vector<XInterval>& legalSpansForRow(RowId rowId) const;
  bool initialized_ = false;
  bool init_attempted_ = false;
  std::atomic<bool> repair_active_{false};
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

std::string orientationName(eUTL::PhysOrientation orientation)
{
  if (orientation == eUTL::PhysOrientationE::R0) return "R0";
  if (orientation == eUTL::PhysOrientationE::R90) return "R90";
  if (orientation == eUTL::PhysOrientationE::R180) return "R180";
  if (orientation == eUTL::PhysOrientationE::R270) return "R270";
  if (orientation == eUTL::PhysOrientationE::MX) return "MX";
  if (orientation == eUTL::PhysOrientationE::MX90) return "MX90";
  if (orientation == eUTL::PhysOrientationE::MY) return "MY";
  if (orientation == eUTL::PhysOrientationE::MY90) return "MY90";
  return cat("unknown(", static_cast<int>(orientation.getValue()), ")");
}

const char* polarityName(BandPolarity polarity)
{
  return polarity == BandPolarity::P ? "P" : "N";
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

bool isStandardCellMaster(const eLIB::PhysLibCell& cell,
                          const fillerSetting& filler_settings)
{
  return cell.getType().isCore() && !cell.getType().isBlock()
         && !filler_settings.isFillerCell(cell.getLibCellId());
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


// --- is this region even placeable? -----------------------------------------
// Answers "is every legal site in these rows covered exactly once" -- no gaps,
// no overlaps. Repair refuses to run on a region that is not, because a swap
// reasoned about a broken placement would be meaningless. Only the rows the
// target can influence are checked; whole-design legality belongs to
// infrastructure, not here.
namespace internal {

enum class CoverageFindingKind
{
  Gap,
  Overlap
};

struct PlacementCoverageRow
{
  RowId rowId = 0;
  std::vector<XInterval> legalSpans;
  std::vector<XInterval> placedSpans;
};

struct CoverageFinding
{
  CoverageFindingKind kind = CoverageFindingKind::Gap;
  RowId rowId = 0;
  XInterval span;
};

inline const char* coverageFindingStatus(CoverageFindingKind kind)
{
  return kind == CoverageFindingKind::Gap ? "Gap" : "Overlap";
}

// Returns findings in row/legal-span/x order. Adjacent findings of the same
// kind in one row are coalesced, including across touching legal spans.
std::vector<CoverageFinding> findCoverageFindings(
    std::vector<PlacementCoverageRow> rows)
{
  std::sort(
      rows.begin(),
      rows.end(),
      [](const PlacementCoverageRow& left, const PlacementCoverageRow& right) {
        return left.rowId < right.rowId;
      });

  std::vector<CoverageFinding> findings;
  for (PlacementCoverageRow& row : rows) {
    std::sort(row.legalSpans.begin(),
              row.legalSpans.end(),
              [](const XInterval& left, const XInterval& right) {
                return left.xl != right.xl ? left.xl < right.xl
                                           : left.xh < right.xh;
              });
    for (const XInterval& legal : row.legalSpans) {
      if (legal.empty()) {
        continue;
      }
      std::vector<DbCoord> cuts{legal.xl, legal.xh};
      std::vector<DbCoord> starts;
      std::vector<DbCoord> ends;
      starts.reserve(row.placedSpans.size());
      ends.reserve(row.placedSpans.size());
      for (const XInterval& span : row.placedSpans) {
        const DbCoord clippedXl = std::max(span.xl, legal.xl);
        const DbCoord clippedXh = std::min(span.xh, legal.xh);
        if (clippedXh <= clippedXl) {
          continue;
        }
        cuts.push_back(clippedXl);
        cuts.push_back(clippedXh);
        starts.push_back(clippedXl);
        ends.push_back(clippedXh);
      }
      std::sort(cuts.begin(), cuts.end());
      cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
      std::sort(starts.begin(), starts.end());
      std::sort(ends.begin(), ends.end());

      size_t nextStart = 0;
      size_t nextEnd = 0;
      int active = 0;
      for (size_t index = 0; index + 1 < cuts.size(); ++index) {
        const DbCoord segmentXl = cuts[index];
        const DbCoord segmentXh = cuts[index + 1];
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
        const CoverageFindingKind kind = active == 0
                                             ? CoverageFindingKind::Gap
                                             : CoverageFindingKind::Overlap;
        if (!findings.empty() && findings.back().kind == kind
            && findings.back().rowId == row.rowId
            && findings.back().span.xh == segmentXl) {
          findings.back().span.xh = segmentXh;
        } else {
          findings.push_back(
              CoverageFinding{kind, row.rowId, {segmentXl, segmentXh}});
        }
      }
    }
  }
  return findings;
}

}  // namespace internal

void FillerRepairEngine::Impl::buildPlannerData()
{
  eUNL::PhysDesMgr* desMgr = des_mgr_;
  Grid* grid = grid_;
  Network* network = network_;
  const ipl::ImplantLayerChecker* checker = checker_overlay_.get();
  const auto& fillerMasters = filler_masters_;
  placement_.clear();
  setup_diagnostics_.clear();
  repair_config_.verbose = log_.enabled();
  if (desMgr == nullptr || grid == nullptr || network == nullptr
      || checker == nullptr) {
    addProblem(Severity::Fatal, "MissingDependency",
               cat("engine initialization dependency is missing: desMgr=",
                   desMgr != nullptr, " grid=", grid != nullptr,
                   " network=", network != nullptr,
                   " checker=", checker != nullptr));
    return;
  }

  // [PORT-ADAPT] The coordinate frame, and the biggest silent-wrongness risk
  // in the port. RowId is the Grid row index and x is relative to the core's
  // left edge, because that is the frame the checker builds its
  // CheckRequest.rowId/colId in -- from these same Grid calls. Both sides
  // must agree; nothing here re-derives or re-validates it.
  //
  // If your Grid indexes rows differently, or measures x from the die rather
  // than the core, every lookup still compiles and every answer is about the
  // wrong place. CHECKER_REPAIR_CONTRACT.md "Row/column frames" spells out
  // the envelope this assumes (no pad row before a standard row, y-sorted
  // rows, one shared row origin X at the core edge).
  placement_.siteWidth = grid->getSiteWidth().v;
  placement_.coreXl = grid->getCore().getXL().getStorage();
  const DbCoord coreYl = grid->getCore().getYL().getStorage();
  const int rowCount = grid->getRowCount().v;
  placement_.rowFrames.reserve(static_cast<size_t>(std::max(rowCount, 0)));
  placement_.rows.reserve(static_cast<size_t>(std::max(rowCount, 0)));
  for (GridY y{0}; y < grid->getRowCount(); ++y) {
    PlacementSnapshot::RowFrame frame;
    frame.yLo = coreYl + grid->gridYToDbu(y).v;
    frame.yHi = coreYl + grid->gridYToDbu(y + 1).v;
    placement_.rowFrames.push_back(frame);
    placement_.rows.push_back(y.v);
  }
  if (!placement_.rowFrames.empty()) {
    placement_.rowHeight = placement_.rowFrames.front().yHi - placement_.rowFrames.front().yLo;
  }
  placement_.byRow.resize(placement_.rowFrames.size());
  placement_.legalSpans.resize(placement_.rowFrames.size());
  if (placement_.siteWidth <= 0 || placement_.rowHeight <= 0 || placement_.rows.empty()) {
    addProblem(Severity::Fatal, "MissingRowGeometry",
               cat("no usable row geometry from Grid: rows=",
                   placement_.rowFrames.size(), " siteWidth=", placement_.siteWidth,
                   " rowHeight=", placement_.rowHeight));
  }

  const auto heightInRows = [this](DbCoord height) {
    return placement_.rowHeight > 0
               ? std::max<DbCoord>((height + placement_.rowHeight - 1) / placement_.rowHeight, 1)
               : 1;
  };
  // --- implant metadata: VT family / band polarity per master, derived from
  // the master's implant shapes exactly like the checker (layer identity via
  // the checker's TechLayerRelativeID, band anchored at the bottommost
  // implant rect -- the rebuildMasterShapes rule).
  // [PORT-ADAPT] The snapshot reads master implant shapes the same way the
  // checker does -- layer identity through the checker's own
  // TechLayerRelativeID, band anchored at the bottommost implant rect. That
  // mirroring is deliberate: the two must agree about which shape sits on
  // which band, or the planner ranks against geometry the checker does not
  // see. If the checker's rebuildMasterShapes rule changes, this changes with
  // it.
  const auto implantLayerOf =
      [&](eLIB::TechLayerRelativeID relId) -> const ipl::Layer* {
    for (const ipl::Layer& layer : checker->getLayers()) {
      if (layer.getTechLayerId() == relId) {
        return &layer;
      }
    }
    return nullptr;
  };

  placement_.masters.resize(network->getMasters().size());
  for (size_t networkMasterIndex = 0;
       networkMasterIndex < network->getMasters().size();
       ++networkMasterIndex) {
    const auto& masterPtr = network->getMasters()[networkMasterIndex];
    const Master* nm = masterPtr.get();
    if (nm == nullptr) {
      addProblem(Severity::Fatal, "NullNetworkMaster",
                 cat("Network master slot ", networkMasterIndex,
                     " is null"));
      continue;
    }
    if (nm->getPhysLibCell() == nullptr) {
      addProblem(Severity::Fatal, "MissingPhysicalMaster",
                 cat("Network master ", nm->getId(),
                     " has no PhysLibCell"));
      continue;
    }
    const eLIB::PhysLibCell* cell = nm->getPhysLibCell();
    const MasterId id = static_cast<MasterId>(nm->getId());
    if (id < 0 || static_cast<size_t>(id) >= network->getMasters().size()) {
      addProblem(Severity::Fatal, "InvalidNetworkMasterId",
                 cat("Network master slot ", networkMasterIndex,
                     " has out-of-range id ", id));
      continue;
    }

    // [PORT-ADAPT] Filler identity comes from infrastructure -- Master and
    // Node carry it, assigned from the configured filler list. The payload
    // never re-derives it from UDM macro flags, and must not start: the two
    // disagree (a CORE_FILLER is also isCore()), and that disagreement was a
    // real bug. Whatever your infrastructure's single filler authority is,
    // this must read it.
    MasterInfo info;
    info.id = id;
    info.width = cell->getWidth().getStorage();
    info.height = heightInRows(cell->getHeight().getStorage());
    // One filler authority (infrastructure). The configured allow-list is a
    // separate concept -- which filler masters may be OFFERED as
    // replacements -- and lives in placement_.fillerMasterIds.
    info.isFiller = nm->isFiller();

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

    ensureSlot(placement_.masters, static_cast<size_t>(id));
    placement_.masters[id]
        = PlacementSnapshot::MasterRef{info, cell->getLibCellId()};
  }

  // --- candidate universe: fillerSetting only, resolved to Network master
  // ids. Entries the Network does not know cannot be validated by the
  // checker either (it builds masters from the Network) -> Warning + skip.
  {
    log_.msg("candidate",
             cat("provider source: fillerSetting configuredCount=",
                 fillerMasters.size(), " networkMasters=",
                 network->getMasters().size()));
    for (size_t configuredIndex = 0;
         configuredIndex < fillerMasters.size();
         ++configuredIndex) {
      const eLIB::PhysLibCell* cell = fillerMasters[configuredIndex];
      if (cell == nullptr) {
        log_.msg("candidate",
                 cat("configured[", configuredIndex,
                     "] physLibCell=null decision=skip"));
        continue;
      }
      const int id = network->getMasterId(cell->getLibCellId());
      log_.msg("candidate", [&] {
        return cat("configured[", configuredIndex, "] {",
                   masterDebug(*cell), "} networkMasterId=", id);
      });
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
        continue;
      }
      const MasterInfo* info = masterInfo(static_cast<MasterId>(id));
      if (info == nullptr) {
        addProblem(Severity::Fatal, "ConfiguredMasterMissingMetadata",
                   cat("configured filler master has no planner metadata: "
                       "configuredIndex=",
                       configuredIndex, " networkMasterId=", id,
                       " {", masterDebug(*cell), "}"));
        continue;
      }
      log_.msg("candidate", [&] {
        return cat("configured[", configuredIndex, "] accepted master=", id,
                   " filler=", info->isFiller, " vt=", info->vt,
                   " width=", info->width, " heightRows=", info->height,
                   " bottom=", polarityName(info->bottomBandPolarity));
      });
      placement_.fillerMasterIds.push_back(static_cast<MasterId>(id));
    }
    std::sort(placement_.fillerMasterIds.begin(), placement_.fillerMasterIds.end());
    placement_.fillerMasterIds.erase(
        std::unique(placement_.fillerMasterIds.begin(), placement_.fillerMasterIds.end()),
        placement_.fillerMasterIds.end());
    log_.msg("candidate", [&] {
      std::string ids;
      for (const MasterId id : placement_.fillerMasterIds) {
        if (!ids.empty()) {
          ids += ',';
        }
        ids += std::to_string(id);
      }
      return cat("provider ready: acceptedCount=", placement_.fillerMasterIds.size(),
                 " masterIds=[", ids, ']');
    });
  }
  if (placement_.fillerMasterIds.empty()) {
    // Empty allow list (or nothing usable in it) means repair could never
    // offer a swap -- fail init instead of failing every later repair.
    addProblem(Severity::Fatal, "NoConfiguredFillerMaster",
               cat("fillerSetting::getFillerPhysCells() yields no usable "
                   "filler master: configuredCount=",
                   fillerMasters.size(),
                   " acceptedCount=", placement_.fillerMasterIds.size(),
                   " networkMasters=", network->getMasters().size()));
  }

  // --- placed instances: Grid supplies row and column (trusted, no
  // cross-frame re-validation); PhysDesMgr supplies status and origin.
  placement_.instances.resize(network->getNodes().size());
  for (size_t networkNodeIndex = 0;
       networkNodeIndex < network->getNodes().size();
       ++networkNodeIndex) {
    const auto& nodePtr = network->getNodes()[networkNodeIndex];
    const Node* node = nodePtr.get();
    if (node == nullptr) {
      addProblem(Severity::Fatal, "NullNetworkNode",
                 cat("Network node slot ", networkNodeIndex, " is null"));
      continue;
    }
    if (node->getMaster() == nullptr
        || node->getMaster()->getPhysLibCell() == nullptr) {
      addProblem(Severity::Fatal, "MissingNodeMaster",
                 cat("Network node ", node->getId(),
                     " has no usable master"));
      continue;
    }
    const eLIB::PhysLibCell* cell = node->getMaster()->getPhysLibCell();
    const eUNL::LeafCellID lcId = node->getDbInst();
    const eUNL::PhysCell physCell = desMgr->getPhysCell(lcId);
    if (!physCell.isValid()) {
      addProblem(Severity::Fatal, "MissingPhysicalCell",
                 cat("Network node ", node->getId(),
                     " has no valid PhysCell mapping"));
      continue;
    }
    const eUNL::PhysObjStatus status = physCell.getStatus();
    if (status != eUNL::PhysObjStatus::PLACED
        && status != eUNL::PhysObjStatus::LOC_FIXED) {
      continue;
    }
    const RowId rowId = static_cast<RowId>(grid->gridSnapDownY(node).v);
    if (rowId < 0 || rowId >= static_cast<RowId>(placement_.rowFrames.size())) {
      continue;  // outside the core grid: context the planner cannot edit
    }
    const eUTL::Point2D origin = physCell.getOrigin();
    const DbCoord x = origin.getX().getStorage() - placement_.coreXl;

    const MasterId masterId = static_cast<MasterId>(node->getMaster()->getId());
    if (masterInfo(masterId) == nullptr) {
      addProblem(Severity::Fatal, "MissingNodeMasterMetadata",
                 cat("Network node ", node->getId(), " references master ",
                     masterId, " without planner metadata"));
      continue;
    }
    const MasterInfo& info = placement_.masters[static_cast<size_t>(masterId)]->info;
    const bool isFiller = node->isFiller();

    const InstanceId id = static_cast<InstanceId>(node->getId());
    if (id < 0 || static_cast<size_t>(id) >= network->getNodes().size()) {
      addProblem(Severity::Fatal, "InvalidNetworkNodeId",
                 cat("Network node slot ", networkNodeIndex,
                     " has out-of-range id ", id));
      continue;
    }
    PlacedInstance placed{id,
                          masterId,
                          rowId,
                          x,
                          toPlannerOrient(physCell.getOrient()),
                          isFiller};
    if (placed.isFiller && info.vt == kUnknownVt) {
      addProblem(Severity::Warning, "FillerWithoutVt",
                 cat("placed filler ", id, " uses master ", masterId,
                     " without implant VT metadata -> not swappable"));
    }
    ensureSlot(placement_.instances, static_cast<size_t>(id));
    placement_.instances[id] = PlacementSnapshot::InstanceRef{
        placed,
        {lcId, cell->getLibCellId(), origin.getX(), origin.getY(),
         physCell.getOrient()}};
    // Multi-row instances appear in every row they occupy (one shared x
    // frame, so the copy keeps the same x).
    const DbCoord heightRows = std::max<DbCoord>(info.height, 1);
    for (DbCoord offset = 0; offset < heightRows; ++offset) {
      const RowId row = rowId + static_cast<RowId>(offset);
      if (row >= static_cast<RowId>(placement_.byRow.size())) {
        break;
      }
      PlacedInstance rowCopy = placed;
      rowCopy.rowId = row;
      placement_.byRow[row].push_back(rowCopy);
    }
  }
  for (std::vector<PlacedInstance>& list : placement_.byRow) {
    std::sort(list.begin(), list.end(),
              [](const PlacedInstance& a, const PlacedInstance& b) {
                return a.x != b.x ? a.x < b.x : a.id < b.id;
              });
  }

  candidate_catalog_.build(placement_, log_);
  if (!candidate_catalog_.hasPlacedCandidate()) {
    addProblem(Severity::Warning,
               "NoCompatibleFillerCandidate",
               "no placed filler has a compatible configured VT master");
  }

  // The initial target snapshot only needs enough horizontal context for the
  // checker's rules and the replacement-filler universe. Repair-window guards
  // are sized later from the actual two-cell instance ring, so using the
  // widest placed master here is both redundant and pathological when that
  // master is a hard macro.
  //
  // ONE source decides rule reach: the checker. `getMaxRuleValue()` is
  // literally how far, in sites, its own scan looks. Anything narrower cuts a
  // run off at the edge of the snapshot, and the checker then reports a
  // min-width violation that does not exist in the design.
  //
  // We deliberately do NOT compute reach ourselves from TechLayer width and
  // spacing. That was the same number derived twice, and the two answers drift:
  // the checker builds LEF58 rules whose minValue can exceed both raw values.
  // It already cost us that bug once. And the converse is free -- an implant
  // width the checker never turned into a rule is a width it never scans for.
  {
    DbCoord maxFillerWidth = 0;
    MasterId maxFillerMaster = -1;
    for (const MasterId id : placement_.fillerMasterIds) {
      const MasterInfo* master = masterInfo(id);
      if (master != nullptr && master->width > maxFillerWidth) {
        maxFillerWidth = master->width;
        maxFillerMaster = id;
      }
    }

    // [PORT-ADAPT] ImplantLayerChecker::getMaxRuleValue(). The whole guard
    // sizing rests on this one number meaning "how far, in SITES, my scan
    // looks". If your checker spells it differently, or returns DBU, fix it
    // here and nowhere else -- but do not replace it with your own reach
    // formula. See the block above for why that is the bug this avoids.
    //
    // Sites here, DBU everywhere else: this multiplication is the only place
    // the two units meet.
    const int reachSites = checker->getMaxRuleValue();
    const DbCoord checkerReach =
        static_cast<DbCoord>(reachSites) * placement_.siteWidth;

    const bool fillerWidthWins = maxFillerWidth > checkerReach;
    placement_.defaultHaloX = std::max(checkerReach, maxFillerWidth);
    log_.msg(
        "engine",
        cat("default halo source: kind=",
            fillerWidthWins ? "FILLER_MASTER_WIDTH" : "CHECKER_RULE_REACH",
            " checkerReach{sites=", reachSites, " dbu=", checkerReach,
            "} widestConfiguredFiller{master=", maxFillerMaster,
            " dbu=", maxFillerWidth,
            "} defaultHaloX=", placement_.defaultHaloX));
  }

  const auto placedCount = std::count_if(
      placement_.instances.begin(), placement_.instances.end(),
      [](const auto& slot) { return slot.has_value(); });
  const auto masterCount = std::count_if(
      placement_.masters.begin(), placement_.masters.end(),
      [](const auto& slot) { return slot.has_value(); });
  log_.msg("engine",
           cat("placement view: ", placedCount, " node(s), ",
               masterCount, " master(s), ", placement_.fillerMasterIds.size(),
               " configured filler master(s), siteWidth=", placement_.siteWidth,
               " defaultHaloX=", placement_.defaultHaloX));
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
  if (des_mgr_ == nullptr || network_ == nullptr
      || checker_overlay_.get() == nullptr) {
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

  for (const ipl::Layer& layer : checker_overlay_.get()->getLayers()) {
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
  return rowId >= 0 && static_cast<size_t>(rowId) < placement_.byRow.size()
             ? placement_.byRow[rowId]
             : emptyInstances();
}

const PlacedInstance* FillerRepairEngine::Impl::instance(InstanceId id) const
{
  return id >= 0 && static_cast<size_t>(id) < placement_.instances.size()
                 && placement_.instances[id].has_value()
             ? &placement_.instances[id]->placed
             : nullptr;
}

const MasterInfo* FillerRepairEngine::Impl::masterInfo(MasterId id) const
{
  return id >= 0 && static_cast<size_t>(id) < placement_.masters.size()
                 && placement_.masters[id].has_value()
             ? &placement_.masters[id]->info
             : nullptr;
}

MasterCandidateResult FillerRepairEngine::Impl::getUsableMasterCandidates(
    InstanceId fillerInstanceId) const
{
  MasterCandidateResult result;
  const PlacedInstance* placed = instance(fillerInstanceId);
  if (placed == nullptr) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "UnknownInstance",
        cat("instance ", fillerInstanceId, " not found")));
    return result;
  }
  if (!placed->isFiller) {
    result.diagnostics.push_back(makeDiag(
        Severity::Warning, "NotAFiller",
        cat("instance ", fillerInstanceId, " is not a filler")));
    return result;
  }
  const MasterInfo* current = masterInfo(placed->masterId);
  if (current == nullptr || !current->isFiller) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error, "UnknownMaster",
        cat("invalid current filler master for instance ",
            fillerInstanceId)));
    return result;
  }

  result.candidates = candidate_catalog_.candidates(placed->masterId);
  if (!result.candidates.empty()) {
    return result;
  }

  const CandidateStats* stats = candidate_catalog_.stats(placed->masterId);
  result.diagnostics.push_back(makeDiag(
      Severity::Info,
      "NoCompatibleFillerMaster",
      stats != nullptr
          ? cat("filler ", fillerInstanceId, " master ", placed->masterId,
                " has no configured replacement matching size, different VT, "
                "and polarity: checked=", stats->checked,
                " rejects{notFiller=", stats->notFiller,
                " unknownVt=", stats->unknownVt,
                " sameVt=", stats->sameVt,
                " widthMismatch=", stats->widthMismatch,
                " heightMismatch=", stats->heightMismatch,
                " polarityMismatch=", stats->polarityMismatch, '}')
          : cat("filler ", fillerInstanceId, " master ", placed->masterId,
                " is absent from the candidate catalog")));
  return result;
}

// --- seam 2: turning checker answers into oracle results --------------------

ipl::CheckRequest FillerRepairEngine::Impl::toCheckRequest(const TargetPlace& place) const
{
  ipl::CheckRequest request;
  request.instanceId = static_cast<ipl::InstanceId>(place.instanceId);
  request.masterId = static_cast<ipl::MasterId>(place.masterId);
  request.rowId = static_cast<ipl::RowId>(place.rowId);
  request.colId = static_cast<ipl::ColId>(
      placement_.siteWidth > 0 ? place.x / placement_.siteWidth : 0);
  request.orientation = toUdmOrient(place.orientation);
  return request;
}

::Rect FillerRepairEngine::Impl::toGuardRect(const Region& region) const
{
  // The checker's isInGuard uses the synthetic frame y = rowId * rowHeight;
  // (rowHi+1)*rowHeight - 1 keeps a touching adjacent row out.
  const DbCoord yl = static_cast<DbCoord>(region.rowLo) * placement_.rowHeight;
  const DbCoord yh =
      static_cast<DbCoord>(region.rowHi + 1) * placement_.rowHeight - 1;
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

// [PORT-ADAPT] CellChangeRecord's shape. This is the one place the search's
// dense ids become UDM handles, so it is where a change to the shared record
// -- a new field, a renamed one, a different CellData alternative -- has to
// be absorbed. Every field must be filled: `orientation_` in particular is
// read by the checker when it evaluates the swapped filler, so leaving it
// default makes MX-placed rows evaluate the wrong implant band.
CellChangeRecord FillerRepairEngine::Impl::cellChangeRecord(
    InstanceId instanceId,
    MasterId newMasterId) const
{
  CellChangeRecord record{dpl2::OpType::Replace,
                          dpl2::CellData{eUNL::LeafCellID(0, 0)},
                          eUTL::UvDist(static_cast<int64_t>(0)),
                          eUTL::UvDist(static_cast<int64_t>(0)),
                          eLIB::LibCellID(0, 0),
                          eLIB::LibCellID(0, 0),
                          eUTL::PhysOrientation(
                              eUTL::PhysOrientationE::R0)};
  const bool haveRef = instanceId >= 0
                       && static_cast<size_t>(instanceId) < placement_.instances.size()
                       && placement_.instances[instanceId].has_value();
  if (haveRef) {
    const PlacementSnapshot::UdmRef& ref
        = placement_.instances[instanceId]->udm;
    record.cell_data_ = dpl2::CellData{ref.cellId};
    record.orig_lib_cell_ = ref.libCellId;
    record.origin_x_ = ref.originX;
    record.origin_y_ = ref.originY;
    record.orientation_ = ref.orientation;
  }
  if (masterInfo(newMasterId) != nullptr) {
    record.new_lib_cell_ = placement_.masters[newMasterId]->libCellId;
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
    for (const CellChangeRecord& change : requests[i].fillerChanges) {
      const eUNL::LeafCellID* cellId = cellChangeRecordLeafCellId(change);
      const Node* node = cellId != nullptr ? network_->getNode(*cellId)
                                           : nullptr;
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

  // [PORT-ADAPT] ImplantLayerChecker::checkPlaceWithOverlays -- THE call the
  // whole feature is built on, and the one with real semantics behind it, not
  // just a signature:
  //
  //   * one FillerChanges = one atomic candidate;
  //   * results correlate BY INPUT ORDER, and results.size() must equal
  //     candidates.size() -- a short or long batch invalidates all of it, and
  //     the code below refuses the whole batch rather than guess;
  //   * a candidate is judged against the guard region, so the guard is part
  //     of the question (see quantizeGuard in RepairPlanner.cpp).
  //
  // If your checker batches differently, adapt here and keep those three
  // properties. Dropping the count check to "salvage" a partial batch would
  // silently mis-attribute answers to candidates.
  const std::vector<ipl::CheckResult> raw
      = checker_overlay_.check(target, guard, changes);
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

  // [PORT-ADAPT] Depends on checker BEHAVIOUR, not on a signature, so it will
  // compile happily while being wrong. If your checker does not repeat its
  // init diagnostics into every result, this strip is harmless. If it repeats
  // them differently -- a different count, or not as a leading run -- every
  // candidate comes back illegal and repair silently never finds anything.
  // The engine classifies that sequence once at init; check it matches.
  //
  // The checker repeats its start-up diagnostics in every single result --
  // twice, in fact, once directly and once inside the embedded region result
  // -- and counts them against isLegal. Those are harmless things it noticed
  // at init, like a missing rule parameter on a layer no master uses.
  //
  // So strip every leading copy of that known sequence, and judge each
  // candidate only on what this request produced. Without it one benign
  // start-up note would make every candidate illegal forever. Anything
  // structural never gets this far: rebuildOracle() already failed closed.
  const auto& initDiags = checker_overlay_.get()->getDiags();
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

// --- the entry point --------------------------------------------------------

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
  Region guard;
  guard.x = XInterval{x - placement_.defaultHaloX, x + width + placement_.defaultHaloX};
  const RowId minRow = placement_.rows.empty() ? 0 : placement_.rows.front();
  const RowId maxRow = placement_.rows.empty() ? 0 : placement_.rows.back();
  guard.rowLo = std::max<RowId>(
      minRow, rowId - static_cast<RowId>(kSnapshotHaloRows));
  guard.rowHi = std::min<RowId>(
      maxRow,
      rowId + static_cast<RowId>(heightRows) - 1
          + static_cast<RowId>(kSnapshotHaloRows));
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
    targetCell = placement_.instances[inst->id]->udm.cellId;
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
      || filler_settings_ == nullptr
      || !isStandardCellMaster(*newMaster, *filler_settings_)) {
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
  const MasterId oldMasterId = inst->masterId;
  const BandPolarity oldBottomBandPolarity
      = oldMaster->bottomBandPolarity;
  const RowId targetRowId = inst->rowId;
  const DbCoord targetX = inst->x;
  const Orient targetOrientation = inst->orientation;

  const RowId requestedRowId = checkerRequest.has_value()
                                   ? static_cast<RowId>(checkerRequest->rowId)
                                   : targetRowId;
  const DbCoord requestedX = checkerRequest.has_value()
                                 ? static_cast<DbCoord>(checkerRequest->colId)
                                       * placement_.siteWidth
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

  if (log_.enabled()) {
    const eUNL::PhysCell physical = des_mgr_->getPhysCell(*targetCell);
    const DbCoord coreYl = grid_->getCore().getYL().getStorage();
    const DbCoord requestYRelative
        = grid_->gridYToDbu(GridY{requestedRowId}).v;
    const DbCoord requestYAbsolute = coreYl + requestYRelative;
    std::string matchingRows;
    if (physical.isValid()) {
      const DbCoord physicalY = physical.getOrigin().getY().getStorage();
      RowId physicalRowId = 0;
      for (const eUNL::PhysRow& row : des_mgr_->getPhysRowIter()) {
        const DbCoord rowYl = row.getOrigin().getY().getStorage();
        const DbCoord rowYh
            = (row.getOrigin().getY() + row.getSite().getHeight())
                  .getStorage();
        const bool containsPhysical
            = physicalY >= rowYl && physicalY < rowYh;
        const bool containsRequest
            = requestYAbsolute >= rowYl && requestYAbsolute < rowYh;
        if (containsPhysical || containsRequest) {
          if (!matchingRows.empty()) {
            matchingRows += "; ";
          }
          matchingRows += cat(
              "{iterationId=", physicalRowId,
              " site=\"", row.getSite().getName(), "\"",
              " pad=", row.getSite().getIsPad(),
              " y=[", rowYl, ",", rowYh, ")",
              " height=", row.getSite().getHeight().getStorage(),
              " orient=", orientationName(row.getOrient()),
              " containsPhysical=", containsPhysical,
              " containsRequest=", containsRequest, "}");
        }
        ++physicalRowId;
      }
    }
    if (matchingRows.empty()) {
      matchingRows = "none";
    }
    const MasterInfo* requestedMasterInfo = masterInfo(target.masterId);
    log_.msg(
        "engine",
        cat("snapshot frame: request{source=",
            checkerRequest.has_value() ? "checker" : "direct",
            " inst=", target.instanceId,
            " master=", target.masterId,
            " row=", requestedRowId,
            " col=",
            placement_.siteWidth > 0 ? requestedX / placement_.siteWidth : -1,
            " xDbu=", requestedX,
            " yRelativeDbu=", requestYRelative,
            " yAbsoluteDbu=", requestYAbsolute,
            " orient=", orientationName(toUdmOrient(requestedOrientation)),
            "} engineSnapshot{master=", oldMasterId,
            " row=", targetRowId,
            " xDbu=", targetX,
            " orient=", orientationName(toUdmOrient(targetOrientation)),
            " samePlacement=", samePlacement,
            "} network{master=", targetNode->getMaster()->getId(),
            " left=", targetNode->getLeft().v,
            " bottom=", targetNode->getBottom().v,
            " gridRow=", grid_->gridSnapDownY(targetNode).v,
            " gridCol=", grid_->gridX(targetNode).v,
            " orient=", orientationName(targetNode->getOrient()),
            "} physical{valid=", physical.isValid(),
            physical.isValid()
                ? cat(" masterLib=",
                      physical.getPhysMaster()
                          .getLibCellId()
                          .getIndexValue(),
                      " origin=(",
                      physical.getOrigin().getX().getStorage(), ",",
                      physical.getOrigin().getY().getStorage(), ")",
                      " orient=", orientationName(physical.getOrient()))
                : std::string(),
            "} masterBands{oldBottom=",
            polarityName(oldBottomBandPolarity),
            " requestedBottom=",
            requestedMasterInfo != nullptr
                ? polarityName(requestedMasterInfo->bottomBandPolarity)
                : "missing",
            " requestedHeightRows=",
            requestedMasterInfo != nullptr ? requestedMasterInfo->height : -1,
            "} matchingPhysRows=[", matchingRows, "]"));
  }

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
  if (!candidate_catalog_.hasPlacedCandidate()) {
    const CandidateStats& stats = candidate_catalog_.aggregateStats();
    addDiagnostic(
        Severity::Warning,
        "NoCompatibleFillerCandidate",
        cat("swap-only repair has no placed filler with a configured master "
            "matching size, different VT, and polarity: checked=",
            stats.checked, " compatible=", stats.compatible,
            " rejects{notFiller=", stats.notFiller,
            " unknownVt=", stats.unknownVt,
            " sameVt=", stats.sameVt,
            " widthMismatch=", stats.widthMismatch,
            " heightMismatch=", stats.heightMismatch,
            " polarityMismatch=", stats.polarityMismatch, '}'));
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
  internal::RepairPlanner planner(*this, *this, repair_config_);
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
  // CellChangeRecord wire. Validate the accepted records, then copy directly.
  for (const CellChangeRecord& change : planned.changes) {
    const eUNL::LeafCellID* cellId = cellChangeRecordLeafCellId(change);
    if (change.op_ != dpl2::OpType::Replace || cellId == nullptr
        || !cellId->isValid() || !change.new_lib_cell_.isValid()) {
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
  return true;
}

void FillerRepairEngine::Impl::setDebugLogging(bool enabled)
{
  repair_config_.verbose = enabled;
  log_.setEnabled(enabled);
}

ipl::CheckResult FillerRepairEngine::Impl::localPrecheck(
    const Region& influence) const
{
  ipl::CheckResult result;
  result.isLegal = true;
  if (!initialized_ || design_ == nullptr || des_mgr_ == nullptr
      || grid_ == nullptr
      || network_ == nullptr) {
    result.isLegal = false;
    result.diagnostics.push_back(
        {"precheck_not_initialized",
         "warning: initialized infrastructure is required before placement "
         "precheck"});
    return result;
  }

  // Regional coverage gate only (spec: repair checks the rows it can edit;
  // whole-design placement legality is infrastructure's own gate). Legal
  // spans come lazily from Grid pixels for exactly these rows; placed spans
  // are read live from PhysDesMgr for the row's snapshot nodes.
  std::vector<internal::PlacementCoverageRow> coverageRows;
  const RowId rowLo = std::max<RowId>(influence.rowLo, 0);
  const RowId rowHi = std::min<RowId>(
      influence.rowHi, static_cast<RowId>(placement_.rowFrames.size()) - 1);
  for (RowId rowId = rowLo; rowId <= rowHi; ++rowId) {
    const PlacementSnapshot::RowFrame& frame = placement_.rowFrames[static_cast<size_t>(rowId)];
    internal::PlacementCoverageRow coverageRow;
    coverageRow.rowId = rowId;
    coverageRow.legalSpans = legalSpansForRow(rowId);
    for (const PlacedInstance& inst : instancesInRow(rowId)) {
      if (inst.id < 0 || static_cast<size_t>(inst.id) >= placement_.instances.size()
          || !placement_.instances[inst.id].has_value()) {
        continue;
      }
      const eUNL::PhysCell cell =
          des_mgr_->getPhysCell(placement_.instances[inst.id]->udm.cellId);
      if (!cell.isValid()) {
        continue;
      }
      const eUNL::PhysObjStatus status = cell.getStatus();
      if (status != eUNL::PhysObjStatus::PLACED
          && status != eUNL::PhysObjStatus::LOC_FIXED) {
        continue;
      }
      const eUTL::Point2D origin = cell.getOrigin();
      const DbCoord cellYl = origin.getY().getStorage();
      const DbCoord cellYh
          = cellYl + cell.getPhysMaster().getHeight().getStorage();
      if (cellYl >= frame.yHi || cellYh <= frame.yLo) {
        continue;  // node no longer overlaps this row since the snapshot
      }
      const DbCoord cellXl = origin.getX().getStorage() - placement_.coreXl;
      const DbCoord cellXh =
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

const std::vector<XInterval>& FillerRepairEngine::Impl::legalSpansForRow(
    RowId rowId) const
{
  auto& cached = placement_.legalSpans[static_cast<size_t>(rowId)];
  if (cached.has_value()) {
    return *cached;
  }
  // A valid pixel not reserved by halo/padding requires exactly one placed
  // cover; maximal runs of such pixels form the legal spans (core-left-
  // relative like every planner x).
  std::vector<XInterval> spans;
  bool inSpan = false;
  DbCoord spanStart = 0;
  for (GridX x{0}; x < grid_->getRowSiteCount(); ++x) {
    const Pixel* pixel = grid_->gridPixel(x, GridY{rowId});
    const bool requiresCoverage = pixel != nullptr && pixel->is_valid
                                  && pixel->padding_reserved_by == nullptr;
    if (requiresCoverage && !inSpan) {
      inSpan = true;
      spanStart = static_cast<DbCoord>(x.v) * placement_.siteWidth;
    } else if (!requiresCoverage && inSpan) {
      inSpan = false;
      spans.push_back({spanStart, static_cast<DbCoord>(x.v) * placement_.siteWidth});
    }
  }
  if (inSpan) {
    spans.push_back({spanStart,
                     static_cast<DbCoord>(grid_->getRowSiteCount().v)
                         * placement_.siteWidth});
  }
  cached = std::move(spans);
  return *cached;
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
  // [PORT-ADAPT] Grid::getDesMgr(). The engine refuses to bind to a manager
  // other than the one Grid was initialized with, because Grid, Network, the
  // checker and this engine must all describe ONE design revision. If your
  // Grid does not retain its manager, give it an accessor -- do not delete
  // this check; a mismatch here is silently wrong answers, not a crash.
  eUNL::PhysDesMgr* const gridDesMgr = grid_->getDesMgr();
  if (gridDesMgr == nullptr) {
    failInit("missing_grid_phys_des_mgr",
             "fatal: Grid does not retain an initialization PhysDesMgr");
    return false;
  }
  if (gridDesMgr != desMgr) {
    failInit("grid_phys_des_mgr_mismatch",
             cat("fatal: engine PhysDesMgr must match Grid's initialization "
                 "manager: gridPhysDesMgr=",
                 static_cast<const void*>(gridDesMgr),
                 " requestedPhysDesMgr=", static_cast<const void*>(desMgr)));
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
  // Per-node Network<->UDM cross-validation was removed deliberately:
  // infrastructure data is trusted as-is, and with lazy initialization the
  // engine is typically created MID-CHECK, while the candidate Node already
  // carries its proposed master ahead of the pending UDM commit
  // (DePlace::isLegal updates the Node before checkDRC). Nodes whose master
  // or physical record is unusable are simply skipped by buildPlannerData.

  design_ = settingDesign;
  filler_settings_ = &fillerSettings;
  filler_masters_ = fillerSettings.getFillerPhysCells();
  for (size_t configuredIndex = 0;
       configuredIndex < filler_masters_.size();
       ++configuredIndex) {
    const eLIB::PhysLibCell* master = filler_masters_[configuredIndex];
    if (master == nullptr) {
      failInit("null_filler_master",
               cat("fatal: getFillerPhysCells() returned null: "
                   "configuredIndex=",
                   configuredIndex,
                   " configuredCount=", filler_masters_.size()));
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
  }
  return init_diagnostics_.empty();
}

bool FillerRepairEngine::Impl::ensureMasterRegistered(
    const eLIB::PhysLibCell& master)
{
  if (network_ == nullptr || grid_ == nullptr || filler_settings_ == nullptr) {
    return false;
  }
  // [PORT-ADAPT] Network::addMaster. This is the ONLY call in the payload
  // whose signature tracks the infrastructure version, and it has already
  // changed twice (it gained the fillerSetting parameter, and its third
  // parameter became const EdgeTypeTable*). If yours differs, fix it here --
  // nothing in the search or the oracle needs to know.
  //
  // The empty edge-type table is on purpose. addMaster dereferences it without
  // a null check, so nullptr is out; an empty one makes it return right after
  // filling in the geometry, which is all the implant oracle reads anyway.
  // This call is mandatory even when the master already exists: addMaster()
  // refreshes Master::isFiller from fillerSetting, the sole filler authority.
  // rebuildOracle() runs after registration and therefore gives the private
  // checker the same refreshed master classification.
  static const EdgeTypeTable kNoEdgeTypes;
  Master* refreshed = network_->addMaster(
      master, *filler_settings_, grid_, &kNoEdgeTypes);
  return refreshed != nullptr
         && refreshed->isFiller()
                == filler_settings_->isFillerCell(master.getLibCellId());
}

bool FillerRepairEngine::Impl::rebuildOracle()
{
  checker_overlay_.clear();
  oracle_diagnostics_.clear();
  repair_config_.verbose = log_.enabled();
  // bindInfrastructure proved the explicit Design, engine and Grid agree.
  checker_overlay_.reset(grid_, design_, network_);
  buildPlannerData();
  bool checkerReady = true;
  for (const ipl::Diagnostic& diagnostic : checker_overlay_.get()->getDiags()) {
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
             " nonPadRows=", placement_.rows.size(),
             " engineSiteWidth=", placement_.siteWidth,
             " gridSiteWidth=", grid_->getSiteWidth().v,
             " checkerSiteWidth=", checker_overlay_.get()->siteWidth(), "}")});
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

void reportRepairUnavailable(const char* reason)
{
  DebugLog(debugLoggingDefault())
      .msg("engine",
           cat("repair unavailable: ",
               reason != nullptr ? reason : "unspecified reason"));
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
