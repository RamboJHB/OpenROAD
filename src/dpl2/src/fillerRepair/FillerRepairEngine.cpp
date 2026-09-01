// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerRepair/FillerRepairEngine.h>
#include <fillerRepair/FillerRetiler.h>
#include <fillerRepair/RepairPlanner.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dpl2 {
namespace fillerRepair {

namespace {

// Immutable planner-facing placement snapshot built once with the engine.
struct PlacementSnapshot
{
  struct MasterRef
  {
    MasterInfo info;
    eLIB::LibCellID libCellId;
  };
  struct RecordRef
  {
    eUNL::LeafCellID cellId;
    eLIB::LibCellID libCellId;
    // Destination records use the same core-relative DBU frame as Node.
    eUTL::UvDist x;
    eUTL::UvDist y;
    eUTL::PhysOrientation orientation;
  };
  struct InstanceRef
  {
    PlacedInstance placed;
    RecordRef record;
  };

  DbCoord siteWidth = 0;
  DbCoord rowHeight = 0;
  DbCoord defaultHaloX = 0;
  std::vector<RowId> rows;
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
        const MasterInfo& candidate = placement.masters[candidateId]->info;
        if (candidate.id == source.id) {
          continue;
        }
        ++stats.checked;
        ++aggregate_.checked;
        const bool notFiller = !candidate.isFiller;
        const bool unknownVt
            = source.vt == kUnknownVt || candidate.vt == kUnknownVt;
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
        if (notFiller || unknownVt || sameVt || widthMismatch || heightMismatch
            || polarityMismatch) {
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
    log.block(
        "candidate",
        "Candidate compatibility catalog",
        {{"configured masters", cat(placement.fillerMasterIds.size())},
         {"master-pair checks", cat(aggregate_.checked)},
         {"compatible pairs", cat(aggregate_.compatible)},
         {"placed candidate", cat(hasPlacedCandidate_)},
         {"reject: not filler", cat(aggregate_.notFiller)},
         {"reject: unknown VT", cat(aggregate_.unknownVt)},
         {"reject: same VT", cat(aggregate_.sameVt)},
         {"reject: width mismatch", cat(aggregate_.widthMismatch)},
         {"reject: height mismatch", cat(aggregate_.heightMismatch)},
         {"reject: polarity mismatch", cat(aggregate_.polarityMismatch)}});
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

// Per-request view that removes caller-deleted fillers from every hot row
// query without mutating the engine's shared immutable snapshot. The anchor
// remains addressable for the planner's target metadata check, but it is not
// an editable row member.
class OverlayPlacementView final : public PlacementView
{
 public:
  OverlayPlacementView(const PlacementView& base,
                       const std::set<InstanceId>& excluded,
                       InstanceId anchor)
      : base_(base), excluded_(excluded), anchor_(anchor)
  {
  }

  const std::vector<RowId>& rows() const override { return base_.rows(); }
  DbCoord siteWidth() const override { return base_.siteWidth(); }
  const std::vector<PlacedInstance>& instancesInRow(
      RowId rowId) const override
  {
    const std::vector<PlacedInstance>& source = base_.instancesInRow(rowId);
    const auto cached = filteredRows_.find(rowId);
    if (cached != filteredRows_.end()) {
      return cached->second;
    }
    if (passthroughRows_.find(rowId) != passthroughRows_.end()) {
      return source;
    }
    if (std::none_of(source.begin(), source.end(), [this](const auto& inst) {
          return excluded_.find(inst.id) != excluded_.end();
        })) {
      passthroughRows_.insert(rowId);
      return source;
    }
    auto [found, inserted] = filteredRows_.try_emplace(rowId);
    (void) inserted;
    found->second.reserve(source.size());
    std::copy_if(source.begin(), source.end(),
                 std::back_inserter(found->second), [this](const auto& inst) {
                   return excluded_.find(inst.id) == excluded_.end();
                 });
    return found->second;
  }
  const PlacedInstance* instance(InstanceId id) const override
  {
    return id != anchor_ && excluded_.find(id) != excluded_.end()
               ? nullptr
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
  CellChangeRecord cellChangeRecord(InstanceId id,
                                    MasterId masterId) const override
  {
    return base_.cellChangeRecord(id, masterId);
  }
  MasterCandidateResult getUsableMasterCandidates(
      InstanceId id) const override
  {
    if (excluded_.find(id) != excluded_.end()) {
      MasterCandidateResult result;
      result.diagnostics.push_back(makeDiag(
          Severity::Warning,
          "CallerDeletedFiller",
          cat("caller-deleted filler ", id, " is not editable")));
      return result;
    }
    return base_.getUsableMasterCandidates(id);
  }

 private:
  const PlacementView& base_;
  const std::set<InstanceId>& excluded_;
  InstanceId anchor_ = -1;
  mutable std::unordered_map<RowId, std::vector<PlacedInstance>> filteredRows_;
  mutable std::unordered_set<RowId> passthroughRows_;
};

struct LayoutMasterOption
{
  MasterId masterId = -1;
  eUTL::PhysOrientation orientation{eUTL::PhysOrientationE::R0};
};

// A filler introduced only in this request. Negative ids keep it disjoint
// from the immutable Network snapshot until the caller commits the result.
struct LayoutAddition
{
  InstanceId instanceId = -1;
  PlacedInstance placed;
  CellChangeRecord record;
  std::vector<LayoutMasterOption> masterOptions;
};

CellChangeRecord invalidCellChangeRecord()
{
  return CellChangeRecord{
      OpType::Replace,
      CellData{eUNL::LeafCellID(0, 0)},
      eUTL::UvDist(int64_t{0}),
      eUTL::UvDist(int64_t{0}),
      eLIB::LibCellID(0, 0),
      eLIB::LibCellID(0, 0),
      eUTL::PhysOrientation(eUTL::PhysOrientationE::R0)};
}

bool advanceLayoutAssignment(
    std::vector<size_t>& indices,
    const std::vector<LayoutAddition>& additions)
{
  for (size_t remaining = additions.size(); remaining > 0; --remaining) {
    const size_t index = remaining - 1;
    ++indices[index];
    if (indices[index] < additions[index].masterOptions.size()) {
      return true;
    }
    indices[index] = 0;
  }
  return false;
}

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

// Placement seen by the existing swap planner after applying caller-owned
// deletions, the target, and one geometric retiling. It never mutates Grid or
// Network and is therefore safe to probe speculatively.
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
        master_lib_cells_(std::move(masterLibCells)),
        target_instance_{target_.instanceId,
                         target_.masterId,
                         target_.rowId,
                         target_.x,
                         target_.orientation,
                         false}
  {
    for (const RowId rowId : base_.rows()) {
      std::vector<PlacedInstance>& row = by_row_[rowId];
      for (const PlacedInstance& placed : base_.instancesInRow(rowId)) {
        if (removed_.count(placed.id) == 0) {
          row.push_back(placed);
        }
      }
    }
    appendToRows(target_instance_, masterInfo(target_.masterId));
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
    if (instanceId >= 0) {
      return base_.getUsableMasterCandidates(instanceId);
    }
    MasterCandidateResult result;
    const auto addition = std::find_if(
        additions_.begin(), additions_.end(), [instanceId](const auto& item) {
          return item.instanceId == instanceId;
        });
    if (addition == additions_.end()) {
      return result;
    }
    for (const LayoutMasterOption& option : addition->masterOptions) {
      if (option.masterId != addition->placed.masterId) {
        result.candidates.push_back(option.masterId);
      }
    }
    return result;
  }
  CellChangeRecord cellChangeRecord(InstanceId instanceId,
                                    MasterId newMasterId) const override
  {
    if (instanceId >= 0) {
      return base_.cellChangeRecord(instanceId, newMasterId);
    }
    const auto addition = std::find_if(
        additions_.begin(), additions_.end(), [instanceId](const auto& item) {
          return item.instanceId == instanceId;
        });
    if (addition == additions_.end()) {
      return invalidCellChangeRecord();
    }
    const auto option = std::find_if(
        addition->masterOptions.begin(),
        addition->masterOptions.end(),
        [newMasterId](const auto& item) {
          return item.masterId == newMasterId;
        });
    const auto libCell = master_lib_cells_.find(newMasterId);
    if (option == addition->masterOptions.end()
        || libCell == master_lib_cells_.end()) {
      return invalidCellChangeRecord();
    }
    CellChangeRecord record = addition->record;
    record.new_lib_cell_ = libCell->second;
    record.orientation_ = option->orientation;
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

// Prefix every swap candidate with the fixed Add records for its tiling, then
// repair negative synthetic participant metadata for the planner.
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
    failed.diagnostics.push_back(makeDiag(
        Severity::Fatal,
        "CheckerProtocolError",
        "checker returned the wrong result count for a layout overlay"));
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

 private:
  RepairOracle& base_;
  const LayoutPlacementView& view_;
  ipl::FillerChanges fixed_;
};

}  // namespace

class FillerRepairEngine::Impl final : private PlacementView
{
 public:
  explicit Impl(const ipl::ImplantLayerChecker& checker, bool debugLogging)
      : checker_(checker),
        grid_(checker.getGrid()),
        network_(checker.getNetwork()),
        checker_diagnostics_(checker.getDiags()),
        log_(debugLogging)
  {
    log_.section("engine", "ENGINE INITIALIZATION");
    buildPlannerData();
    for (const ipl::Diagnostic& diagnostic : checker_diagnostics_) {
      log_.block(
          "engine",
          "Checker initialization note",
          {{"status", diagnostic.status}, {"message", diagnostic.message}});
    }
    log_.block(
        "engine",
        "Initialization result",
        {{"ready", cat(setup_ok_)},
         {"nodes", cat(network_ != nullptr ? network_->getNodes().size() : 0)},
         {"masters",
          cat(network_ != nullptr ? network_->getMasters().size() : 0)}});
    if (!setup_ok_) {
      log_.block("engine",
                 "Initialization skipped",
                 {{"reason", "required Grid/Network geometry is unavailable"}});
    }
  }

  bool ready() const { return setup_ok_; }
  RepairOutcome repair(const ipl::CheckRequest& request) const;

 private:
  static constexpr int kSnapshotHaloRows = 1;

  void buildPlannerData();
  void logIssue(const std::string& code,
                const std::string& message,
                bool blocking = false);
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
  class BoundOracle final : public RepairOracle
  {
   public:
    BoundOracle(const Impl& impl, const ipl::CheckRequest& target)
        : impl_(impl), target_(target)
    {
    }

    OracleResult checkPlaceWithOverlay(const OracleRequest& request) override
    {
      return impl_.checkPlaceWithOverlay(target_, request);
    }

    std::vector<OracleResult> checkPlaceWithOverlays(
        const std::vector<OracleRequest>& requests) override
    {
      return impl_.checkPlaceWithOverlays(target_, requests);
    }

   private:
    const Impl& impl_;
    const ipl::CheckRequest& target_;
  };

  OracleResult checkPlaceWithOverlay(const ipl::CheckRequest& target,
                                     const OracleRequest& request) const;
  std::vector<OracleResult> checkPlaceWithOverlays(
      const ipl::CheckRequest& target,
      const std::vector<OracleRequest>& requests) const;

  ::Rect toGuardRect(const Region& region) const;
  Violation toPlannerViolation(const ipl::Violation& violation,
                               InstanceId targetInstance) const;
  Region snapshotGuard(const TargetPlace& target) const;
  void logRepairSuccess(const ipl::CheckRequest& request,
                        const RepairOutcome& result,
                        const std::string& status) const;
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
  std::vector<ipl::Diagnostic> checker_diagnostics_;
  PlacementSnapshot placement_;
  FillerCandidateCatalog candidate_catalog_;
  RepairConfig repair_config_;
  DebugLog log_;
  bool setup_ok_ = true;
};

namespace {

Orient toPlannerOrient(eUTL::PhysOrientation orientation)
{
  if (orientation == eUTL::PhysOrientationE::R180)
    return Orient::R180;
  if (orientation == eUTL::PhysOrientationE::MX)
    return Orient::MX;
  if (orientation == eUTL::PhysOrientationE::MY)
    return Orient::MY;
  return Orient::R0;
}

std::string orientationName(eUTL::PhysOrientation orientation)
{
  if (orientation == eUTL::PhysOrientationE::R0)
    return "R0";
  if (orientation == eUTL::PhysOrientationE::R90)
    return "R90";
  if (orientation == eUTL::PhysOrientationE::R180)
    return "R180";
  if (orientation == eUTL::PhysOrientationE::R270)
    return "R270";
  if (orientation == eUTL::PhysOrientationE::MX)
    return "MX";
  if (orientation == eUTL::PhysOrientationE::MX90)
    return "MX90";
  if (orientation == eUTL::PhysOrientationE::MY)
    return "MY";
  if (orientation == eUTL::PhysOrientationE::MY90)
    return "MY90";
  return cat("unknown(", static_cast<int>(orientation.getValue()), ")");
}

bool supportedOrientation(eUTL::PhysOrientation orientation)
{
  return orientation == eUTL::PhysOrientationE::R0
         || orientation == eUTL::PhysOrientationE::R180
         || orientation == eUTL::PhysOrientationE::MX
         || orientation == eUTL::PhysOrientationE::MY;
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
    case ipl::Layer::Vt::S:
      return 0;
    case ipl::Layer::Vt::L:
      return 1;
    case ipl::Layer::Vt::H:
      return 2;
    case ipl::Layer::Vt::UL:
      return 3;
    case ipl::Layer::Vt::Unknown:
      break;
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
  repair_config_.verbose = log_.enabled();
  if (grid_ == nullptr || network_ == nullptr) {
    logIssue("MissingDependency", "Grid and Network are required", true);
    return;
  }

  // Planner x is core-relative DBU and row is the Grid row index, matching the
  // coordinates used by CheckRequest.
  placement_.siteWidth = grid_->getSiteWidth().v;
  const int rowCount = grid_->getRowCount().v;
  placement_.rows.reserve(static_cast<size_t>(std::max(rowCount, 0)));
  for (GridY y{0}; y < grid_->getRowCount(); ++y) {
    placement_.rows.push_back(y.v);
  }
  if (!placement_.rows.empty()) {
    placement_.rowHeight
        = grid_->gridYToDbu(GridY{1}).v - grid_->gridYToDbu(GridY{0}).v;
  }
  placement_.byRow.resize(placement_.rows.size());
  if (placement_.siteWidth <= 0 || placement_.rowHeight <= 0
      || placement_.rows.empty()) {
    logIssue("MissingRowGeometry",
             cat("rows=",
                 placement_.rows.size(),
                 " siteWidth=",
                 placement_.siteWidth,
                 " rowHeight=",
                 placement_.rowHeight),
             true);
  }

  const auto heightInRows = [this](DbCoord height) {
    return placement_.rowHeight > 0
               ? std::max<DbCoord>(
                     (height + placement_.rowHeight - 1) / placement_.rowHeight,
                     1)
               : 1;
  };
  const auto implantLayerOf = [&](ipl::LayerId id) -> const ipl::Layer* {
    for (const ipl::Layer& layer : checker_.getLayers()) {
      if (layer.getId() == id) {
        return &layer;
      }
    }
    return nullptr;
  };

  const auto& checkerMasters = checker_.getMasterItems();
  placement_.masters.reserve(network_->getMasters().size());
  for (const auto& [networkMasterIndex, masterPtr] : network_->getMasters()) {
    const Master* master = masterPtr.get();
    if (master == nullptr) {
      logIssue("NullNetworkMaster",
               cat("Network master slot ", networkMasterIndex, " is null"));
      continue;
    }
    const MasterId id = static_cast<MasterId>(master->getId());
    if (id < 0) {
      logIssue("InvalidNetworkMasterId",
               cat("Network master slot ", networkMasterIndex, " has id ", id));
      continue;
    }
    const auto checkerMaster = checkerMasters.find(id);
    if (checkerMaster == checkerMasters.end()) {
      logIssue("MissingCheckerMaster",
               cat("Network master ", id, " has no checker MasterItem"));
      continue;
    }

    const ipl::MasterItem& item = checkerMaster->second;
    MasterInfo info;
    info.id = id;
    info.width = item.width;
    info.height = heightInRows(item.height);
    info.isFiller = master->isFiller();

    DbCoord bottomYl = 0;
    bool haveBottom = false;
    for (const ipl::MasterShape& shape : item.shapes) {
      const ipl::Layer* layer = implantLayerOf(shape.layer);
      if (layer == nullptr) {
        continue;
      }
      if (info.vt == kUnknownVt && layer->getVt() != ipl::Layer::Vt::Unknown) {
        info.vt = toPlannerVt(layer->getVt());
      }
      const DbCoord yl = shape.rect.getYL().getStorage();
      if (!haveBottom || yl < bottomYl) {
        haveBottom = true;
        bottomYl = yl;
        info.bottomBandPolarity = layer->getPolar() == ipl::Layer::Polar::P
                                      ? BandPolarity::P
                                      : BandPolarity::N;
      }
    }

    ensureSlot(placement_.masters, static_cast<size_t>(id));
    placement_.masters[id]
        = PlacementSnapshot::MasterRef{info, master->getDbMaster()};
  }

  // Prefer the configured filler list. Checker-helper tests have no real
  // fillerSetting, so they use the same Network filler flags as the engine.
  {
    log_.section("candidate", "CONFIGURED FILLER MASTERS");
    std::vector<MasterId> configured;
    std::string source = "Network filler flags";
    if (const fillerSetting* setting = network_->getFillerSetting()) {
      const auto& configuredCells = setting->getFillerCells();
      if (!configuredCells.empty()) {
        source = "fillerSetting";
        for (size_t i = 0; i < configuredCells.size(); ++i) {
          const int id = network_->getMasterId(configuredCells[i]);
          if (id < 0) {
            logIssue("ConfiguredMasterNotInNetwork",
                     cat("configured filler ", i, " is not registered"));
            continue;
          }
          configured.push_back(static_cast<MasterId>(id));
        }
      }
    }
    if (configured.empty()) {
      for (const auto& [id, master] : network_->getMasters()) {
        if (master != nullptr && master->isFiller()) {
          configured.push_back(static_cast<MasterId>(id));
        }
      }
    }
    for (const MasterId id : configured) {
      const MasterInfo* info = masterInfo(id);
      if (info == nullptr || !info->isFiller) {
        logIssue(
            "UnusableFillerMaster",
            cat("candidate master ", id, " has no filler planner metadata"));
        continue;
      }
      placement_.fillerMasterIds.push_back(id);
    }
    std::sort(placement_.fillerMasterIds.begin(),
              placement_.fillerMasterIds.end());
    placement_.fillerMasterIds.erase(
        std::unique(placement_.fillerMasterIds.begin(),
                    placement_.fillerMasterIds.end()),
        placement_.fillerMasterIds.end());
    std::string ids;
    for (const MasterId id : placement_.fillerMasterIds) {
      if (!ids.empty())
        ids += ',';
      ids += std::to_string(id);
    }
    log_.block("candidate",
               "Candidate provider",
               {{"source", source},
                {"configured", cat(configured.size())},
                {"accepted", cat(placement_.fillerMasterIds.size())},
                {"master IDs", cat('[', ids, ']')}});
  }
  if (placement_.fillerMasterIds.empty()) {
    logIssue(
        "NoFillerCandidate",
        "no usable filler master; requests needing repair will be skipped");
  }

  placement_.instances.reserve(network_->getNodes().size());
  for (const auto& [networkNodeIndex, nodePtr] : network_->getNodes()) {
    const Node* node = nodePtr.get();
    if (node == nullptr) {
      logIssue("NullNetworkNode",
               cat("Network node slot ", networkNodeIndex, " is null"));
      continue;
    }
    if (node->getMaster() == nullptr) {
      logIssue("MissingNodeMaster",
               cat("Network node ", node->getId(), " has no master"));
      continue;
    }
    if (!node->isPlaced() && !node->isFixed()) {
      continue;
    }
    const RowId rowId = static_cast<RowId>(grid_->gridSnapDownY(node).v);
    if (rowId < 0 || rowId >= static_cast<RowId>(placement_.rows.size())) {
      logIssue("NodeOutsideGrid",
               cat("Network node ", node->getId(), " maps to row ", rowId));
      continue;
    }
    const MasterId masterId = static_cast<MasterId>(node->getMaster()->getId());
    if (masterInfo(masterId) == nullptr) {
      logIssue("MissingNodeMasterMetadata",
               cat("Network node ",
                   node->getId(),
                   " references skipped master ",
                   masterId));
      continue;
    }
    const MasterInfo& info
        = placement_.masters[static_cast<size_t>(masterId)]->info;
    const InstanceId id = static_cast<InstanceId>(node->getId());
    if (id < 0) {
      logIssue("InvalidNetworkNodeId",
               cat("Network node slot ", networkNodeIndex, " has id ", id));
      continue;
    }
    const DbCoord x = node->getLeft().v;
    PlacedInstance placed{id,
                          masterId,
                          rowId,
                          x,
                          toPlannerOrient(node->getOrient()),
                          node->isFiller()};
    if (placed.isFiller && info.vt == kUnknownVt) {
      logIssue("FillerWithoutVt",
               cat("placed filler ",
                   id,
                   " uses master ",
                   masterId,
                   " without implant VT metadata"));
    }
    ensureSlot(placement_.instances, static_cast<size_t>(id));
    placement_.instances[id] = PlacementSnapshot::InstanceRef{
        placed,
        {node->getDbInst(),
         node->getMaster()->getDbMaster(),
         eUTL::UvDist(x),
         eUTL::UvDist(node->getBottom().v),
         node->getOrient()}};
    const DbCoord heightRows = std::max<DbCoord>(info.height, 1);
    for (DbCoord offset = 0; offset < heightRows; ++offset) {
      const RowId row = rowId + static_cast<RowId>(offset);
      if (row >= static_cast<RowId>(placement_.byRow.size()))
        break;
      PlacedInstance rowCopy = placed;
      rowCopy.rowId = row;
      placement_.byRow[row].push_back(rowCopy);
    }
  }
  for (std::vector<PlacedInstance>& list : placement_.byRow) {
    std::sort(list.begin(),
              list.end(),
              [](const PlacedInstance& a, const PlacedInstance& b) {
                return a.x != b.x ? a.x < b.x : a.id < b.id;
              });
  }

  candidate_catalog_.build(placement_, log_);
  if (!candidate_catalog_.hasPlacedCandidate()) {
    logIssue("NoCompatibleFillerCandidate",
             "no placed filler has a compatible replacement master");
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

    const int reachSites = checker_.getMaxRuleValue();
    const DbCoord checkerReach
        = static_cast<DbCoord>(reachSites) * placement_.siteWidth;

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

  const auto placedCount
      = std::count_if(placement_.instances.begin(),
                      placement_.instances.end(),
                      [](const auto& slot) { return slot.has_value(); });
  const auto masterCount
      = std::count_if(placement_.masters.begin(),
                      placement_.masters.end(),
                      [](const auto& slot) { return slot.has_value(); });
  log_.block(
      "engine",
      "Placement view",
      {{"nodes", cat(placedCount)},
       {"masters", cat(masterCount)},
       {"configured filler masters", cat(placement_.fillerMasterIds.size())},
       {"site width", cat(placement_.siteWidth)},
       {"default halo X", cat(placement_.defaultHaloX)}});
}

void FillerRepairEngine::Impl::logIssue(const std::string& code,
                                        const std::string& message,
                                        bool blocking)
{
  if (blocking)
    setup_ok_ = false;
  log_.block("engine",
             blocking ? "Initialization blocked" : "Skipped input",
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
    result.diagnostics.push_back(
        makeDiag(Severity::Error,
                 "UnknownInstance",
                 cat("instance ", fillerInstanceId, " not found")));
    return result;
  }
  if (!placed->isFiller) {
    result.diagnostics.push_back(
        makeDiag(Severity::Warning,
                 "NotAFiller",
                 cat("instance ", fillerInstanceId, " is not a filler")));
    return result;
  }
  const MasterInfo* current = masterInfo(placed->masterId);
  if (current == nullptr || !current->isFiller) {
    result.diagnostics.push_back(makeDiag(
        Severity::Error,
        "UnknownMaster",
        cat("invalid current filler master for instance ", fillerInstanceId)));
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
          ? cat("filler ",
                fillerInstanceId,
                " master ",
                placed->masterId,
                " has no configured replacement matching size, different VT, "
                "and polarity: checked=",
                stats->checked,
                " rejects{notFiller=",
                stats->notFiller,
                " unknownVt=",
                stats->unknownVt,
                " sameVt=",
                stats->sameVt,
                " widthMismatch=",
                stats->widthMismatch,
                " heightMismatch=",
                stats->heightMismatch,
                " polarityMismatch=",
                stats->polarityMismatch,
                '}')
          : cat("filler ",
                fillerInstanceId,
                " master ",
                placed->masterId,
                " is absent from the candidate catalog")));
  return result;
}

// --- seam 2: turning checker answers into oracle results --------------------

::Rect FillerRepairEngine::Impl::toGuardRect(const Region& region) const
{
  // The checker's isInGuard uses the synthetic frame y = rowId * rowHeight;
  // (rowHi+1)*rowHeight - 1 keeps a touching adjacent row out.
  const DbCoord yl = static_cast<DbCoord>(region.rowLo) * placement_.rowHeight;
  const DbCoord yh
      = static_cast<DbCoord>(region.rowHi + 1) * placement_.rowHeight - 1;
  return ::Rect(eUTL::UvDist(static_cast<int64_t>(region.x.xl)),
                eUTL::UvDist(static_cast<int64_t>(yl)),
                eUTL::UvDist(static_cast<int64_t>(region.x.xh)),
                eUTL::UvDist(static_cast<int64_t>(yh)));
}

Violation FillerRepairEngine::Impl::toPlannerViolation(
    const ipl::Violation& v,
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

// Convert planner ids to the shared pre-commit change record. Orientation is
// preserved because the checker uses it to choose the implant band.
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
                          eUTL::PhysOrientation(eUTL::PhysOrientationE::R0)};
  const bool haveRef
      = instanceId >= 0
        && static_cast<size_t>(instanceId) < placement_.instances.size()
        && placement_.instances[instanceId].has_value();
  if (haveRef) {
    const PlacementSnapshot::RecordRef& ref
        = placement_.instances[instanceId]->record;
    record.cell_data_ = dpl2::CellData{ref.cellId};
    record.orig_lib_cell_ = ref.libCellId;
    record.x_ = ref.x;
    record.y_ = ref.y;
    record.orientation_ = ref.orientation;
  }
  if (masterInfo(newMasterId) != nullptr) {
    record.new_lib_cell_ = placement_.masters[newMasterId]->libCellId;
  }
  return record;
}

OracleResult FillerRepairEngine::Impl::checkPlaceWithOverlay(
    const ipl::CheckRequest& target,
    const OracleRequest& request) const
{
  std::vector<OracleResult> results = checkPlaceWithOverlays(target, {request});
  if (results.size() == 1) {
    return std::move(results.front());
  }
  OracleResult failure;
  failure.requestId = request.requestId;
  failure.status = OracleStatus::CheckerError;
  failure.diagnostics.push_back(makeDiag(
      Severity::Fatal,
      "CheckerProtocolError",
      "checker result count does not match the single oracle request"));
  return failure;
}

std::vector<OracleResult> FillerRepairEngine::Impl::checkPlaceWithOverlays(
    const ipl::CheckRequest& target,
    const std::vector<OracleRequest>& requests) const
{
  if (requests.empty()) {
    return {};
  }

  const OracleRequest& first = requests.front();
  const ::Rect guard = toGuardRect(first.guardRegion);
  std::vector<ipl::FillerChanges> changes;
  changes.reserve(requests.size());
  for (const OracleRequest& request : requests) {
    changes.push_back(request.fillerChanges);
  }

  const std::vector<ipl::CheckResult> raw
      = checker_.checkPlaceWithOverlays(target, guard, changes);
  log_.block(
      "engine",
      "Overlay batch",
      {{"candidates", cat(changes.size())}, {"results", cat(raw.size())}});

  // Results correlate by input order. A cardinality mismatch cannot be safely
  // recovered because it would associate checker answers with wrong changes.
  if (raw.size() != requests.size()) {
    log_.block("engine",
               "Overlay batch protocol error",
               {{"expected results", cat(requests.size())},
                {"received results", cat(raw.size())}});
    return std::vector<OracleResult>(raw.size());
  }

  const auto& initDiags = checker_diagnostics_;
  const auto requestDiagOffset
      = [&initDiags](const std::vector<ipl::Diagnostic>& diagnostics) {
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
            if (!matches)
              break;
            offset += initDiags.size();
          }
          return offset;
        };

  std::vector<OracleResult> results(requests.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    OracleResult& out = results[i];
    out.requestId = requests[i].requestId;
    const ipl::CheckResult& checked = raw[i];
    for (size_t d = requestDiagOffset(checked.diagnostics);
         d < checked.diagnostics.size();
         ++d) {
      out.diagnostics.push_back(makeDiag(Severity::Warning,
                                         checked.diagnostics[d].status,
                                         checked.diagnostics[d].message));
    }
    out.violations.reserve(checked.violations.size());
    for (const ipl::Violation& violation : checked.violations) {
      Violation converted
          = toPlannerViolation(violation, first.targetPlace.instanceId);
      const InstanceId checkerTargetId
          = target.cell != nullptr ? target.cell->getId() : -1;
      for (ViolationParticipant& participant : converted.participants) {
        if (participant.instanceId != checkerTargetId) {
          continue;
        }
        participant.instanceId = first.targetPlace.instanceId;
        participant.isTarget = true;
        participant.masterId = first.targetPlace.masterId;
        participant.rowId = first.targetPlace.rowId;
        const MasterInfo* targetMaster
            = masterInfo(first.targetPlace.masterId);
        participant.xRange
            = XInterval{first.targetPlace.x,
                        first.targetPlace.x
                            + (targetMaster != nullptr ? targetMaster->width
                                                       : 0)};
        participant.isFiller = false;
      }
      out.violations.push_back(std::move(converted));
    }
    if (out.violations.empty() && !checked.isLegal
        && !out.diagnostics.empty()) {
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
  const DbCoord heightRows
      = master != nullptr ? std::max<DbCoord>(master->height, 1) : 1;
  Region guard;
  guard.x = XInterval{target.x - placement_.defaultHaloX,
                      target.x + width + placement_.defaultHaloX};
  const RowId minRow = placement_.rows.empty() ? 0 : placement_.rows.front();
  const RowId maxRow = placement_.rows.empty() ? 0 : placement_.rows.back();
  guard.rowLo = std::max<RowId>(
      minRow, target.rowId - static_cast<RowId>(kSnapshotHaloRows));
  guard.rowHi
      = std::min<RowId>(maxRow,
                        target.rowId + static_cast<RowId>(heightRows) - 1
                            + static_cast<RowId>(kSnapshotHaloRows));
  return guard;
}

void FillerRepairEngine::Impl::logRepairSuccess(
    const ipl::CheckRequest& request,
    const RepairOutcome& result,
    const std::string& status) const
{
  if (!log_.enabled()) {
    return;
  }

  const auto nodeName = [this](const Node* node) {
    if (node == nullptr) {
      return std::string{"<unresolved>"};
    }
    if (checker_.getDesign() != nullptr && node->getDbInst().isValid()) {
      return checker_.cellName(node);
    }
    return std::string{"<unavailable>"};
  };
  const auto masterName = [this](MasterId masterId) -> std::string {
    const Master* master
        = masterId >= 0 ? network_->getMaster(masterId) : nullptr;
    const eLIB::PhysLibCell* physical
        = master != nullptr ? master->getPhysLibCell() : nullptr;
    if (physical != nullptr) {
      return std::string{physical->getLibCell().getName()};
    }
    return masterId >= 0 ? std::string{"<unavailable>"}
                         : std::string{"<none>"};
  };
  const auto masterSiteWidth = [this](MasterId masterId) {
    const MasterInfo* master = masterInfo(masterId);
    if (master == nullptr || placement_.siteWidth <= 0) {
      return std::string{"<unavailable>"};
    }
    if (master->width % placement_.siteWidth != 0) {
      return cat(master->width,
                 " DBU (site=",
                 placement_.siteWidth,
                 " DBU; not aligned)");
    }
    return cat(master->width / placement_.siteWidth,
               " site(s) (",
               master->width,
               " DBU; site=",
               placement_.siteWidth,
               " DBU)");
  };
  const auto dbCellId = [](const eUNL::LeafCellID* cellId) {
    return cellId != nullptr && cellId->isValid()
               ? cat(cellId->getIndexValue())
               : std::string{"<none>"};
  };

  const size_t addCount = std::count_if(
      result.changes.begin(), result.changes.end(), [](const auto& change) {
        return change.op_ == OpType::Add;
      });
  const size_t swapCount = std::count_if(
      result.changes.begin(), result.changes.end(), [](const auto& change) {
        return change.op_ == OpType::Replace;
      });
  log_.section("engine", "REPAIR SUCCESS");
  log_.block("engine",
             "Repair result",
             {{"status", status},
              {"caller Delete overlays", cat(request.overlayChanges.size())},
              {"returned Add changes", cat(addCount)},
              {"returned Swap changes", cat(swapCount)}});

  for (size_t index = 0; index < request.overlayChanges.size(); ++index) {
    const CellChangeRecord& overlay = request.overlayChanges[index];
    const eUNL::LeafCellID* cellId = cellChangeRecordLeafCellId(overlay);
    const InstanceId instanceId
        = cellId != nullptr ? network_->getNodeId(*cellId) : -1;
    const Node* node = instanceId >= 0 ? network_->getNode(instanceId) : nullptr;
    const MasterId oldMasterId
        = node != nullptr && node->getMaster() != nullptr
              ? node->getMaster()->getId()
              : network_->getMasterId(overlay.orig_lib_cell_);
    const eUTL::PhysOrientation orientation
        = node != nullptr ? node->getOrient() : overlay.orientation_;
    const int64_t x = node != nullptr ? node->getLeft().v
                                      : overlay.x_.getStorage();
    const int64_t y = node != nullptr ? node->getBottom().v
                                      : overlay.y_.getStorage();
    log_.block(
        "engine",
        cat("Caller Delete overlay #", index + 1),
        {{"operation", "DELETE (caller input)"},
         {"cell kind",
          node == nullptr ? "<unresolved>"
                          : (node->isFiller() ? "filler" : "standard cell")},
         {"old cell id", instanceId >= 0 ? cat(instanceId) : "<unresolved>"},
         {"old DB cell id", dbCellId(cellId)},
         {"old cell name", nodeName(node)},
         {"old master id",
          oldMasterId >= 0 ? cat(oldMasterId) : "<unresolved>"},
         {"old master name", masterName(oldMasterId)},
         {"old site width", masterSiteWidth(oldMasterId)},
         {"old orientation", orientationName(orientation)},
         {"origin (core DBU)", cat('(', x, ',', y, ')')},
         {"new state", "deleted by caller before target placement"}});
  }

  for (size_t index = 0; index < result.changes.size(); ++index) {
    const CellChangeRecord& change = result.changes[index];
    const bool isAdd = change.op_ == OpType::Add;
    const eUNL::LeafCellID* cellId = cellChangeRecordLeafCellId(change);
    const InstanceId instanceId
        = cellId != nullptr ? network_->getNodeId(*cellId) : -1;
    const Node* oldNode
        = instanceId >= 0 ? network_->getNode(instanceId) : nullptr;
    const MasterId oldMasterId
        = oldNode != nullptr && oldNode->getMaster() != nullptr
              ? oldNode->getMaster()->getId()
              : network_->getMasterId(change.orig_lib_cell_);
    const MasterId newMasterId
        = network_->getMasterId(change.new_lib_cell_);
    const std::string* addedName
        = std::get_if<std::string>(&change.cell_data_);
    const std::string oldCellId
        = isAdd ? "<none>"
                : (instanceId >= 0 ? cat(instanceId) : "<unresolved>");
    const std::string newCellId
        = isAdd ? "<assigned on commit>" : oldCellId;
    const std::string oldName = isAdd ? "<none>" : nodeName(oldNode);
    const std::string newName
        = isAdd ? (addedName != nullptr ? *addedName : "<invalid Add name>")
                : oldName;
    const std::string oldOrientation
        = isAdd || oldNode == nullptr
              ? (isAdd ? "<none>" : orientationName(change.orientation_))
              : orientationName(oldNode->getOrient());
    log_.block(
        "engine",
        cat("Returned repair change #", index + 1),
        {{"operation",
          isAdd ? "ADD" : (change.op_ == OpType::Replace
                                 ? "SWAP (Replace)"
                                 : "UNEXPECTED")},
         {"old cell id", oldCellId},
         {"old DB cell id", isAdd ? "<none>" : dbCellId(cellId)},
         {"old cell name", oldName},
         {"new cell id", newCellId},
         {"new DB cell id",
          isAdd ? "<assigned on commit>" : dbCellId(cellId)},
         {"new cell name", newName},
         {"old master id", isAdd ? "<none>" : cat(oldMasterId)},
         {"old master name", isAdd ? "<none>" : masterName(oldMasterId)},
         {"old site width",
          isAdd ? "<none>" : masterSiteWidth(oldMasterId)},
         {"new master id", cat(newMasterId)},
         {"new master name", masterName(newMasterId)},
         {"new site width", masterSiteWidth(newMasterId)},
         {"old orientation", oldOrientation},
         {"new orientation", orientationName(change.orientation_)},
         {"origin (core DBU)",
          cat('(', change.x_.getStorage(), ',', change.y_.getStorage(), ')')}});
  }
}

RepairOutcome FillerRepairEngine::Impl::repair(
    const ipl::CheckRequest& request) const
{
  RepairOutcome result;
  log_.section("engine", "REPAIR REQUEST");
  const auto skip = [this](const std::string& code, const std::string& reason) {
    log_.block(
        "engine", "Repair skipped", {{"code", code}, {"reason", reason}});
  };

  if (!setup_ok_) {
    skip("EngineNotReady", "required Grid/Network geometry is unavailable");
    return result;
  }
  if (request.cell == nullptr || request.cell->getMaster() == nullptr) {
    skip("MissingTarget", "CheckRequest has no temporary Node/master");
    return result;
  }
  if (request.overlayChanges.empty()) {
    skip("MissingOverlay", "CheckRequest has no replaced Network node");
    return result;
  }

  std::set<InstanceId> overlayInstanceIds;
  for (const CellChangeRecord& overlay : request.overlayChanges) {
    const eUNL::LeafCellID* cellId = cellChangeRecordLeafCellId(overlay);
    if (overlay.op_ != OpType::Delete || cellId == nullptr) {
      skip("InvalidOverlay",
           "every target overlay must be a Delete carrying LeafCellID");
      return result;
    }
    const InstanceId id = network_->getNodeId(*cellId);
    if (id < 0 || instance(id) == nullptr
        || !overlayInstanceIds.insert(id).second) {
      skip("InvalidOverlay",
           cat("target overlay has an unknown or duplicate node ", id));
      return result;
    }
  }

  const DbCoord targetX
      = static_cast<DbCoord>(request.x.v) * placement_.siteWidth;
  InstanceId targetId = -1;
  const InstanceId requestedId = request.cell->getId();
  if (overlayInstanceIds.count(requestedId) != 0
      && instance(requestedId) != nullptr) {
    targetId = requestedId;
  }
  for (const InstanceId id : overlayInstanceIds) {
    if (targetId >= 0) {
      break;
    }
    const PlacedInstance* candidate = instance(id);
    const MasterInfo* candidateMaster
        = candidate != nullptr ? masterInfo(candidate->masterId) : nullptr;
    if (candidateMaster == nullptr) {
      continue;
    }
    const DbCoord heightRows = std::max<DbCoord>(candidateMaster->height, 1);
    if (candidate->x <= targetX
        && targetX < candidate->x + candidateMaster->width
        && candidate->rowId <= request.y.v
        && request.y.v < candidate->rowId + heightRows) {
      targetId = id;
      break;
    }
  }
  if (targetId < 0 && !overlayInstanceIds.empty()) {
    targetId = *overlayInstanceIds.begin();
  }
  if (targetId < 0) {
    skip("MissingTargetId",
         "temporary target has no anchor among the Delete overlays");
    return result;
  }
  const PlacedInstance* current = instance(targetId);
  if (current == nullptr) {
    skip("UnknownTarget",
         cat("overlay target ",
             targetId,
             " is absent from the repair snapshot"));
    return result;
  }

  const MasterId replacementId = request.cell->getMaster()->getId();
  const MasterInfo* replacement = masterInfo(replacementId);
  if (replacement == nullptr) {
    skip("TargetMasterUnavailable",
         cat("temporary Node master ",
             replacementId,
             " was skipped during initialization"));
    return result;
  }

  TargetPlace target;
  target.instanceId = current->id;
  target.masterId = replacementId;
  target.rowId = request.y.v;
  target.x = targetX;
  target.orientation = toPlannerOrient(request.orientation);

  Region influence = snapshotGuard(target);
  const bool allFillerOverlays
      = std::all_of(overlayInstanceIds.begin(),
                    overlayInstanceIds.end(),
                    [this](InstanceId id) {
                      const PlacedInstance* placed = instance(id);
                      return placed != nullptr && placed->isFiller;
                    });
  std::set<internal::SiteCell> releasedSites;
  bool exactCover = true;
  if (allFillerOverlays) {
    if (replacement->width <= 0
        || replacement->width % placement_.siteWidth != 0
        || replacement->height < 1) {
      skip("UnsupportedTargetFootprint",
           "temporary target must be site aligned with a positive row height");
      return result;
    }
    const int targetWidthSites
        = static_cast<int>(replacement->width / placement_.siteWidth);
    const int targetHeightRows = static_cast<int>(replacement->height);
    const int targetCol = request.x.v;
    const int rowCount = grid_->getRowCount().v;
    const int colCount = grid_->getRowSiteCount().v;
    if (targetCol < 0 || target.rowId < 0
        || targetCol + targetWidthSites > colCount
        || target.rowId + targetHeightRows > rowCount) {
      skip("TargetFootprintOutOfGrid",
           "temporary target footprint extends outside the placement grid");
      return result;
    }

    std::set<internal::SiteCell> targetSites;
    std::set<internal::SiteCell> deletedSites;
    for (int rowOffset = 0; rowOffset < targetHeightRows; ++rowOffset) {
      for (int colOffset = 0; colOffset < targetWidthSites; ++colOffset) {
        targetSites.insert(internal::SiteCell{
            target.rowId + rowOffset, targetCol + colOffset});
      }
    }
    for (const InstanceId id : overlayInstanceIds) {
      const PlacedInstance* filler = instance(id);
      const MasterInfo* master
          = filler != nullptr ? masterInfo(filler->masterId) : nullptr;
      if (filler == nullptr || master == nullptr || !master->isFiller
          || master->width <= 0
          || master->width % placement_.siteWidth != 0
          || master->height < 1 || master->height > 2
          || filler->x < 0 || filler->x % placement_.siteWidth != 0) {
        skip("UnsupportedDeletedFiller",
             cat("Delete overlay filler ", id,
                 " has an unsupported footprint"));
        return result;
      }
      const int fillerCol
          = static_cast<int>(filler->x / placement_.siteWidth);
      const int fillerWidthSites
          = static_cast<int>(master->width / placement_.siteWidth);
      bool intersectsTarget = false;
      for (int rowOffset = 0; rowOffset < master->height; ++rowOffset) {
        for (int colOffset = 0; colOffset < fillerWidthSites; ++colOffset) {
          const internal::SiteCell site{filler->rowId + rowOffset,
                                        fillerCol + colOffset};
          if (site.rowId < 0 || site.rowId >= rowCount || site.colId < 0
              || site.colId >= colCount) {
            skip("DeletedFillerOutOfGrid",
                 cat("Delete overlay filler ", id,
                     " extends outside the placement grid"));
            return result;
          }
          const Pixel* pixel
              = grid_->gridPixel(GridX{site.colId}, GridY{site.rowId});
          if (pixel == nullptr || !pixel->is_valid
              || pixel->padding_reserved_by != nullptr
              || pixel->cell == nullptr || pixel->cell->getId() != id) {
            skip("InvalidDeletedFillerFootprint",
                 cat("Delete overlay filler ", id,
                     " does not own its complete legal footprint"));
            return result;
          }
          deletedSites.insert(site);
          intersectsTarget
              = intersectsTarget || targetSites.count(site) != 0;
        }
      }
      if (!intersectsTarget) {
        skip("UnrelatedDeletedFiller",
             cat("Delete overlay filler ", id,
                 " does not intersect the temporary target"));
        return result;
      }
      influence.x.xl = std::min(
          influence.x.xl, filler->x - placement_.defaultHaloX);
      influence.x.xh = std::max(influence.x.xh,
                                filler->x + master->width
                                    + placement_.defaultHaloX);
      influence.rowLo
          = std::max<RowId>(placement_.rows.front(),
                            std::min(influence.rowLo, filler->rowId - 1));
      influence.rowHi = std::min<RowId>(
          placement_.rows.back(),
          std::max(influence.rowHi,
                   filler->rowId + static_cast<RowId>(master->height)));
    }

    // The target may cover pre-existing whitespace, but it may not hide an
    // unchanged cell. Only the selected Delete fillers are removed.
    for (const internal::SiteCell& site : targetSites) {
      const Pixel* pixel
          = grid_->gridPixel(GridX{site.colId}, GridY{site.rowId});
      const Node* occupant = pixel != nullptr ? pixel->cell : nullptr;
      if (pixel == nullptr || !pixel->is_valid
          || pixel->padding_reserved_by != nullptr
          || (occupant != nullptr
              && overlayInstanceIds.count(occupant->getId()) == 0)) {
        skip("TargetOverlapsUnchangedInstance",
             cat("target cannot use site row=", site.rowId,
                 " col=", site.colId));
        return result;
      }
    }
    std::set_difference(deletedSites.begin(),
                        deletedSites.end(),
                        targetSites.begin(),
                        targetSites.end(),
                        std::inserter(releasedSites, releasedSites.end()));
    exactCover = deletedSites == targetSites;
  }
  log_.block("engine",
             "Target",
             {{"overlay node", cat(target.instanceId)},
              {"old kind", current->isFiller ? "filler" : "std cell"},
              {"new master", cat(target.masterId)},
              {"row", cat(target.rowId)},
              {"x", cat(target.x)},
              {"orientation", orientationName(request.orientation)},
              {"exact cover", cat(exactCover)},
              {"released sites", cat(releasedSites.size())},
              {"guard", show(influence)}});

  BoundOracle oracle(*this, request);
  if (!releasedSites.empty()) {
    std::string addedFillerNamePrefix = "FILLER_REPAIR_";
    if (const fillerSetting* setting = network_->getFillerSetting();
        setting != nullptr && !setting->getPrefix().empty()) {
      addedFillerNamePrefix = setting->getPrefix();
      if (addedFillerNamePrefix.back() != '_') {
        addedFillerNamePrefix.push_back('_');
      }
      addedFillerNamePrefix += "FILLER_REPAIR_";
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
          internal::FillerFootprint{
              masterId,
              static_cast<int>(master->width / placement_.siteWidth),
              static_cast<int>(master->height)});
      masterLibCells.emplace(masterId,
                             placement_.masters[masterId]->libCellId);
    }
    if (footprints.empty()) {
      skip("NoRetilingFillerMaster",
           "no configured site-aligned one/two-row filler master");
      return result;
    }

    const internal::RetileResult tilings = internal::enumerateRetilings(
        std::vector<internal::SiteCell>(releasedSites.begin(),
                                        releasedSites.end()),
        footprints);
    log_.block("engine",
               "Retiling search",
               {{"deleted fillers", cat(overlayInstanceIds.size())},
                {"released sites", cat(releasedSites.size())},
                {"tilings", cat(tilings.solutions.size())},
                {"search states", cat(tilings.searchStates)},
                {"truncated", cat(tilings.truncated)}});
    if (tilings.solutions.empty()) {
      skip(tilings.truncated ? "RetilingBudgetExceeded"
                             : "ReleasedAreaNotTileable",
           "configured filler footprints cannot exactly cover the released "
           "sites");
      return result;
    }

    int layoutAssignmentsChecked = 0;
    bool layoutAssignmentsTruncated = false;
    for (const std::vector<internal::TiledFiller>& tiling :
         tilings.solutions) {
      std::vector<LayoutAddition> additions;
      bool usable = true;
      int addIndex = 0;
      for (const internal::TiledFiller& tile : tiling) {
        const MasterInfo* footprintMaster = masterInfo(tile.masterId);
        if (footprintMaster == nullptr) {
          usable = false;
          break;
        }
        std::vector<LayoutMasterOption> options;
        MasterId chosen = -1;
        int matchingFootprints = 0;
        int orientedFootprints = 0;
        for (const MasterId candidate : placement_.fillerMasterIds) {
          const MasterInfo* master = masterInfo(candidate);
          const Master* networkMaster = network_->getMaster(candidate);
          const eLIB::PhysLibCell* physCell
              = networkMaster != nullptr ? networkMaster->getPhysLibCell()
                                         : nullptr;
          const eLIB::TechSite* site
              = physCell != nullptr ? physCell->getTechSite() : nullptr;
          if (master == nullptr || networkMaster == nullptr
              || !networkMaster->isFiller()
              || master->width != footprintMaster->width
              || master->height != footprintMaster->height) {
            continue;
          }
          ++matchingFootprints;
          std::optional<eUTL::PhysOrientation> orientation;
          if (site != nullptr) {
            orientation
                = grid_->getSiteOrientation(GridX{tile.colId},
                                            GridY{tile.rowId},
                                            site->getName());
          } else {
            // Database-free checker fixtures do not carry PhysLibCell/site
            // objects. The released site is still painted by a selected
            // filler, whose committed orientation is the row orientation.
            const Pixel* released
                = grid_->gridPixel(GridX{tile.colId}, GridY{tile.rowId});
            if (released != nullptr && released->cell != nullptr) {
              orientation = released->cell->getOrient();
            }
          }
          if (!orientation.has_value()
              || !supportedOrientation(*orientation)) {
            continue;
          }
          ++orientedFootprints;
          options.push_back(LayoutMasterOption{candidate, *orientation});
          if (chosen < 0
              || (master->vt == replacement->vt
                  && masterInfo(chosen)->vt != replacement->vt)) {
            chosen = candidate;
          }
        }
        if (chosen < 0) {
          log_.block("engine",
                     "Unusable retiling tile",
                     {{"row", cat(tile.rowId)},
                      {"col", cat(tile.colId)},
                      {"footprint master", cat(tile.masterId)},
                      {"matching footprints", cat(matchingFootprints)},
                      {"oriented footprints", cat(orientedFootprints)},
                      {"configured candidates",
                       cat(placement_.fillerMasterIds.size())}});
          usable = false;
          break;
        }
        const auto chosenOption = std::find_if(
            options.begin(), options.end(), [chosen](const auto& option) {
              return option.masterId == chosen;
            });
        std::rotate(options.begin(), chosenOption, chosenOption + 1);

        const std::string name
            = cat(addedFillerNamePrefix,
                  tile.rowId,
                  '_',
                  tile.colId,
                  "_W",
                  footprintMaster->width,
                  "_H",
                  footprintMaster->height,
                  '_',
                  addIndex);
        const eUTL::PhysOrientation orientation = options.front().orientation;
        CellChangeRecord addition{
            OpType::Add,
            CellData{name},
            eUTL::UvDist(static_cast<int64_t>(tile.colId)
                         * placement_.siteWidth),
            eUTL::UvDist(
                static_cast<int64_t>(grid_->gridYToDbu(GridY{tile.rowId}).v)),
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
        local.record = std::move(addition);
        local.masterOptions = std::move(options);
        additions.push_back(std::move(local));
      }
      if (!usable) {
        continue;
      }

      std::vector<size_t> assignment(additions.size(), 0);
      bool haveAssignment = true;
      while (haveAssignment) {
        if (repair_config_.checkerCallBudgetPerRepair > 0
            && layoutAssignmentsChecked
                   >= repair_config_.checkerCallBudgetPerRepair) {
          layoutAssignmentsTruncated = true;
          break;
        }
        ++layoutAssignmentsChecked;
        std::vector<LayoutAddition> seededAdditions = additions;
        ipl::FillerChanges fixed;
        for (size_t index = 0; index < seededAdditions.size(); ++index) {
          LayoutAddition& addition = seededAdditions[index];
          const LayoutMasterOption& option
              = addition.masterOptions[assignment[index]];
          const auto libCell = masterLibCells.find(option.masterId);
          if (libCell == masterLibCells.end()) {
            usable = false;
            break;
          }
          addition.placed.masterId = option.masterId;
          addition.placed.orientation = toPlannerOrient(option.orientation);
          addition.record.new_lib_cell_ = libCell->second;
          addition.record.orientation_ = option.orientation;
          fixed.push_back(addition.record);
        }
        if (!usable) {
          break;
        }

        LayoutPlacementView layoutView(
            *this,
            target,
            overlayInstanceIds,
            seededAdditions,
            masterLibCells);
        LayoutOracle layoutOracle(oracle, layoutView, fixed);
        OracleRequest layoutSnapshotRequest;
        layoutSnapshotRequest.requestId = 0;
        layoutSnapshotRequest.targetPlace = target;
        layoutSnapshotRequest.guardRegion = influence;
        const OracleResult layoutSnapshot
            = layoutOracle.checkPlaceWithOverlay(layoutSnapshotRequest);
        log_.block("engine",
                   "Retiling checker result",
                   {{"status", cat(static_cast<int>(layoutSnapshot.status))},
                    {"violations", cat(layoutSnapshot.violations.size())},
                    {"diagnostics", cat(layoutSnapshot.diagnostics.size())}});
        if (layoutSnapshot.status != OracleStatus::Checked) {
          for (const Diagnostic& diagnostic : layoutSnapshot.diagnostics) {
            log_.block("engine",
                       "Rejected retiling",
                       {{"code", diagnostic.code},
                        {"message", diagnostic.message}});
          }
          haveAssignment
              = advanceLayoutAssignment(assignment, additions);
          continue;
        }
        if (layoutSnapshot.violations.empty()) {
          result.hasSolution = true;
          result.changes = std::move(fixed);
          logRepairSuccess(request, result, "retiled solution");
          return result;
        }

        FillerRepairRequest plannerRequest;
        plannerRequest.targetPlace = target;
        plannerRequest.violations = layoutSnapshot.violations;
        internal::RepairPlanner planner(
            layoutView, layoutOracle, repair_config_);
        const FillerRepairResult planned = planner.repair(plannerRequest);
        if (planned.hasSolution) {
          result.hasSolution = true;
          result.changes = mergeFillerChanges(fixed, planned.changes);
          logRepairSuccess(request, result, "retiled and repaired");
          return result;
        }
        // The planner explores the alternate masters exposed by this tiling,
        // so another seed assignment cannot add a new swap state.
        break;
      }
      if (layoutAssignmentsTruncated) {
        break;
      }
    }
    skip(layoutAssignmentsTruncated
             ? "RetilingMasterAssignmentBudgetExceeded"
             : "NoLegalRetiling",
         "no exact-cover filler layout passed the implant checker");
    return result;
  }

  OracleRequest snapshotRequest;
  snapshotRequest.requestId = 0;
  snapshotRequest.targetPlace = target;
  snapshotRequest.guardRegion = influence;
  const OracleResult snapshot = oracle.checkPlaceWithOverlay(snapshotRequest);
  if (snapshot.status != OracleStatus::Checked) {
    for (const Diagnostic& diagnostic : snapshot.diagnostics) {
      log_.block("engine",
                 "Checker response",
                 {{"code", diagnostic.code}, {"message", diagnostic.message}});
    }
    skip("SnapshotFailed", "checker did not return a usable target snapshot");
    return result;
  }
  if (snapshot.violations.empty()) {
    result.hasSolution = true;
    logRepairSuccess(request, result, "no filler change needed");
    return result;
  }
  if (!candidate_catalog_.hasPlacedCandidate()) {
    const CandidateStats& stats = candidate_catalog_.aggregateStats();
    skip("NoCompatibleFillerCandidate",
         cat("checked=", stats.checked, " compatible=", stats.compatible));
    return result;
  }

  FillerRepairRequest plannerRequest;
  plannerRequest.targetPlace = target;
  plannerRequest.violations = snapshot.violations;
  std::optional<OverlayPlacementView> overlayView;
  const PlacementView& baseView = *this;
  const PlacementView* plannerView = &baseView;
  if (overlayInstanceIds.size() > 1) {
    overlayView.emplace(baseView, overlayInstanceIds, target.instanceId);
    plannerView = &*overlayView;
  }
  internal::RepairPlanner planner(*plannerView, oracle, repair_config_);
  const FillerRepairResult planned = planner.repair(plannerRequest);
  for (const Diagnostic& diagnostic : planned.diagnostics) {
    log_.block("planner",
               "Planner result",
               {{"code", diagnostic.code}, {"message", diagnostic.message}});
  }
  if (!planned.hasSolution) {
    skip("NoSolution", "planner exhausted its repair search");
    return result;
  }

  result.hasSolution = true;
  result.changes = planned.changes;
  logRepairSuccess(request, result, "solution");
  return result;
}

FillerRepairEngine::FillerRepairEngine(const ipl::ImplantLayerChecker& checker)
    : impl_(std::make_unique<Impl>(checker, debugLoggingDefault()))
{
}

FillerRepairEngine::~FillerRepairEngine() = default;

bool FillerRepairEngine::isReady() const
{
  return impl_->ready();
}

RepairOutcome FillerRepairEngine::repair(
    const ipl::CheckRequest& request) const
{
  return impl_->repair(request);
}

}  // namespace fillerRepair
}  // namespace dpl2
