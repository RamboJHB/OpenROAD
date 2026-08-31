// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerRepair/FillerRepairEngine.h>
#include <fillerRepair/RepairPlanner.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <algorithm>
#include <iterator>
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
      out.violations.push_back(
          toPlannerViolation(violation, first.targetPlace.instanceId));
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
  for (const InstanceId id : overlayInstanceIds) {
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
  if (targetId < 0) {
    skip("MissingTargetId",
         "no Delete overlay covers the temporary target origin");
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

  const Region influence = snapshotGuard(target);
  log_.block("engine",
             "Target",
             {{"overlay node", cat(target.instanceId)},
              {"old kind", current->isFiller ? "filler" : "std cell"},
              {"new master", cat(target.masterId)},
              {"row", cat(target.rowId)},
              {"x", cat(target.x)},
              {"orientation", orientationName(request.orientation)},
              {"guard", show(influence)}});

  BoundOracle oracle(*this, request);
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
    log_.block(
        "engine", "Repair result", {{"status", "no filler change needed"}});
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
  log_.block(
      "engine",
      "Repair result",
      {{"status", "solution"}, {"filler changes", cat(result.changes.size())}});
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
