#include <FillerRepairDumpReplay.hh>
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <drc/ImplantLayerCheckerHelper.h>
#include <fillerRepair/RepairPlanner.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/network.h>

namespace dpl2 {
namespace {

namespace fr = fillerRepair;

// [fillerRepair-fix] This command-side bridge reconstructs the two pure
// planner seams from a checker-helper dump; it is not a runtime adapter.

fr::Orient toPlannerOrient(eUTL::PhysOrientation orientation)
{
  if (orientation == eUTL::PhysOrientationE::R180) {
    return fr::Orient::R180;
  }
  if (orientation == eUTL::PhysOrientationE::MX) {
    return fr::Orient::MX;
  }
  if (orientation == eUTL::PhysOrientationE::MY) {
    return fr::Orient::MY;
  }
  return fr::Orient::R0;
}

eUTL::PhysOrientation toCheckerOrient(fr::Orient orientation)
{
  switch (orientation) {
    case fr::Orient::R180:
      return eUTL::PhysOrientationE::R180;
    case fr::Orient::MX:
      return eUTL::PhysOrientationE::MX;
    case fr::Orient::MY:
      return eUTL::PhysOrientationE::MY;
    case fr::Orient::R0:
      return eUTL::PhysOrientationE::R0;
  }
  return eUTL::PhysOrientationE::R0;
}

fr::VtId toPlannerVt(ipl::Layer::Vt vt)
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
      return fr::kUnknownVt;
  }
  return fr::kUnknownVt;
}

class DumpPlacementView final : public fr::PlacementView
{
 public:
  explicit DumpPlacementView(const ipl::ImplantInput& input)
      : site_width_(input.siteWidth), row_height_(input.rowHeight)
  {
    std::map<ipl::LayerId, ipl::Layer> layers;
    for (const ipl::Layer& layer : input.layers) {
      layers[layer.getId()] = layer;
    }

    std::set<fr::MasterId> configuredFillers;
    if (input.fillerSetting.present) {
      configuredFillers.insert(input.fillerSetting.fillerMasterIds.begin(),
                               input.fillerSetting.fillerMasterIds.end());
    }
    for (const ipl::MasterItem& master : input.masters) {
      fr::MasterInfo info;
      info.id = master.masterId;
      info.width = master.width;
      info.height = std::max<fr::DbCoord>(
          1,
          row_height_ > 0
              ? (master.height + row_height_ - 1) / row_height_
              : 1);
      info.isFiller = master.isFiller
                      || configuredFillers.count(master.masterId) != 0;

      bool haveBottomShape = false;
      fr::DbCoord bottomY = 0;
      for (const ipl::MasterShape& shape : master.shapes) {
        const auto layer = layers.find(shape.layer);
        if (layer == layers.end()) {
          continue;
        }
        if (info.vt == fr::kUnknownVt
            && layer->second.getVt() != ipl::Layer::Vt::Unknown) {
          info.vt = toPlannerVt(layer->second.getVt());
        }
        const fr::DbCoord yl = shape.rect.getYL().getStorage();
        if (!haveBottomShape || yl < bottomY) {
          haveBottomShape = true;
          bottomY = yl;
          info.bottomBandPolarity
              = layer->second.getPolar() == ipl::Layer::Polar::P
                    ? fr::BandPolarity::P
                    : fr::BandPolarity::N;
        }
      }
      masters_[info.id] = info;
    }

    if (input.fillerSetting.present) {
      filler_master_ids_.assign(input.fillerSetting.fillerMasterIds.begin(),
                                input.fillerSetting.fillerMasterIds.end());
    } else {
      for (const auto& [id, master] : masters_) {
        if (master.isFiller) {
          filler_master_ids_.push_back(id);
        }
      }
    }
    std::sort(filler_master_ids_.begin(), filler_master_ids_.end());
    filler_master_ids_.erase(
        std::unique(filler_master_ids_.begin(), filler_master_ids_.end()),
        filler_master_ids_.end());

    for (ipl::RowId rowId = 0; rowId < input.rowCount; ++rowId) {
      rows_.push_back(static_cast<fr::RowId>(rowId));
    }

    for (const ipl::PlacedInst& placed : input.placedInsts) {
      fr::PlacedInstance instance;
      instance.id = placed.instanceId;
      instance.masterId = placed.masterId;
      instance.rowId = placed.rowId;
      instance.x = static_cast<fr::DbCoord>(placed.colId) * site_width_;
      instance.orientation = toPlannerOrient(placed.orientation);
      instance.isFiller = placed.isFiller;
      instances_[instance.id] = instance;

      const fr::MasterInfo* master = masterInfo(instance.masterId);
      const fr::DbCoord heightRows = master != nullptr ? master->height : 1;
      for (fr::DbCoord offset = 0; offset < heightRows; ++offset) {
        const fr::RowId row = instance.rowId + static_cast<fr::RowId>(offset);
        if (row < 0 || row >= input.rowCount) {
          continue;
        }
        fr::PlacedInstance rowCopy = instance;
        rowCopy.rowId = row;
        by_row_[row].push_back(rowCopy);
      }
    }
    for (auto& [rowId, instances] : by_row_) {
      (void) rowId;
      std::sort(instances.begin(),
                instances.end(),
                [](const fr::PlacedInstance& left,
                   const fr::PlacedInstance& right) {
                  return left.x != right.x ? left.x < right.x
                                           : left.id < right.id;
                });
    }
  }

  const std::vector<fr::RowId>& rows() const override { return rows_; }
  fr::DbCoord siteWidth() const override { return site_width_; }
  fr::DbCoord rowHeight() const { return row_height_; }

  const std::vector<fr::PlacedInstance>& instancesInRow(
      fr::RowId rowId) const override
  {
    const auto found = by_row_.find(rowId);
    return found != by_row_.end() ? found->second : emptyInstances();
  }

  const fr::PlacedInstance* instance(fr::InstanceId id) const override
  {
    const auto found = instances_.find(id);
    return found != instances_.end() ? &found->second : nullptr;
  }

  const fr::MasterInfo* masterInfo(fr::MasterId id) const override
  {
    const auto found = masters_.find(id);
    return found != masters_.end() ? &found->second : nullptr;
  }

  const std::vector<fr::MasterId>& fillerMasterIds() const override
  {
    return filler_master_ids_;
  }

  CellChangeRecord cellChangeRecord(fr::InstanceId instanceId,
                                    fr::MasterId newMasterId) const override
  {
    const fr::PlacedInstance* placed = instance(instanceId);
    if (placed == nullptr) {
      return CellChangeRecord{OpType::Replace,
                              CellData{eUNL::LeafCellID(0, 0)},
                              eUTL::UvDist(static_cast<int64_t>(0)),
                              eUTL::UvDist(static_cast<int64_t>(0)),
                              eLIB::LibCellID(0, 0),
                              eLIB::LibCellID(0, 0),
                              eUTL::PhysOrientationE::R0};
    }
    return CellChangeRecord{
        OpType::Replace,
        CellData{eUNL::LeafCellID(0, placed->id)},
        eUTL::UvDist(static_cast<int64_t>(placed->x)),
        eUTL::UvDist(static_cast<int64_t>(placed->rowId) * row_height_),
        eLIB::LibCellID(0, placed->masterId),
        eLIB::LibCellID(0, newMasterId),
        toCheckerOrient(placed->orientation)};
  }

 private:
  fr::DbCoord site_width_ = 0;
  fr::DbCoord row_height_ = 0;
  std::vector<fr::RowId> rows_;
  std::map<fr::MasterId, fr::MasterInfo> masters_;
  std::map<fr::InstanceId, fr::PlacedInstance> instances_;
  std::map<fr::RowId, std::vector<fr::PlacedInstance>> by_row_;
  std::vector<fr::MasterId> filler_master_ids_;
};

class DumpCheckerOracle final : public fr::RepairOracle
{
 public:
  DumpCheckerOracle(const DumpPlacementView& view,
                    const ipl::ImplantLayerChecker& checker)
      : view_(view), checker_(checker)
  {
  }

  fr::OracleResult checkPlaceWithOverlay(
      const fr::OracleRequest& request) override
  {
    std::vector<fr::OracleResult> results = checkPlaceWithOverlays({request});
    if (results.size() == 1) {
      return std::move(results.front());
    }
    fr::OracleResult result;
    result.requestId = request.requestId;
    result.status = fr::OracleStatus::CheckerError;
    result.diagnostics.push_back(fr::makeDiag(
        fr::Severity::Fatal,
        "CheckerProtocolError",
        "checker result count does not match the oracle request"));
    return result;
  }

  std::vector<fr::OracleResult> checkPlaceWithOverlays(
      const std::vector<fr::OracleRequest>& requests) override
  {
    std::vector<fr::OracleResult> results(requests.size());
    if (requests.empty()) {
      return results;
    }

    const fr::OracleRequest& first = requests.front();
    for (const fr::OracleRequest& request : requests) {
      if (request.targetPlace.instanceId != first.targetPlace.instanceId
          || request.targetPlace.masterId != first.targetPlace.masterId
          || request.targetPlace.rowId != first.targetPlace.rowId
          || request.targetPlace.x != first.targetPlace.x
          || request.targetPlace.orientation
                 != first.targetPlace.orientation
          || !(request.guardRegion == first.guardRegion)) {
        for (size_t index = 0; index < requests.size(); ++index) {
          results[index].requestId = requests[index].requestId;
          results[index].status = fr::OracleStatus::CheckerError;
          results[index].diagnostics.push_back(fr::makeDiag(
              fr::Severity::Fatal,
              "MixedOracleBatch",
              "one checker batch must share target and guard"));
        }
        return results;
      }
    }

    std::vector<ipl::FillerChanges> changes;
    changes.reserve(requests.size());
    for (const fr::OracleRequest& request : requests) {
      changes.push_back(request.fillerChanges);
    }
    Network* const network = checker_.getNetwork();
    Node* const replaced = network != nullptr
                               ? network->getNode(first.targetPlace.instanceId)
                               : nullptr;
    Master* const replacement = network != nullptr
                                    ? network->getMaster(
                                          first.targetPlace.masterId)
                                    : nullptr;
    if (replaced == nullptr || replaced->getMaster() == nullptr
        || replacement == nullptr) {
      for (size_t index = 0; index < requests.size(); ++index) {
        results[index].requestId = requests[index].requestId;
        results[index].status = fr::OracleStatus::CheckerError;
      }
      return results;
    }
    Node temporary;
    temporary.setId(replaced->getId());
    temporary.setDbInst(replaced->getDbInst());
    temporary.setMaster(replacement);
    temporary.setType(Node::CELL);
    temporary.setWidth(replaced->getWidth());
    temporary.setHeight(replaced->getHeight());
    temporary.setLeft(replaced->getLeft());
    temporary.setBottom(replaced->getBottom());
    temporary.setOrient(toCheckerOrient(first.targetPlace.orientation));
    const eLIB::LibCellID oldMaster = replaced->getMaster()->getDbMaster();
    const ipl::CheckRequest target{
        &temporary,
        GridX(static_cast<ipl::ColId>(first.targetPlace.x
                                      / view_.siteWidth())),
        GridY(first.targetPlace.rowId),
        toCheckerOrient(first.targetPlace.orientation),
        {{OpType::Delete,
          replaced->getDbInst(),
          eUTL::UvDist(replaced->getLeft().v),
          eUTL::UvDist(replaced->getBottom().v),
          oldMaster,
          oldMaster,
          replaced->getOrient()}}};
    const fr::DbCoord yl
        = static_cast<fr::DbCoord>(first.guardRegion.rowLo) * view_.rowHeight();
    const fr::DbCoord yh
        = static_cast<fr::DbCoord>(first.guardRegion.rowHi + 1)
              * view_.rowHeight()
          - 1;
    const ::Rect guard(
        eUTL::UvDist(static_cast<int64_t>(first.guardRegion.x.xl)),
        eUTL::UvDist(static_cast<int64_t>(yl)),
        eUTL::UvDist(static_cast<int64_t>(first.guardRegion.x.xh)),
        eUTL::UvDist(static_cast<int64_t>(yh)));
    const std::vector<ipl::CheckResult> raw
        = checker_.checkPlaceWithOverlays(target, guard, changes);
    if (raw.size() != requests.size()) {
      return std::vector<fr::OracleResult>(raw.size());
    }

    for (size_t index = 0; index < requests.size(); ++index) {
      fr::OracleResult& result = results[index];
      result.requestId = requests[index].requestId;
      const ipl::CheckResult& checkerResult = raw[index];
      for (const ipl::Diagnostic& diagnostic : checkerResult.diagnostics) {
        result.diagnostics.push_back(fr::makeDiag(
            fr::Severity::Warning, diagnostic.status, diagnostic.message));
      }
      for (const ipl::Violation& violation : checkerResult.violations) {
        result.violations.push_back(
            toPlannerViolation(violation, first.targetPlace.instanceId));
      }
      if (result.violations.empty() && !checkerResult.isLegal
          && !result.diagnostics.empty()) {
        result.status = fr::OracleStatus::InvalidOverlay;
      } else {
        result.status = fr::OracleStatus::Checked;
        result.isLegal
            = result.violations.empty() && result.diagnostics.empty();
      }
    }
    return results;
  }

 private:
  fr::Violation toPlannerViolation(const ipl::Violation& violation,
                                   fr::InstanceId targetId) const
  {
    fr::Violation result;
    result.ruleId = violation.ruleId;
    result.kind = violation.ruleSource == ipl::RuleSource::Width
                          || violation.ruleSource
                                 == ipl::RuleSource::Lef58Width
                      ? fr::ViolationKind::MinWidth
                      : fr::ViolationKind::MinSpacing;
    result.relation = violation.relationship == ipl::Relationship::InterRow
                          ? fr::ViolationRelation::InterRow
                          : fr::ViolationRelation::IntraRow;
    result.primaryLayer = violation.primaryLayer;
    result.secondaryLayer = violation.secondaryLayer;
    result.xWindow = {violation.xWindow.xl, violation.xWindow.xh};
    result.measuredValue = violation.measuredValue;
    result.requiredValue = violation.requiredValue;
    result.rowIds.assign(violation.rowIds.begin(), violation.rowIds.end());
    for (const ipl::InstanceId id : violation.instances) {
      fr::ViolationParticipant participant;
      participant.instanceId = id;
      participant.isTarget = id == targetId;
      if (const fr::PlacedInstance* instance = view_.instance(id)) {
        participant.masterId = instance->masterId;
        participant.rowId = instance->rowId;
        participant.isFiller = instance->isFiller;
        const fr::MasterInfo* master = view_.masterInfo(instance->masterId);
        const fr::DbCoord width = master != nullptr ? master->width : 0;
        participant.xRange = {instance->x, instance->x + width};
      }
      result.participants.push_back(participant);
    }
    return result;
  }

  const DumpPlacementView& view_;
  const ipl::ImplantLayerChecker& checker_;
};

struct Proposal
{
  bool legal = false;
  bool repaired = false;
  ipl::FillerChanges changes;
  std::vector<fr::Diagnostic> diagnostics;
};

fr::Region snapshotRegion(const DumpPlacementView& view,
                          const ipl::ImplantLayerChecker& checker,
                          const fr::TargetPlace& target)
{
  const fr::MasterInfo* targetMaster = view.masterInfo(target.masterId);
  const fr::DbCoord targetWidth
      = targetMaster != nullptr ? targetMaster->width : 0;
  const fr::DbCoord targetHeight
      = targetMaster != nullptr ? std::max<fr::DbCoord>(targetMaster->height, 1)
                                : 1;
  fr::DbCoord widestFiller = 0;
  for (const fr::MasterId id : view.fillerMasterIds()) {
    if (const fr::MasterInfo* filler = view.masterInfo(id)) {
      widestFiller = std::max(widestFiller, filler->width);
    }
  }
  const fr::DbCoord checkerReach
      = static_cast<fr::DbCoord>(checker.getMaxRuleValue()) * view.siteWidth();
  const fr::DbCoord halo = std::max(checkerReach, widestFiller);
  const fr::RowId lastRow
      = view.rows().empty() ? 0 : view.rows().back();
  return fr::Region{
      fr::XInterval{target.x - halo, target.x + targetWidth + halo},
      std::max<fr::RowId>(0, target.rowId - 1),
      std::min<fr::RowId>(lastRow,
                          target.rowId
                              + static_cast<fr::RowId>(targetHeight))};
}

Proposal evaluateProposal(const DumpPlacementView& view,
                          DumpCheckerOracle& oracle,
                          const ipl::ImplantLayerChecker& checker,
                          const Node& node,
                          fr::MasterId newMasterId)
{
  Proposal proposal;
  const fr::PlacedInstance* placed = view.instance(node.getId());
  if (placed == nullptr) {
    return proposal;
  }
  const fr::TargetPlace target{placed->id,
                               newMasterId,
                               placed->rowId,
                               placed->x,
                               placed->orientation};
  const fr::OracleResult initial = oracle.checkPlaceWithOverlay(
      fr::OracleRequest{0, target, snapshotRegion(view, checker, target), {}});
  if (initial.status != fr::OracleStatus::Checked) {
    proposal.diagnostics = initial.diagnostics;
    return proposal;
  }
  if (initial.violations.empty()) {
    proposal.legal = true;
    return proposal;
  }
  fr::RepairConfig config;
  config.verbose = fr::debugLoggingDefault();
  fr::internal::RepairPlanner planner(view, oracle, config);
  const fr::FillerRepairResult repair
      = planner.repair(fr::FillerRepairRequest{target, initial.violations});
  proposal.legal = repair.hasSolution;
  proposal.repaired = repair.hasSolution && !repair.changes.empty();
  proposal.changes = repair.changes;
  return proposal;
}

bool validInput(const ipl::ImplantInput& input)
{
  if (input.rowCount <= 0 || input.colCount <= 0 || input.rowHeight <= 0
      || input.siteWidth <= 0 || input.layers.empty()
      || input.masters.empty() || input.placedInsts.empty()) {
    return false;
  }
  for (size_t index = 0; index < input.masters.size(); ++index) {
    if (input.masters[index].masterId != static_cast<ipl::MasterId>(index)) {
      return false;
    }
  }
  std::vector<bool> instanceIds(input.placedInsts.size(), false);
  for (const ipl::PlacedInst& placed : input.placedInsts) {
    if (placed.instanceId < 0
        || static_cast<size_t>(placed.instanceId) >= instanceIds.size()
        || instanceIds[placed.instanceId] || placed.masterId < 0
        || static_cast<size_t>(placed.masterId) >= input.masters.size()
        || placed.rowId < 0 || placed.rowId >= input.rowCount
        || placed.colId < 0 || placed.colId >= input.colCount) {
      return false;
    }
    instanceIds[placed.instanceId] = true;
  }
  return std::all_of(instanceIds.begin(), instanceIds.end(), [](bool present) {
    return present;
  });
}

void printChanges(const ipl::FillerChanges& changes, std::ostream& out)
{
  for (const CellChangeRecord& change : changes) {
    const eUNL::LeafCellID* cellId
        = std::get_if<eUNL::LeafCellID>(&change.cell_data_);
    out << "    filler cell="
        << (cellId != nullptr ? cellId->getIndexValue() : -1) << " master "
        << change.orig_lib_cell_.getIndexValue() << " -> "
        << change.new_lib_cell_.getIndexValue() << '\n';
  }
}

}  // namespace

// [FRPORT] Optional -load entry that reconstructs and runs the migrated engine.
FillerRepairDumpReplayResult replayFillerRepairDump(
    const std::string& filePath,
    const FillerRepairDumpReplayOptions& options,
    std::ostream& out)
{
  FillerRepairDumpReplayResult result;
  const bool targeted = options.instanceId >= 0 || options.masterId >= 0;
  if ((options.instanceId >= 0) != (options.masterId >= 0)) {
    result.error = "dump replay requires both instance and master ids";
    return result;
  }

  const ipl::ImplantInput input
      = ipl::ImplantLayerCheckerHelper::load(filePath);
  if (!validInput(input)) {
    result.error = "could not load a valid checker-helper dump";
    return result;
  }
  if (!input.fillerSetting.present
      || input.fillerSetting.fillerMasterIds.empty()) {
    result.error = "dump has no configured fillerSetting masters";
    return result;
  }

  ipl::ImplantLayerCheckerHelper helper;
  helper.initialize(input);
  ipl::ImplantLayerChecker checker(
      helper.getGrid(), nullptr, helper.getNetwork());
  helper.initChecker(checker);
  if (!checker.getDiags().empty()) {
    result.error = "reconstructed checker has initialization diagnostics";
    return result;
  }
  DumpPlacementView view(input);
  DumpCheckerOracle oracle(view, checker);
  Network* network = helper.getNetwork();
  if (network == nullptr) {
    result.error = "checker helper did not reconstruct Network";
    return result;
  }

  out << "loaded checker dump: rows=" << input.rowCount
      << " cols=" << input.colCount << " masters=" << input.masters.size()
      << " instances=" << input.placedInsts.size()
      << " configuredFillers="
      << input.fillerSetting.fillerMasterIds.size() << '\n';

  if (targeted) {
    const Node* node = network->getNode(options.instanceId);
    const fr::MasterInfo* candidate = view.masterInfo(options.masterId);
    const fr::MasterInfo* current
        = node != nullptr && node->getMaster() != nullptr
              ? view.masterInfo(node->getMaster()->getId())
              : nullptr;
    if (node == nullptr || current == nullptr || candidate == nullptr
        || node->isFiller() || candidate->isFiller
        || current->width != candidate->width
        || current->height != candidate->height) {
      result.error
          = "target ids must select a standard cell and same-size std master";
      return result;
    }
    const Proposal proposal
        = evaluateProposal(view, oracle, checker, *node, options.masterId);
    result.proposals = 1;
    result.cleanRightAway = proposal.legal && !proposal.repaired ? 1 : 0;
    result.repaired = proposal.repaired ? 1 : 0;
    result.unrepairable = proposal.legal ? 0 : 1;
    result.totalFillerSwaps = static_cast<int>(proposal.changes.size());
    out << "target node=" << options.instanceId << " master="
        << current->id << " -> " << options.masterId << ": "
        << (proposal.repaired
                ? "REPAIRED"
                : (proposal.legal ? "LEGAL" : "NO REPAIR"))
        << '\n';
    for (const fr::Diagnostic& diagnostic : proposal.diagnostics) {
      out << "  " << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    printChanges(proposal.changes, out);
    result.completed = true;
    result.passed = true;
    return result;
  }

  out << "--- dump baseline ---\n";
  for (const auto& [nodeId, node] : network->getNodes()) {
    (void) nodeId;
    if (node == nullptr || !node->isStdCell() || node->getMaster() == nullptr) {
      continue;
    }
    ++result.baselineChecked;
    const fr::PlacedInstance* placed = view.instance(node->getId());
    if (placed == nullptr) {
      ++result.baselineIllegal;
      continue;
    }
    const fr::TargetPlace target{placed->id,
                                 placed->masterId,
                                 placed->rowId,
                                 placed->x,
                                 placed->orientation};
    const fr::OracleResult baseline = oracle.checkPlaceWithOverlay(
        fr::OracleRequest{0,
                          target,
                          snapshotRegion(view, checker, target),
                          {}});
    if (baseline.status != fr::OracleStatus::Checked
        || !baseline.violations.empty()) {
      ++result.baselineIllegal;
    }
  }

  int reported = 0;
  out << "--- dump same-size proposals ---\n";
  for (const auto& [nodeId, node] : network->getNodes()) {
    (void) nodeId;
    if (result.proposals >= options.maxProposals) {
      break;
    }
    if (node == nullptr || !node->isStdCell() || node->getMaster() == nullptr) {
      continue;
    }
    const fr::MasterInfo* current
        = view.masterInfo(node->getMaster()->getId());
    if (current == nullptr) {
      continue;
    }
    for (const ipl::MasterItem& master : input.masters) {
      const fr::MasterInfo* candidate = view.masterInfo(master.masterId);
      if (candidate == nullptr || candidate->id == current->id
          || candidate->isFiller || candidate->width != current->width
          || candidate->height != current->height) {
        continue;
      }
      ++result.proposals;
      const Proposal proposal
          = evaluateProposal(view, oracle, checker, *node, candidate->id);
      if (proposal.repaired) {
        ++result.repaired;
        result.totalFillerSwaps += static_cast<int>(proposal.changes.size());
      } else if (proposal.legal) {
        ++result.cleanRightAway;
      } else {
        ++result.unrepairable;
      }
      if (reported++ < options.maxReportedLines) {
        out << "node=" << node->getId() << " master=" << current->id
            << " -> " << candidate->id << ": "
            << (proposal.repaired
                    ? "REPAIRED"
                    : (proposal.legal ? "LEGAL" : "NO REPAIR"))
            << '\n';
        printChanges(proposal.changes, out);
      }
      break;
    }
  }

  out << "baseline checked=" << result.baselineChecked
      << " illegal=" << result.baselineIllegal
      << " proposals=" << result.proposals
      << " clean=" << result.cleanRightAway
      << " repaired=" << result.repaired
      << " unrepairable=" << result.unrepairable
      << " fillerSwaps=" << result.totalFillerSwaps << '\n';
  result.completed = true;
  result.passed = result.baselineIllegal == 0;
  return result;
}

}  // namespace dpl2
