// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerRepair/FillerRepairEngine.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fillerRepair/FillerRetiler.h>
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
    std::string siteName;
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
  std::vector<std::vector<XInterval>> legalSpans;
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
    log.block("candidate",
              "Candidate compatibility catalog",
              {{"configured masters",
                cat(placement.fillerMasterIds.size())},
               {"master-pair checks", cat(aggregate_.checked)},
               {"compatible pairs", cat(aggregate_.compatible)},
               {"placed candidate", cat(hasPlacedCandidate_)},
               {"reject: not filler", cat(aggregate_.notFiller)},
               {"reject: unknown VT", cat(aggregate_.unknownVt)},
               {"reject: same VT", cat(aggregate_.sameVt)},
               {"reject: width mismatch", cat(aggregate_.widthMismatch)},
               {"reject: height mismatch", cat(aggregate_.heightMismatch)},
               {"reject: polarity mismatch",
                cat(aggregate_.polarityMismatch)}});
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

// One request-local filler introduced by geometric retiling. Negative ids are
// deliberately local to a repair call; neither Network nor UDM is modified.
struct LayoutAddition
{
  InstanceId instanceId = -1;
  PlacedInstance placed;
  CellChangeRecord record;
};

ipl::FillerChanges mergeFillerChanges(const ipl::FillerChanges& fixed,
                                      const ipl::FillerChanges& variable)
{
  ipl::FillerChanges merged = fixed;
  for (const CellChangeRecord& change : variable) {
    const std::string* name
        = change.op_ == OpType::Add
              ? std::get_if<std::string>(&change.cell_data_)
              : nullptr;
    if (name != nullptr) {
      const auto existing = std::find_if(
          merged.begin(), merged.end(), [&](const CellChangeRecord& item) {
            const std::string* itemName
                = item.op_ == OpType::Add
                      ? std::get_if<std::string>(&item.cell_data_)
                      : nullptr;
            return itemName != nullptr && *itemName == *name;
          });
      if (existing != merged.end()) {
        *existing = change;
        continue;
      }
    }
    merged.push_back(change);
  }
  return merged;
}

// Per-call placement view after applying target placement, filler deletions,
// and one exact-cover tiling. It is immutable after construction and therefore
// safe for concurrent repairs even though the shared engine snapshot is read.
class LayoutPlacementView final : public PlacementView
{
 public:
  LayoutPlacementView(
      const PlacementView& base,
      TargetPlace target,
      std::set<InstanceId> removed,
      std::vector<LayoutAddition> additions,
      std::unordered_map<MasterId, eLIB::LibCellID> masterLibCells)
      : base_(base),
        target_(target),
        removed_(std::move(removed)),
        additions_(std::move(additions)),
        master_lib_cells_(std::move(masterLibCells))
  {
    const MasterInfo* targetMaster = masterInfo(target_.masterId);
    target_instance_ = PlacedInstance{target_.instanceId,
                                      target_.masterId,
                                      target_.rowId,
                                      target_.x,
                                      target_.orientation,
                                      false};

    for (const RowId rowId : base_.rows()) {
      std::vector<PlacedInstance>& row = by_row_[rowId];
      for (const PlacedInstance& placed : base_.instancesInRow(rowId)) {
        if (placed.id != target_.instanceId
            && removed_.count(placed.id) == 0) {
          row.push_back(placed);
        }
      }
    }
    appendToRows(target_instance_, targetMaster);
    for (const LayoutAddition& addition : additions_) {
      synthetic_.emplace(addition.instanceId, addition.placed);
      appendToRows(addition.placed, masterInfo(addition.placed.masterId));
    }
    for (auto& [rowId, row] : by_row_) {
      (void) rowId;
      std::sort(row.begin(), row.end(), [](const PlacedInstance& left,
                                           const PlacedInstance& right) {
        return left.x != right.x ? left.x < right.x : left.id < right.id;
      });
    }
  }

  const std::vector<RowId>& rows() const override { return base_.rows(); }
  DbCoord siteWidth() const override { return base_.siteWidth(); }

  const std::vector<PlacedInstance>& instancesInRow(
      RowId rowId) const override
  {
    const auto found = by_row_.find(rowId);
    return found != by_row_.end() ? found->second : emptyInstances();
  }

  const PlacedInstance* instance(InstanceId id) const override
  {
    if (id == target_.instanceId) {
      return &target_instance_;
    }
    if (removed_.count(id) != 0) {
      return nullptr;
    }
    const auto synthetic = synthetic_.find(id);
    return synthetic != synthetic_.end() ? &synthetic->second
                                         : base_.instance(id);
  }

  const MasterInfo* masterInfo(MasterId id) const override
  {
    return base_.masterInfo(id);
  }

  const std::vector<MasterId>& fillerMasterIds() const override
  {
    return base_.fillerMasterIds();
  }

  MasterCandidateResult getUsableMasterCandidates(
      InstanceId instanceId) const override
  {
    return instanceId >= 0
               ? base_.getUsableMasterCandidates(instanceId)
               : PlacementView::getUsableMasterCandidates(instanceId);
  }

  CellChangeRecord cellChangeRecord(InstanceId instanceId,
                                    MasterId newMasterId) const override
  {
    if (instanceId >= 0) {
      return base_.cellChangeRecord(instanceId, newMasterId);
    }
    const auto addition = std::find_if(
        additions_.begin(),
        additions_.end(),
        [instanceId](const LayoutAddition& item) {
          return item.instanceId == instanceId;
        });
    if (addition == additions_.end()) {
      return CellChangeRecord{};
    }
    CellChangeRecord record = addition->record;
    const auto libCell = master_lib_cells_.find(newMasterId);
    if (libCell != master_lib_cells_.end()) {
      record.new_lib_cell_ = libCell->second;
    }
    return record;
  }

 private:
  void appendToRows(const PlacedInstance& placed, const MasterInfo* master)
  {
    const int heightRows
        = master != nullptr ? std::max<int>(master->height, 1) : 1;
    for (int offset = 0; offset < heightRows; ++offset) {
      PlacedInstance copy = placed;
      copy.rowId += offset;
      by_row_[copy.rowId].push_back(copy);
    }
  }

  const PlacementView& base_;
  TargetPlace target_;
  std::set<InstanceId> removed_;
  std::vector<LayoutAddition> additions_;
  std::unordered_map<MasterId, eLIB::LibCellID> master_lib_cells_;
  PlacedInstance target_instance_;
  std::unordered_map<InstanceId, PlacedInstance> synthetic_;
  std::map<RowId, std::vector<PlacedInstance>> by_row_;
};

// Prefixes every planner candidate with the fixed geometric Delete/Add
// transaction. The real checker still decides all implant legality.
class LayoutOracle final : public RepairOracle
{
 public:
  LayoutOracle(RepairOracle& base,
               const LayoutPlacementView& view,
               ipl::FillerChanges fixed)
      : base_(base), view_(view), fixed_(std::move(fixed))
  {
  }

  OracleResult checkPlaceWithOverlay(const OracleRequest& request) override
  {
    std::vector<OracleResult> results = checkPlaceWithOverlays({request});
    if (results.size() == 1) {
      return std::move(results.front());
    }
    OracleResult failed;
    failed.requestId = request.requestId;
    failed.status = OracleStatus::CheckerError;
    failed.diagnostics.push_back(
        makeDiag(Severity::Fatal,
                 "CheckerProtocolError",
                 "checker returned the wrong result count for one layout "
                 "overlay request"));
    return failed;
  }

  std::vector<OracleResult> checkPlaceWithOverlays(
      const std::vector<OracleRequest>& requests) override
  {
    std::vector<OracleRequest> merged = requests;
    for (OracleRequest& request : merged) {
      request.fillerChanges
          = mergeFillerChanges(fixed_, request.fillerChanges);
    }
    std::vector<OracleResult> results = base_.checkPlaceWithOverlays(merged);
    for (OracleResult& result : results) {
      for (Violation& violation : result.violations) {
        for (ViolationParticipant& participant : violation.participants) {
          const PlacedInstance* placed
              = view_.instance(participant.instanceId);
          if (placed == nullptr) {
            continue;
          }
          const MasterInfo* master = view_.masterInfo(placed->masterId);
          participant.masterId = placed->masterId;
          participant.rowId = placed->rowId;
          participant.xRange
              = XInterval{placed->x,
                          placed->x + (master != nullptr ? master->width : 0)};
          participant.isFiller = placed->isFiller;
        }
      }
    }
    return results;
  }

  const ipl::FillerChanges& fixedChanges() const { return fixed_; }

 private:
  RepairOracle& base_;
  const LayoutPlacementView& view_;
  ipl::FillerChanges fixed_;
};

}  // namespace

class FillerRepairEngine::Impl final : private PlacementView,
                                       private RepairOracle
{
 public:
  explicit Impl(const ipl::ImplantLayerChecker& checker)
      : checker_(checker),
        grid_(checker.getGrid()),
        network_(checker.getNetwork()),
        log_(debugLoggingDefault())
  {
    log_.section("engine", "ENGINE INITIALIZATION");
    buildInitialSnapshot();
    if (!initialized_) {
      log_.block("engine",
                 "Initialization failed",
                 {{"diagnostics", cat(init_diagnostics_.size())}});
    }
  }

  bool ready() const { return initialized_; }
  const std::vector<ipl::Diagnostic>& initDiagnostics() const
  {
    return init_diagnostics_;
  }
  // Gap/overlap coverage restricted to selected repair rows. repair() checks
  // the initial target influence before oracle evaluation; adaptive candidates
  // that edit farther rows are checked before entering the checker batch.
  // This keeps the safety gate proportional to touched rows instead of the
  // whole placed design.
  ipl::CheckResult localPrecheck(const Region& influence) const;
  RepairOutcome repair(const ipl::CheckRequest& request);

 private:
  static constexpr int kSnapshotHaloRows = 1;

  void buildPlannerData();
  bool snapshotIsValid() const;
  bool isNonBlockingCheckerInitDiagnostic(
      const ipl::Diagnostic& diagnostic) const;
  void buildInitialSnapshot();
  bool ensureMasterRegistered(const eLIB::PhysLibCell& master);
  bool rebuildOracle();
  void buildLegalSpans();
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
  // container counts only if the Network id spaces are not dense).
  template <typename T>
  static void ensureSlot(std::vector<T>& table, size_t id)
  {
    if (id >= table.size()) {
      table.resize(id + 1);
    }
  }

  const ipl::ImplantLayerChecker& checker_;
  Grid* grid_ = nullptr;
  Network* network_ = nullptr;
  eUNL::PhysDesMgr* des_mgr_ = nullptr;
  const fillerSetting* filler_settings_ = nullptr;
  std::vector<const eLIB::PhysLibCell*> filler_masters_;
  PlacementSnapshot placement_;
  FillerCandidateCatalog candidate_catalog_;
  RepairConfig repair_config_;
  DebugLog log_;
  std::vector<Diagnostic> setup_diagnostics_;
  std::vector<ipl::Diagnostic> init_diagnostics_;
  std::vector<ipl::Diagnostic> oracle_diagnostics_;
  const std::vector<XInterval>& legalSpansForRow(RowId rowId) const;
  bool initialized_ = false;
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
  const ipl::ImplantLayerChecker* checker = &checker_;
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
  // wrong place. HandOff.md records the required envelope: no pad row before
  // a standard row, y-sorted rows, and one shared row origin X at the core
  // edge.
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
    const eLIB::TechSite* site = cell->getTechSite();
    placement_.masters[id] = PlacementSnapshot::MasterRef{
        info, cell->getLibCellId(), site != nullptr ? site->getName() : ""};
  }

  // --- candidate universe: fillerSetting only, resolved to Network master
  // ids. Entries the Network does not know cannot be validated by the
  // checker either (it builds masters from the Network) -> Warning + skip.
  {
    log_.section("candidate", "CONFIGURED FILLER MASTERS");
    log_.block("candidate",
               "Candidate provider source",
               {{"source", "fillerSetting"},
                {"configured count", cat(fillerMasters.size())},
                {"network masters", cat(network->getMasters().size())}});
    std::vector<std::vector<std::string>> configuredRows;
    configuredRows.reserve(fillerMasters.size());
    for (size_t configuredIndex = 0;
         configuredIndex < fillerMasters.size();
         ++configuredIndex) {
      const eLIB::PhysLibCell* cell = fillerMasters[configuredIndex];
      if (cell == nullptr) {
        configuredRows.push_back({cat(configuredIndex),
                                  "physLibCell=null",
                                  "-",
                                  "skip",
                                  "-"});
        continue;
      }
      const int id = network->getMasterId(cell->getLibCellId());
      const std::string description = masterDebug(*cell);
      if (id < 0) {
        configuredRows.push_back({cat(configuredIndex),
                                  description,
                                  cat(id),
                                  "reject",
                                  "not in Network"});
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
        configuredRows.push_back({cat(configuredIndex),
                                  description,
                                  cat(id),
                                  "reject",
                                  "planner metadata missing"});
        addProblem(Severity::Fatal, "ConfiguredMasterMissingMetadata",
                   cat("configured filler master has no planner metadata: "
                       "configuredIndex=",
                       configuredIndex, " networkMasterId=", id,
                       " {", masterDebug(*cell), "}"));
        continue;
      }
      configuredRows.push_back(
          {cat(configuredIndex),
           description,
           cat(id),
           "accept",
           cat("filler=", info->isFiller, " vt=", info->vt,
               " width=", info->width, " heightRows=", info->height,
               " bottom=", polarityName(info->bottomBandPolarity))});
      placement_.fillerMasterIds.push_back(static_cast<MasterId>(id));
    }
    log_.table("candidate",
               "Configured master decisions",
               {"index", "physical master", "network ID", "decision", "metadata / reason"},
               configuredRows);
    std::sort(placement_.fillerMasterIds.begin(), placement_.fillerMasterIds.end());
    placement_.fillerMasterIds.erase(
        std::unique(placement_.fillerMasterIds.begin(), placement_.fillerMasterIds.end()),
        placement_.fillerMasterIds.end());
    std::string ids;
    for (const MasterId id : placement_.fillerMasterIds) {
      if (!ids.empty()) {
        ids += ',';
      }
      ids += std::to_string(id);
    }
    log_.block("candidate",
               "Candidate provider ready",
               {{"accepted count", cat(placement_.fillerMasterIds.size())},
                {"master IDs", cat('[', ids, ']')}});
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
    log_.block(
        "engine",
        "Default halo source",
        {{"kind",
          fillerWidthWins ? "FILLER_MASTER_WIDTH" : "CHECKER_RULE_REACH"},
         {"checker reach (sites)", cat(reachSites)},
         {"checker reach (DBU)", cat(checkerReach)},
         {"widest filler master", cat(maxFillerMaster)},
         {"widest filler (DBU)", cat(maxFillerWidth)},
         {"default halo X", cat(placement_.defaultHaloX)}});
  }

  const auto placedCount = std::count_if(
      placement_.instances.begin(), placement_.instances.end(),
      [](const auto& slot) { return slot.has_value(); });
  const auto masterCount = std::count_if(
      placement_.masters.begin(), placement_.masters.end(),
      [](const auto& slot) { return slot.has_value(); });
  log_.block("engine",
             "Placement view",
             {{"nodes", cat(placedCount)},
              {"masters", cat(masterCount)},
              {"configured filler masters",
               cat(placement_.fillerMasterIds.size())},
              {"site width", cat(placement_.siteWidth)},
              {"default halo X", cat(placement_.defaultHaloX)}});
}

bool FillerRepairEngine::Impl::snapshotIsValid() const
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
  if (des_mgr_ == nullptr || network_ == nullptr) {
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

  for (const ipl::Layer& layer : checker_.getLayers()) {
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
  log_.block("engine",
             "Setup diagnostic",
             {{"code", code}, {"message", message}});
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

  // The initial target influence was checked before planner search.
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
      = checker_.checkPlaceWithOverlays(target, guard, changes);
  log_.block("engine",
             "Overlay batch",
             {{"candidates", cat(changes.size())},
              {"results", cat(raw.size())}});

  // Ordered correlation is the final checker's entire batch protocol. Any
  // missing OR extra result invalidates the whole batch. Preserve only the
  // returned cardinality so OracleGate can diagnose the exact mismatch; no
  // checker finding from a mis-correlated batch is consumed.
  if (raw.size() != legalIndices.size()) {
    log_.block("engine",
               "Overlay batch protocol error",
               {{"expected results", cat(legalIndices.size())},
                {"received results", cat(raw.size())}});
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
  const auto& initDiags = checker_.getDiags();
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
  RepairOutcome result;
  log_.section("engine", "REPAIR REQUEST");
  const auto addDiagnostic = [&result](Severity severity,
                                       const std::string& code,
                                       const std::string& message) {
    result.diagnostics.push_back(
        toPublicDiagnostic(makeDiag(severity, code, message)));
  };

  if (!initialized_) {
    result.diagnostics = init_diagnostics_;
    addDiagnostic(Severity::Fatal,
                  "engine_not_initialized",
                  "engine construction did not produce a ready snapshot");
    return result;
  }

  if (!snapshotIsValid()) {
    for (const Diagnostic& diagnostic : setup_diagnostics_) {
      result.diagnostics.push_back(toPublicDiagnostic(diagnostic));
    }
    addDiagnostic(Severity::Fatal,
                  "EngineNotReady",
                  "infrastructure snapshot failed validation; repair refused");
    return result;
  }

  // Resolve the checker request against the immutable engine snapshot. Do not
  // reread candidate placement from the live Node: DePlace may already have
  // changed that Node while the UDM commit is still pending.
  const int targetId = request.instanceId;
  const PlacedInstance* inst =
      targetId >= 0 ? instance(static_cast<InstanceId>(targetId)) : nullptr;
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
  const eUNL::LeafCellID targetCell
      = placement_.instances[inst->id]->udm.cellId;
  const Master* requestedMaster
      = network_->getMaster(static_cast<int>(request.masterId));
  const eLIB::PhysLibCell* newMaster
      = requestedMaster != nullptr ? requestedMaster->getPhysLibCell()
                                   : nullptr;
  if (newMaster == nullptr) {
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
  if (placement_.siteWidth <= 0 || replacementWidth <= 0
      || replacementWidth % placement_.siteWidth != 0
      || replacementHeight < 1 || replacementHeight > 2
      || oldMaster->width <= 0
      || oldMaster->width % placement_.siteWidth != 0
      || oldMaster->height < 1 || oldMaster->height > 2) {
    addDiagnostic(Severity::Fatal,
                  "UnsupportedTargetFootprint",
                  "target masters must be site-aligned rectangles one or "
                  "two rows high");
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
  const DbCoord requestedX
      = static_cast<DbCoord>(request.colId) * placement_.siteWidth;
  const Orient requestedOrientation = toPlannerOrient(request.orientation);
  Region initialInfluence = snapshotGuard(
      requestedRowId, requestedX, replacementWidth, replacementHeight);
  const Region oldInfluence = snapshotGuard(
      targetRowId, targetX, oldMaster->width, oldMaster->height);
  initialInfluence.x.xl
      = std::min(initialInfluence.x.xl, oldInfluence.x.xl);
  initialInfluence.x.xh
      = std::max(initialInfluence.x.xh, oldInfluence.x.xh);
  initialInfluence.rowLo
      = std::min(initialInfluence.rowLo, oldInfluence.rowLo);
  initialInfluence.rowHi
      = std::max(initialInfluence.rowHi, oldInfluence.rowHi);

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

  // Master construction belongs to infrastructure because it requires the
  // real edge table. Repair may refresh an existing snapshot, but it never
  // invents an incomplete Network master.
  if (network_->getMaster(newMaster->getLibCellId()) == nullptr) {
    addDiagnostic(Severity::Fatal,
                  "TargetMasterNotRegistered",
                  "target master is absent from Network; infrastructure "
                  "must register it with the real edge table before check");
    return result;
  }

  const int newMasterId = network_->getMasterId(newMaster->getLibCellId());
  // Snapshot changes are an owner-level revision boundary. Never rebuild
  // shared engine state inside a repair call: concurrent repairs must all see
  // one immutable design revision.
  if (newMasterId >= 0
      && masterInfo(static_cast<MasterId>(newMasterId)) == nullptr) {
    addDiagnostic(Severity::Fatal,
                  "StaleEngineSnapshot",
                  "target master was registered after engine initialization; "
                  "the owner must rebuild the engine before parallel checks");
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
    std::vector<std::vector<std::string>> matchingRows;
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
          matchingRows.push_back(
              {cat(physicalRowId),
               row.getSite().getName(),
               cat(row.getSite().getIsPad()),
               cat('[', rowYl, ',', rowYh, ')'),
               cat(row.getSite().getHeight().getStorage()),
               orientationName(row.getOrient()),
               cat(containsPhysical),
               cat(containsRequest)});
        }
        ++physicalRowId;
      }
    }
    const MasterInfo* requestedMasterInfo = masterInfo(target.masterId);
    log_.section("engine", "TARGET SNAPSHOT FRAME");
    log_.block(
        "engine",
        "Request",
        {{"source", "checker"},
         {"instance", cat(target.instanceId)},
         {"master", cat(target.masterId)},
         {"row", cat(requestedRowId)},
         {"column",
          cat(placement_.siteWidth > 0
                  ? requestedX / placement_.siteWidth
                  : -1)},
         {"x (DBU)", cat(requestedX)},
         {"y relative (DBU)", cat(requestYRelative)},
         {"y absolute (DBU)", cat(requestYAbsolute)},
         {"orientation",
          orientationName(toUdmOrient(requestedOrientation))}});
    log_.block("engine",
               "Engine snapshot",
               {{"master", cat(oldMasterId)},
                {"row", cat(targetRowId)},
                {"x (DBU)", cat(targetX)},
                {"orientation",
                 orientationName(toUdmOrient(targetOrientation))},
                {"same placement", cat(samePlacement)}});
    log_.block("engine",
               "Network node",
               {{"master", cat(targetNode->getMaster()->getId())},
                {"left", cat(targetNode->getLeft().v)},
                {"bottom", cat(targetNode->getBottom().v)},
                {"grid row", cat(grid_->gridSnapDownY(targetNode).v)},
                {"grid column", cat(grid_->gridX(targetNode).v)},
                {"orientation", orientationName(targetNode->getOrient())}});
    log_.block(
        "engine",
        "Physical cell",
        {{"valid", cat(physical.isValid())},
         {"master lib",
          physical.isValid()
              ? cat(physical.getPhysMaster()
                        .getLibCellId()
                        .getIndexValue())
              : "-"},
         {"origin",
          physical.isValid()
              ? cat('(', physical.getOrigin().getX().getStorage(), ',',
                    physical.getOrigin().getY().getStorage(), ')')
              : "-"},
         {"orientation",
          physical.isValid() ? orientationName(physical.getOrient()) : "-"}});
    log_.block(
        "engine",
        "Master bands",
        {{"old bottom", polarityName(oldBottomBandPolarity)},
         {"requested bottom",
          requestedMasterInfo != nullptr
              ? polarityName(requestedMasterInfo->bottomBandPolarity)
              : "missing"},
         {"requested height (rows)",
          cat(requestedMasterInfo != nullptr ? requestedMasterInfo->height
                                             : -1)}});
    log_.table("engine",
               "Matching physical rows",
               {"iteration ID",
                "site",
                "pad",
                "y range",
                "height",
                "orientation",
                "has physical",
                "has request"},
               matchingRows);
  }

  const bool layoutChanged
      = target.rowId != targetRowId || target.x != targetX
        || replacement->width != oldMaster->width
        || replacement->height != oldMaster->height;
  if (layoutChanged) {
    // The layout rewrite is deliberately local and immutable: identify every
    // filler displaced by the proposed target, exact-cover the released site
    // cells, then submit Delete/Add plus any VT swaps as one checker overlay.
    const int newCol = static_cast<int>(target.x / placement_.siteWidth);
    const int newWidthSites
        = static_cast<int>(replacement->width / placement_.siteWidth);
    const int newHeightRows = static_cast<int>(replacement->height);
    const int oldCol = static_cast<int>(targetX / placement_.siteWidth);
    const int oldWidthSites
        = static_cast<int>(oldMaster->width / placement_.siteWidth);
    const int oldHeightRows = static_cast<int>(oldMaster->height);
    const int rowCount = static_cast<int>(placement_.rows.size());
    const int colCount = grid_->getRowSiteCount().v;
    if (target.x % placement_.siteWidth != 0 || newCol < 0
        || newCol + newWidthSites > colCount || target.rowId < 0
        || target.rowId + newHeightRows > rowCount) {
      addDiagnostic(Severity::Fatal,
                    "TargetFootprintOutOfGrid",
                    "proposed target footprint is not a legal site rectangle");
      return result;
    }

    std::set<InstanceId> removed;
    for (int rowOffset = 0; rowOffset < newHeightRows; ++rowOffset) {
      const RowId rowId = target.rowId + rowOffset;
      for (int colOffset = 0; colOffset < newWidthSites; ++colOffset) {
        const int colId = newCol + colOffset;
        const Pixel* pixel = grid_->gridPixel(GridX{colId}, GridY{rowId});
        if (pixel == nullptr || !pixel->is_valid
            || pixel->padding_reserved_by != nullptr) {
          addDiagnostic(Severity::Warning,
                        "TargetFootprintOnIllegalSite",
                        cat("target covers illegal/reserved site row=", rowId,
                            " col=", colId));
          return result;
        }
        const Node* occupant = pixel->cell;
        if (occupant == nullptr || occupant->getId() == target.instanceId) {
          continue;
        }
        const PlacedInstance* displaced = instance(occupant->getId());
        if (displaced == nullptr || !displaced->isFiller) {
          addDiagnostic(Severity::Warning,
                        "TargetOverlapsNonFiller",
                        cat("target overlaps non-filler instance ",
                            occupant->getId(), " at row=", rowId,
                            " col=", colId));
          return result;
        }
        removed.insert(displaced->id);
      }
    }

    std::set<internal::SiteCell> emptySites;
    const auto addFootprint = [&emptySites](RowId rowId,
                                            int colId,
                                            int widthSites,
                                            int heightRows) {
      for (int rowOffset = 0; rowOffset < heightRows; ++rowOffset) {
        for (int colOffset = 0; colOffset < widthSites; ++colOffset) {
          emptySites.insert(
              internal::SiteCell{rowId + rowOffset, colId + colOffset});
        }
      }
    };
    addFootprint(targetRowId, oldCol, oldWidthSites, oldHeightRows);
    Region rewriteInfluence = initialInfluence;
    for (const InstanceId id : removed) {
      const PlacedInstance* filler = instance(id);
      const MasterInfo* master
          = filler != nullptr ? masterInfo(filler->masterId) : nullptr;
      if (filler == nullptr || master == nullptr || !master->isFiller
          || master->width <= 0
          || master->width % placement_.siteWidth != 0
          || master->height < 1 || master->height > 2) {
        addDiagnostic(Severity::Fatal,
                      "UnsupportedDisplacedFiller",
                      cat("displaced filler ", id,
                          " has an unsupported footprint"));
        return result;
      }
      const int fillerCol
          = static_cast<int>(filler->x / placement_.siteWidth);
      const int fillerWidth
          = static_cast<int>(master->width / placement_.siteWidth);
      addFootprint(filler->rowId,
                   fillerCol,
                   fillerWidth,
                   static_cast<int>(master->height));
      rewriteInfluence.x.xl
          = std::min(rewriteInfluence.x.xl, filler->x);
      rewriteInfluence.x.xh
          = std::max(rewriteInfluence.x.xh, filler->x + master->width);
      rewriteInfluence.rowLo
          = std::min(rewriteInfluence.rowLo, filler->rowId);
      rewriteInfluence.rowHi = std::max(
          rewriteInfluence.rowHi,
          filler->rowId + static_cast<RowId>(master->height) - 1);
    }
    for (int rowOffset = 0; rowOffset < newHeightRows; ++rowOffset) {
      for (int colOffset = 0; colOffset < newWidthSites; ++colOffset) {
        emptySites.erase(
            internal::SiteCell{target.rowId + rowOffset, newCol + colOffset});
      }
    }

    // Every released cell must be legal and occupied only by the old target
    // or a filler named in the Delete prefix. Otherwise exact-cover geometry
    // would conceal an input placement error.
    for (const internal::SiteCell& site : emptySites) {
      if (site.rowId < 0 || site.rowId >= rowCount || site.colId < 0
          || site.colId >= colCount) {
        addDiagnostic(Severity::Fatal,
                      "ReleasedSiteOutOfGrid",
                      "released target/filler footprint leaves the core grid");
        return result;
      }
      const Pixel* pixel
          = grid_->gridPixel(GridX{site.colId}, GridY{site.rowId});
      if (pixel == nullptr || !pixel->is_valid
          || pixel->padding_reserved_by != nullptr) {
        addDiagnostic(Severity::Fatal,
                      "ReleasedSiteNotFillable",
                      cat("released site is illegal/reserved row=", site.rowId,
                          " col=", site.colId));
        return result;
      }
      const Node* occupant = pixel->cell;
      if (occupant != nullptr && occupant->getId() != target.instanceId
          && removed.count(occupant->getId()) == 0) {
        addDiagnostic(Severity::Fatal,
                      "ReleasedSiteStillOccupied",
                      cat("released site overlaps unchanged instance ",
                          occupant->getId()));
        return result;
      }
    }

    std::vector<internal::FillerFootprint> footprints;
    std::unordered_map<MasterId, eLIB::LibCellID> masterLibCells;
    for (const MasterId masterId : placement_.fillerMasterIds) {
      const MasterInfo* master = masterInfo(masterId);
      if (master == nullptr || !master->isFiller || master->width <= 0
          || master->width % placement_.siteWidth != 0
          || master->height < 1 || master->height > 2) {
        continue;
      }
      footprints.push_back(
          internal::FillerFootprint{masterId,
                                    static_cast<int>(master->width
                                                     / placement_.siteWidth),
                                    static_cast<int>(master->height)});
      masterLibCells.emplace(masterId,
                             placement_.masters[masterId]->libCellId);
    }
    if (footprints.empty()) {
      addDiagnostic(Severity::Warning,
                    "NoRetilingFillerMaster",
                    "no configured one/two-row site-aligned filler master");
      return result;
    }

    const internal::RetileResult tilings = internal::enumerateRetilings(
        std::vector<internal::SiteCell>(emptySites.begin(), emptySites.end()),
        footprints);
    log_.section("engine", "LAYOUT REWRITE");
    log_.block("engine",
               "Retiling search",
               {{"removed fillers", cat(removed.size())},
                {"empty sites", cat(emptySites.size())},
                {"tilings", cat(tilings.solutions.size())},
                {"search states", cat(tilings.searchStates)},
                {"truncated", cat(tilings.truncated)}});
    if (tilings.solutions.empty()) {
      addDiagnostic(Severity::Warning,
                    tilings.truncated ? "RetilingBudgetExceeded"
                                      : "ReleasedAreaNotTileable",
                    "configured filler footprints cannot exactly cover the "
                    "released sites");
      return result;
    }

    ipl::FillerChanges deletionPrefix;
    for (const InstanceId id : removed) {
      const PlacedInstance* filler = instance(id);
      CellChangeRecord deletion
          = cellChangeRecord(id, filler != nullptr ? filler->masterId : -1);
      deletion.op_ = OpType::Delete;
      deletion.new_lib_cell_ = deletion.orig_lib_cell_;
      deletionPrefix.push_back(std::move(deletion));
    }

    std::vector<ipl::Diagnostic> rejectedLayoutDiagnostics;
    for (const std::vector<internal::TiledFiller>& tiling :
         tilings.solutions) {
      std::vector<LayoutAddition> additions;
      ipl::FillerChanges fixed = deletionPrefix;
      bool usable = true;
      int addIndex = 0;
      for (const internal::TiledFiller& tile : tiling) {
        const MasterInfo* footprintMaster = masterInfo(tile.masterId);
        if (footprintMaster == nullptr) {
          usable = false;
          break;
        }

        MasterId chosen = -1;
        eUTL::PhysOrientation orientation(eUTL::PhysOrientationE::R0);
        for (const MasterId candidate : placement_.fillerMasterIds) {
          const MasterInfo* master = masterInfo(candidate);
          if (master == nullptr
              || master->width != footprintMaster->width
              || master->height != footprintMaster->height) {
            continue;
          }
          const std::string& siteName
              = placement_.masters[candidate]->siteName;
          if (siteName.empty()) {
            continue;
          }
          const std::optional<eUTL::PhysOrientation> expected
              = grid_->getSiteOrientation(GridX{tile.colId},
                                          GridY{tile.rowId},
                                          siteName);
          if (!expected.has_value()
              || !supportedOrientation(*expected)) {
            continue;
          }
          if (chosen < 0
              || (master->vt == replacement->vt
                  && masterInfo(chosen)->vt != replacement->vt)) {
            chosen = candidate;
            orientation = *expected;
          }
        }
        if (chosen < 0) {
          usable = false;
          break;
        }

        // Extend dpl's coordinate name with physical dimensions and sequence.
        const std::string name = cat(filler_settings_->getPrefix(),
                                     "_FR_",
                                     tile.rowId,
                                     '_',
                                     tile.colId,
                                     "_W",
                                     footprintMaster->width,
                                     "_H",
                                     footprintMaster->height
                                         * placement_.rowHeight,
                                     '_',
                                     addIndex);
        const DbCoord xAbsolute
            = placement_.coreXl
              + static_cast<DbCoord>(tile.colId) * placement_.siteWidth;
        const DbCoord yAbsolute
            = placement_.rowFrames[static_cast<size_t>(tile.rowId)].yLo;
        CellChangeRecord addition{OpType::Add,
                                  CellData{name},
                                  eUTL::UvDist(xAbsolute),
                                  eUTL::UvDist(yAbsolute),
                                  eLIB::LibCellID(0, 0),
                                  placement_.masters[chosen]->libCellId,
                                  orientation};
        const InstanceId syntheticId = -1 - addIndex++;
        LayoutAddition local;
        local.instanceId = syntheticId;
        local.placed = PlacedInstance{
            syntheticId,
            chosen,
            tile.rowId,
            static_cast<DbCoord>(tile.colId) * placement_.siteWidth,
            toPlannerOrient(orientation),
            true};
        local.record = addition;
        additions.push_back(std::move(local));
        fixed.push_back(std::move(addition));
      }
      if (!usable) {
        continue;
      }

      LayoutPlacementView layoutView(
          *this, target, removed, additions, masterLibCells);
      LayoutOracle layoutOracle(*this, layoutView, fixed);
      OracleRequest snapshotRequest;
      snapshotRequest.requestId = 0;
      snapshotRequest.targetPlace = target;
      snapshotRequest.guardRegion = rewriteInfluence;
      const OracleResult snapshot
          = layoutOracle.checkPlaceWithOverlay(snapshotRequest);
      if (snapshot.status != OracleStatus::Checked) {
        for (const Diagnostic& diagnostic : snapshot.diagnostics) {
          rejectedLayoutDiagnostics.push_back(
              toPublicDiagnostic(diagnostic));
        }
        continue;
      }
      if (snapshot.violations.empty()) {
        result.hasSolution = true;
        result.changes = fixed;
        addDiagnostic(Severity::Info,
                      "LayoutRetiled",
                      cat("removed ", removed.size(), " filler(s), added ",
                          additions.size(), " filler(s)"));
        return result;
      }

      FillerRepairRequest plannerRequest;
      plannerRequest.targetPlace = target;
      plannerRequest.violations = snapshot.violations;
      internal::RepairPlanner planner(
          layoutView, layoutOracle, repair_config_);
      const FillerRepairResult planned = planner.repair(plannerRequest);
      if (!planned.hasSolution) {
        continue;
      }
      result.hasSolution = true;
      result.changes
          = mergeFillerChanges(fixed, planned.changes);
      for (const Diagnostic& diagnostic : planned.diagnostics) {
        result.diagnostics.push_back(toPublicDiagnostic(diagnostic));
      }
      return result;
    }

    result.diagnostics.insert(result.diagnostics.end(),
                              rejectedLayoutDiagnostics.begin(),
                              rejectedLayoutDiagnostics.end());
    addDiagnostic(Severity::Warning,
                  "NoLegalRetiling",
                  "no exact-cover filler layout passed the implant checker");
    return result;
  }

  // 2) Initial snapshot: the new target place with ZERO filler changes.
  // Snapshot and every later engine baseline/candidate go through this same
  // object -> one consistent oracle worldview.
  OracleRequest snapshotRequest;
  snapshotRequest.requestId = 0;
  snapshotRequest.targetPlace = target;
  snapshotRequest.guardRegion = initialInfluence;
  log_.section("engine", "INITIAL OVERLAY CHECK");
  log_.block("engine",
             "Target snapshot",
             {{"instance", cat(target.instanceId)},
              {"new master", cat(target.masterId)},
              {"guard", show(snapshotRequest.guardRegion)}});

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
  // 3) Pure search over this object's data-source and oracle interfaces.
  FillerRepairRequest plannerRequest;
  plannerRequest.targetPlace = target;
  plannerRequest.violations = snapshot.violations;
  internal::RepairPlanner planner(*this, *this, repair_config_);
  const FillerRepairResult planned = planner.repair(plannerRequest);

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
  // whole-design placement legality is infrastructure's own gate). Legal
  // spans were frozen from Grid pixels at initialization; placed spans are
  // read live from PhysDesMgr for the row's snapshot nodes.
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
  static const std::vector<XInterval> empty;
  if (rowId < 0
      || static_cast<size_t>(rowId) >= placement_.legalSpans.size()) {
    return empty;
  }
  return placement_.legalSpans[static_cast<size_t>(rowId)];
}

void FillerRepairEngine::Impl::buildLegalSpans()
{
  placement_.legalSpans.assign(placement_.rowFrames.size(), {});
  for (RowId rowId = 0;
       static_cast<size_t>(rowId) < placement_.rowFrames.size();
       ++rowId) {
    // A valid pixel not reserved by halo/padding requires exactly one placed
    // cover; maximal runs of such pixels form the legal spans (core-left-
    // relative like every planner x).
    std::vector<XInterval>& spans
        = placement_.legalSpans[static_cast<size_t>(rowId)];
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
        spans.push_back(
            {spanStart,
             static_cast<DbCoord>(x.v) * placement_.siteWidth});
      }
    }
    if (inSpan) {
      spans.push_back({spanStart,
                       static_cast<DbCoord>(grid_->getRowSiteCount().v)
                           * placement_.siteWidth});
    }
  }
}

void FillerRepairEngine::Impl::failInit(const std::string& status,
                                        const std::string& message)
{
  init_diagnostics_.push_back({status, message});
  log_.block("engine",
             "Initialization diagnostic",
             {{"status", status}, {"message", message}});
}

void FillerRepairEngine::Impl::buildInitialSnapshot()
{
  eUNL::Design* const design = checker_.getDesign();
  if (grid_ == nullptr || design == nullptr || network_ == nullptr) {
    failInit("missing_infrastructure",
             cat("fatal: missing initialized Grid, Design, or Network: grid=",
                 grid_ != nullptr, " design=", design != nullptr,
                 " network=", network_ != nullptr));
    return;
  }
  eUNL::PhysDesMgr* const desMgr = design->getPhysDesMgr();
  if (desMgr == nullptr) {
    failInit("missing_phys_des_mgr",
             cat("fatal: checker Design has no PhysDesMgr: design=",
                 static_cast<const void*>(design),
                 " designPhysDesMgr=", static_cast<const void*>(desMgr),
                 " gridSiteWidth=",
                 grid_->getSiteWidth().v,
                 " gridRows=", grid_->getRowCount().v,
                 " networkNodes=", network_->getNodes().size(),
                 " networkMasters=", network_->getMasters().size()));
    return;
  }
  const fillerSetting* const fillerSettings = network_->getFillerSetting();
  if (fillerSettings == nullptr) {
    failInit("missing_filler_setting",
             "fatal: Network has no active fillerSetting; DePlace must bind "
             "it before filler repair initialization");
    return;
  }
  if (fillerSettings->getFillerPhysCells().empty()) {
    failInit("empty_filler_allow_list",
             cat("fatal: fillerSetting::getFillerPhysCells() is empty: "
                 "settingDesign=",
                 static_cast<const void*>(fillerSettings->getDesign()),
                 " networkNodes=", network_->getNodes().size(),
                 " networkMasters=", network_->getMasters().size()));
    return;
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
                 fillerSettings->getFillerPhysCells().size()));
    return;
  }
  // Per-node Network<->UDM cross-validation was removed deliberately:
  // infrastructure data is trusted as-is. The owner finishes master
  // registration and setting binding before it initializes this immutable
  // snapshot; each later target change arrives as a CheckRequest overlay.
  // Nodes whose master or physical record is unusable are simply skipped by
  // buildPlannerData.

  des_mgr_ = desMgr;
  filler_settings_ = fillerSettings;
  filler_masters_ = fillerSettings->getFillerPhysCells();
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
               cat("fatal: configured filler master is absent from Network "
                   "or could not be classified: configuredIndex=",
                   configuredIndex, " {", masterDebug(*master),
                   "} networkMasters=", network_->getMasters().size(),
                   " gridSiteWidth=", grid_->getSiteWidth().v,
                   " gridRows=", grid_->getRowCount().v));
    }
  }
  if (init_diagnostics_.empty()) {
    initialized_ = rebuildOracle();
    if (!initialized_) {
      init_diagnostics_.insert(init_diagnostics_.end(),
                               oracle_diagnostics_.begin(),
                               oracle_diagnostics_.end());
      for (const ipl::Diagnostic& diagnostic : oracle_diagnostics_) {
        log_.block("engine",
                   "Initialization diagnostic",
                   {{"status", diagnostic.status},
                    {"message", diagnostic.message}});
      }
    }
  }
}

bool FillerRepairEngine::Impl::ensureMasterRegistered(
    const eLIB::PhysLibCell& master)
{
  if (network_ == nullptr || filler_settings_ == nullptr) {
    return false;
  }
  // Infrastructure owns Master creation because it has the real edge table.
  // Repair only refreshes the allow-list classification on an existing one.
  Master* const registered = network_->getMaster(master.getLibCellId());
  if (registered == nullptr) {
    return false;
  }
  registered->setFiller(true);
  return true;
}

bool FillerRepairEngine::Impl::rebuildOracle()
{
  oracle_diagnostics_.clear();
  repair_config_.verbose = log_.enabled();
  buildPlannerData();
  buildLegalSpans();
  bool checkerReady = true;
  for (const ipl::Diagnostic& diagnostic : checker_.getDiags()) {
    if (isNonBlockingCheckerInitDiagnostic(diagnostic)) {
      log_.block("engine",
                 "Non-blocking checker initialization diagnostic",
                 {{"status", diagnostic.status},
                  {"message", diagnostic.message}});
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
             " checkerSiteWidth=", checker_.siteWidth(), "}")});
    log_.block("engine",
               "Blocking checker initialization diagnostic",
               {{"status", diagnostic.status},
                {"message", diagnostic.message}});
  }
  if (checkerReady && snapshotIsValid()) {
    return true;
  }
  for (const Diagnostic& diagnostic : setup_diagnostics_) {
    oracle_diagnostics_.push_back(toPublicDiagnostic(diagnostic));
  }
  return false;
}

FillerRepairEngine::FillerRepairEngine(
    const ipl::ImplantLayerChecker& checker)
    : impl_(std::make_unique<Impl>(checker))
{
}

FillerRepairEngine::~FillerRepairEngine() = default;

bool FillerRepairEngine::isReady() const
{
  return impl_->ready();
}

const std::vector<ipl::Diagnostic>&
FillerRepairEngine::getInitDiagnostics() const
{
  return impl_->initDiagnostics();
}

RepairOutcome FillerRepairEngine::repair(const ipl::CheckRequest& request)
{
  return impl_->repair(request);
}


}  // namespace fillerRepair
}  // namespace dpl2
