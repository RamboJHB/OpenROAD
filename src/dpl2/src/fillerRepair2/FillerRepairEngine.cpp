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
  void reset(Grid* grid, Network* network)
  {
    checker_ = std::make_unique<ipl::ImplantLayerChecker>(grid, network);
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
  // Gap/overlap coverage restricted to the rows a repair may edit.
  ipl::CheckResult localPrecheck(const Region& influence) const;
  RepairOutcome repair(const ipl::CheckRequest& request);

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
// Grows `table` so `id` is a valid index (ids can exceed the presized
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

const char* operationName(dpl2::OpType operation)
{
  switch (operation) {
    case dpl2::OpType::Replace: return "Replace";
    case dpl2::OpType::Delete: return "Delete";
    case dpl2::OpType::Add: return "Add";
  }
  return "Unknown";
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

// Node carry it, assigned from the configured filler list. The payload
    MasterInfo info;
    info.id = id;
    info.width = cell->getWidth().getStorage();
    info.height = heightInRows(cell->getHeight().getStorage());
// One filler authority (infrastructure). The configured allow-list is a
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
    placement_.masters[id] = PlacementSnapshot::MasterRef{info, cell->getLibCellId()};
  }

// --- candidate universe: fillerSetting only, resolved to Network master
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
    addProblem(Severity::Fatal, "NoConfiguredFillerMaster",
               cat("fillerSetting::getFillerPhysCells() yields no usable "
                   "filler master: configuredCount=",
                   fillerMasters.size(),
                   " acceptedCount=", placement_.fillerMasterIds.size(),
                   " networkMasters=", network->getMasters().size()));
  }

// --- placed instances: Grid supplies row and column (trusted, no
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
  if (diagnostic.status == "skipped_phys_status") {
    return true;
  }

// Missing rules are safe only for an implant layer unused by EVERY master
  if (diagnostic.status != "missing_rule_parameter"
      && diagnostic.status != "skipped_missing_rule_parameter") {
    return false;
  }
  if (des_mgr_ == nullptr || network_ == nullptr || checker_overlay_.get() == nullptr) {
    return false;
  }

  const auto& masters = network_->getMasters();
  log_.checkpoint(
      "checker-diag",
      cat("used-layer scan begin: status=", diagnostic.status,
          " masters=", masters.size()));
  std::set<std::string> usedLayerNames;
  const eLIB::TechLib& tech = des_mgr_->getTopTech();
  for (size_t masterIndex = 0; masterIndex < masters.size(); ++masterIndex) {
    if (masterIndex % 100 == 0) {
      log_.checkpoint("checker-diag",
                      cat("used-layer scan progress: masterIndex=", masterIndex,
                          '/', masters.size()));
    }
    const auto& masterPtr = masters[masterIndex];
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
  log_.checkpoint("checker-diag",
                  cat("used-layer scan done: masters=", masters.size(),
                      " usedLayers=", usedLayerNames.size()));

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

// --- seam 2: turning checker answers into oracle results --------------------
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
    const PlacementSnapshot::UdmRef& ref = placement_.instances[instanceId]->udm;
    record.cell_data_ = dpl2::CellData{ref.cellId};
    record.orig_lib_cell_ = ref.libCellId;
    record.x_ = ref.originX;
    record.y_ = ref.originY;
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

  if (log_.enabled()) {
    const auto describeMaster = [this](MasterId masterId) {
      const MasterInfo* info = masterInfo(masterId);
      return info != nullptr
                 ? cat("{networkId=", masterId, " found=1 width=", info->width,
                       " height=", info->height, " filler=", info->isFiller,
                       " vt=", info->vt, " bottomPolarity=",
                       polarityName(info->bottomBandPolarity), '}')
                 : cat("{networkId=", masterId, " found=0}");
    };
    log_.msg(
        "proposal",
        cat("batch payload: inputRequests=", requests.size(),
            " passedProposals=", changes.size(),
            " precheckFiltered=", requests.size() - changes.size(),
            " target{instance=", target.instanceId,
            " master=", target.masterId, " row=", target.rowId,
            " col=", target.colId,
            " orient=", orientationName(target.orientation),
            " masterInfo=", describeMaster(target.masterId),
            "} guard{planner=", show(first.guardRegion),
            " checkerDbu=[", guard.getXL().getStorage(), ',',
            guard.getYL().getStorage(), ",", guard.getXH().getStorage(), ',',
            guard.getYH().getStorage(), "]}"));
    for (size_t proposalIndex = 0; proposalIndex < changes.size();
         ++proposalIndex) {
      const size_t sourceIndex = legalIndices[proposalIndex];
      const OracleRequest& sourceRequest = requests[sourceIndex];
      log_.msg("proposal",
               cat("proposal[", proposalIndex,
                   "]: sourceRequestIndex=", sourceIndex,
                   " requestId=", sourceRequest.requestId,
                   " changes=", changes[proposalIndex].size()));
      for (size_t changeIndex = 0;
           changeIndex < changes[proposalIndex].size();
           ++changeIndex) {
        const CellChangeRecord& change
            = changes[proposalIndex][changeIndex];
        const eUNL::LeafCellID* cellId
            = cellChangeRecordLeafCellId(change);
        const std::string* cellName
            = std::get_if<std::string>(&change.cell_data_);
        const int nodeId
            = cellId != nullptr ? network_->getNodeId(*cellId) : -1;
        const Node* node = nodeId >= 0 ? network_->getNode(nodeId) : nullptr;
        const PlacedInstance* snapshot
            = nodeId >= 0 ? instance(static_cast<InstanceId>(nodeId)) : nullptr;
        const int originalMasterId
            = network_->getMasterId(change.orig_lib_cell_);
        const int replacementMasterId
            = network_->getMasterId(change.new_lib_cell_);
        const int nodeMasterId
            = node != nullptr && node->getMaster() != nullptr
                  ? node->getMaster()->getId()
                  : -1;
        const std::string cellData
            = cellId != nullptr
                  ? cat("LeafCellID{index=", cellId->getIndexValue(),
                        " valid=", cellId->isValid(), "}")
                  : cellName != nullptr
                        ? cat("name{value=\"", *cellName, "\"}")
                        : std::string("unknown");
        const std::string nodeData
            = node != nullptr
                  ? cat("{found=1 id=", node->getId(),
                        " master=", nodeMasterId,
                        " type=", static_cast<int>(node->getType()),
                        " filler=", node->isFiller(),
                        " stdCell=", node->isStdCell(),
                        " placed=", node->isPlaced(),
                        " left=", node->getLeft().v,
                        " bottom=", node->getBottom().v,
                        " orient=", orientationName(node->getOrient()), '}')
                  : std::string("{found=0}");
        const std::string snapshotData
            = snapshot != nullptr
                  ? cat("{found=1 id=", snapshot->id,
                        " master=", snapshot->masterId,
                        " row=", snapshot->rowId, " x=", snapshot->x,
                        " orient=",
                        orientationName(toUdmOrient(snapshot->orientation)),
                        " filler=", snapshot->isFiller, '}')
                  : std::string("{found=0}");
        log_.msg(
            "proposal",
            cat("proposal[", proposalIndex, "] change[", changeIndex,
                "]: op=", operationName(change.op_), '(',
                static_cast<int>(change.op_), ") cell=", cellData,
                " nodeId=", nodeId,
                " record{origLib=", change.orig_lib_cell_.getIndexValue(),
                " newLib=", change.new_lib_cell_.getIndexValue(),
                " x=", change.x_.getStorage(),
                " y=", change.y_.getStorage(),
                " orient=", orientationName(change.orientation_),
                "} node=", nodeData, " snapshot=", snapshotData,
                " originalMaster=", describeMaster(originalMasterId),
                " replacementMaster=", describeMaster(replacementMasterId)));
      }
    }
    log_.checkpoint("proposal", "batch payload complete");
  }

  log_.checkpoint("checker-call",
                  cat("overlay begin: candidates=", changes.size(),
                      " targetInst=", target.instanceId));
  const std::vector<ipl::CheckResult> raw
      = checker_overlay_.check(target, guard, changes);
  log_.checkpoint("checker-call",
                  cat("overlay done: candidates=", changes.size(),
                      " results=", raw.size()));

// Ordered correlation is the final checker's entire batch protocol. Any
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
  log_.checkpoint("repair", "enter: source=checker");

  RepairOutcome result;
  struct RepairTraceGuard
  {
    const DebugLog& log;
    const RepairOutcome& result;
    ~RepairTraceGuard()
    {
      log.checkpoint("repair",
                     cat("exit: hasSolution=", result.hasSolution,
                         " changes=", result.changes.size(),
                         " diagnostics=", result.diagnostics.size()));
    }
  } traceGuard{log_, result};
  const auto addDiagnostic = [this, &result](Severity severity,
                                             const std::string& code,
                                             const std::string& message) {
    result.diagnostics.push_back(
        toPublicDiagnostic(makeDiag(severity, code, message)));
    log_.checkpoint("repair",
                    cat("diagnostic: severity=", static_cast<int>(severity),
                        " code=", code, " message=", message));
  };
  log_.checkpoint(
      "repair",
      cat("checker request: instance=", request.instanceId,
          " master=", request.masterId, " row=", request.rowId,
          " col=", request.colId,
          " orient=", orientationName(request.orientation)));

  if (repair_active_.exchange(true, std::memory_order_acq_rel)) {
    addDiagnostic(Severity::Fatal,
                  "ReentrantRepair",
                  "repair() re-entered on one FillerRepairEngine");
    return result;
  }
  log_.checkpoint("repair", "entry lock acquired");
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
  log_.checkpoint("repair",
                  cat("engine state accepted: initialized=", initialized_));

  const int targetId = request.instanceId;
  log_.checkpoint("repair", cat("target id resolved: targetId=", targetId));
  const PlacedInstance* inst =
      targetId >= 0 ? instance(static_cast<InstanceId>(targetId)) : nullptr;
  log_.checkpoint("repair",
                  cat("target snapshot lookup done: found=", inst != nullptr));
  if (inst == nullptr) {
    addDiagnostic(Severity::Fatal,
                  "UnknownTarget",
                  "target is not a placed node in this engine snapshot");
    return result;
  }
  if (static_cast<InstanceId>(request.instanceId) != inst->id
      || request.masterId < 0 || request.rowId < 0 || request.colId < 0
      || !supportedOrientation(request.orientation)) {
    addDiagnostic(Severity::Fatal,
                  "InvalidCheckRequest",
                  "checker supplied an invalid target placement request");
    return result;
  }
  log_.checkpoint("repair", "checker request fields accepted");
  const eUNL::LeafCellID targetCell = placement_.instances[inst->id]->udm.cellId;
  log_.checkpoint("repair",
                  cat("replacement master lookup begin: masterId=",
                      request.masterId));
  const Master* requestedMaster =
      network_->getMaster(static_cast<int>(request.masterId));
  log_.checkpoint("repair",
                  cat("replacement master lookup done: found=",
                      requestedMaster != nullptr));
  const eLIB::PhysLibCell* newMaster =
      requestedMaster != nullptr ? requestedMaster->getPhysLibCell() : nullptr;
  log_.checkpoint("repair",
                  cat("replacement physical master lookup done: found=",
                      newMaster != nullptr));
  if (newMaster == nullptr) {
    addDiagnostic(Severity::Fatal,
                  "TargetMasterUnknown",
                  "target replacement master is absent from Network");
    return result;
  }
  log_.checkpoint("repair", "target cell and replacement master resolved");
  const MasterInfo* oldMaster = masterInfo(inst->masterId);
  log_.checkpoint("repair",
                  cat("current master snapshot lookup done: masterId=",
                      inst->masterId, " found=", oldMaster != nullptr));
  if (oldMaster == nullptr) {
    addDiagnostic(Severity::Fatal,
                  "TargetMasterUnknown",
                  "the target's current master is absent from the snapshot");
    return result;
  }
  const Node* targetNode = network_->getNode(targetId);
  const bool targetIsStdCell = targetNode != nullptr
                               && targetNode->isStdCell()
                               && !inst->isFiller;
  const bool targetIsFiller = targetNode != nullptr
                              && targetNode->isFiller()
                              && inst->isFiller;
  const bool replacementIsStdCell
      = filler_settings_ != nullptr
        && isStandardCellMaster(*newMaster, *filler_settings_);
  log_.checkpoint(
      "repair",
      cat("target classification done: nodeFound=", targetNode != nullptr,
          " targetIsStdCell=", targetIsStdCell,
          " targetIsFiller=", targetIsFiller,
          " settingsFound=", filler_settings_ != nullptr,
          " replacementIsStdCell=", replacementIsStdCell));
  if ((!targetIsStdCell && !targetIsFiller) || filler_settings_ == nullptr
      || !replacementIsStdCell) {
    addDiagnostic(Severity::Fatal,
                  "TargetNotStdCell",
                  "target must be a standard cell or filler and the "
                  "replacement master must be a standard cell");
    return result;
  }
  log_.checkpoint("repair", "replacement geometry lookup begin");
  const DbCoord replacementWidth = newMaster->getWidth().getStorage();
  const DbCoord replacementHeight = grid_->gridHeight(*newMaster).v;
  log_.checkpoint("repair",
                  cat("replacement geometry lookup done: old=",
                      oldMaster->width, 'x', oldMaster->height,
                      " replacement=", replacementWidth, 'x',
                      replacementHeight));
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

  const RowId requestedRowId = static_cast<RowId>(request.rowId);
  const DbCoord requestedX =
      static_cast<DbCoord>(request.colId) * placement_.siteWidth;
  const Orient requestedOrientation = toPlannerOrient(request.orientation);
  const Region initialInfluence = snapshotGuard(
      requestedRowId, requestedX, replacementWidth, replacementHeight);
  log_.checkpoint("repair",
                  cat("target influence ready: targetInst=", targetInstanceId,
                      " influence=", show(initialInfluence)));

// Fail before registering an uninstantiated replacement master. The
  log_.checkpoint("repair",
                  cat("precheck begin: targetInst=", targetInstanceId,
                      " influence=", show(initialInfluence)));
  const ipl::CheckResult placement = localPrecheck(initialInfluence);
  log_.checkpoint("repair",
                  cat("precheck done: legal=", placement.isLegal,
                      " diagnostics=", placement.diagnostics.size()));
  if (!placement.isLegal) {
    result.diagnostics = placement.diagnostics;
    addDiagnostic(Severity::Warning,
                  "PrecheckFailed",
                  "placement precheck failed in the target influence rows; "
                  "filler repair was skipped");
    return result;
  }

// DePlace may not have imported an uninstantiated target replacement yet.
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
    const eUNL::PhysCell physical = des_mgr_->getPhysCell(targetCell);
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
            "checker",
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
  OracleRequest snapshotRequest;
  snapshotRequest.requestId = 0;
  snapshotRequest.targetPlace = target;
  snapshotRequest.guardRegion = initialInfluence;
  log_.msg("engine",
           cat("snapshot: target inst=", target.instanceId, " newMaster=",
               target.masterId, " guard=",
               show(snapshotRequest.guardRegion)));

  log_.checkpoint("repair",
                  cat("snapshot checker begin: targetInst=", target.instanceId,
                      " guard=", show(snapshotRequest.guardRegion)));
  const OracleResult snapshot = checkPlaceWithOverlay(snapshotRequest);
  log_.checkpoint("repair",
                  cat("snapshot checker done: status=",
                      static_cast<int>(snapshot.status),
                      " violations=", snapshot.violations.size()));
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
  FillerRepairRequest plannerRequest;
  plannerRequest.targetPlace = target;
  plannerRequest.violations = snapshot.violations;
  log_.checkpoint("repair",
                  cat("planner begin: targetInst=", target.instanceId,
                      " violations=", plannerRequest.violations.size()));
  internal::RepairPlanner planner(*this, *this, repair_config_);
  const FillerRepairResult planned = planner.repair(plannerRequest);
  log_.checkpoint("repair",
                  cat("planner done: hasSolution=", planned.hasSolution,
                      " changes=", planned.changes.size(),
                      " diagnostics=", planned.diagnostics.size()));

  result.hasSolution = planned.hasSolution;
  for (const Diagnostic& diagnostic : planned.diagnostics) {
    result.diagnostics.push_back(toPublicDiagnostic(diagnostic));
  }
  if (!planned.hasSolution) {
// Keep the public boundary atomic even if an internal search path ever
    return result;
  }

// The planner request, checker request and public result all use this same
  for (const CellChangeRecord& change : planned.changes) {
    const eUNL::LeafCellID* cellId = cellChangeRecordLeafCellId(change);
    if (change.op_ != dpl2::OpType::Replace || cellId == nullptr
        || !cellId->isValid() || !change.new_lib_cell_.isValid()
        || network_->getNodeId(*cellId) == targetId) {
      result.hasSolution = false;
      result.changes.clear();
      addDiagnostic(Severity::Fatal,
                    "MappingLost",
                    "accepted filler record has an invalid UDM id mapping "
                    "or edits the replaced target");
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
  log_.checkpoint("engine-init", "init begin");
  if (init_attempted_) {
    log_.checkpoint("engine-init", "init rejected: already attempted");
    return false;
  }
  init_attempted_ = true;
  des_mgr_ = desMgr;
  log_.checkpoint("engine-init", "infrastructure bind begin");
  if (!bindInfrastructure(desMgr, fillerSettings)) {
    log_.checkpoint("engine-init", "infrastructure bind failed");
    return false;
  }
  log_.checkpoint("engine-init", "infrastructure bind done");

  initialized_ = rebuildOracle();
  log_.checkpoint("engine-init",
                  cat("rebuild oracle returned: ready=", initialized_));
  if (!initialized_) {
    init_diagnostics_.insert(init_diagnostics_.end(),
                             oracle_diagnostics_.begin(),
                             oracle_diagnostics_.end());
    return false;
  }
  log_.checkpoint("engine-init", "init done");
  return true;
}

ipl::CheckResult FillerRepairEngine::Impl::localPrecheck(
    const Region& influence) const
{
  ipl::CheckResult result;
  result.isLegal = true;
  if (!initialized_ || des_mgr_ == nullptr || grid_ == nullptr
      || network_ == nullptr) {
    result.isLegal = false;
    result.diagnostics.push_back(
        {"precheck_not_initialized",
         "warning: initialized infrastructure is required before placement "
         "precheck"});
    return result;
  }

// Regional coverage gate only (spec: repair checks the rows it can edit;
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
  (void) master;
  if (network_ == nullptr || grid_ == nullptr || filler_settings_ == nullptr) {
    return false;
  }
  return true;

}

bool FillerRepairEngine::Impl::rebuildOracle()
{
  log_.checkpoint("engine-init", "rebuild oracle begin");
  checker_overlay_.clear();
  oracle_diagnostics_.clear();
  repair_config_.verbose = log_.enabled();
  // The private checker receives the manager already retained by Grid.
  log_.checkpoint("engine-init", "private checker construction begin");
  checker_overlay_.reset(grid_, network_);
  log_.checkpoint(
      "engine-init",
      cat("private checker construction done: diagnostics=",
          checker_overlay_.get()->getDiags().size(),
          " layers=", checker_overlay_.get()->getLayers().size()));
  log_.checkpoint("engine-init", "placement snapshot begin");
  buildPlannerData();
  log_.checkpoint("engine-init", "placement snapshot done");
  const auto& checkerDiagnostics = checker_overlay_.get()->getDiags();
  log_.checkpoint("engine-init",
                  cat("checker diagnostic scan begin: count=",
                      checkerDiagnostics.size()));
  bool checkerReady = true;
  for (size_t diagnosticIndex = 0;
       diagnosticIndex < checkerDiagnostics.size();
       ++diagnosticIndex) {
    const ipl::Diagnostic& diagnostic = checkerDiagnostics[diagnosticIndex];
    log_.checkpoint("engine-init",
                    cat("checker diagnostic classify begin: index=",
                        diagnosticIndex, '/', checkerDiagnostics.size(),
                        " status=", diagnostic.status));
    const bool nonBlocking = isNonBlockingCheckerInitDiagnostic(diagnostic);
    log_.checkpoint("engine-init",
                    cat("checker diagnostic classify done: index=",
                        diagnosticIndex, '/', checkerDiagnostics.size(),
                        " nonBlocking=", nonBlocking));
    if (nonBlocking) {
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
  log_.checkpoint("engine-init",
                  cat("checker diagnostic scan done: ready=", checkerReady));
  if (checkerReady && isReady()) {
    log_.checkpoint("engine-init", "rebuild oracle done: ready=1");
    return true;
  }
  for (const Diagnostic& diagnostic : setup_diagnostics_) {
    oracle_diagnostics_.push_back(toPublicDiagnostic(diagnostic));
  }
  log_.checkpoint("engine-init", "rebuild oracle done: ready=0");
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
    : impl_(std::make_unique<Impl>(grid, network))
{
}

FillerRepairEngine::~FillerRepairEngine() = default;

bool FillerRepairEngine::init(eUNL::PhysDesMgr* desMgr,
                              const fillerSetting& fillerSettings)
{
  return impl_->init(desMgr, fillerSettings);
}

RepairOutcome FillerRepairEngine::repair(const ipl::CheckRequest& request)
{
  return impl_->repair(request);
}

}  // namespace fillerRepair
}  // namespace dpl2
