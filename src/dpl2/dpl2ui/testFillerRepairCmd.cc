#include <testFillerRepairCmd.hh>

#include <FillerRepairDumpReplay.hh>
#include <dpl2/DePlace.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <phys/physDesMgr.hh>

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace dpl2 {
namespace {

constexpr int kMaxProposals = 2000;
constexpr int kMaxReportedLines = 50;

struct Footprint
{
  int64_t width = 0;
  int64_t height = 0;

  bool operator<(const Footprint& other) const
  {
    return width != other.width ? width < other.width : height < other.height;
  }

  bool operator==(const Footprint& other) const
  {
    return width == other.width && height == other.height;
  }
};

Footprint footprintOf(const eLIB::PhysLibCell& cell)
{
  return {cell.getWidth().getStorage(), cell.getHeight().getStorage()};
}

bool parseNonNegativeInt(const std::string& text, int& value)
{
  if (text.empty()
      || text.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  try {
    const unsigned long long parsed = std::stoull(text);
    if (parsed
        > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
      return false;
    }
    value = static_cast<int>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

Node* findNode(Network& network,
               eUNL::PhysDesMgr& desMgr,
               const std::string& nameOrId)
{
  int nodeId = -1;
  if (parseNonNegativeInt(nameOrId, nodeId)) {
    return network.getNode(nodeId);
  }
  for (const auto& [id, owned] : network.getNodes()) {
    (void) id;
    if (owned == nullptr) {
      continue;
    }
    const eUNL::PhysCell physical = desMgr.getPhysCell(owned->getDbInst());
    if (physical.isValid() && physical.getName() == nameOrId) {
      return owned.get();
    }
  }
  return nullptr;
}

const eLIB::PhysLibCell* findMaster(eUNL::Design& design,
                                    Network& network,
                                    const std::string& nameOrId)
{
  int masterId = -1;
  if (parseNonNegativeInt(nameOrId, masterId)) {
    const Master* master = network.getMaster(masterId);
    return master != nullptr ? master->getPhysLibCell() : nullptr;
  }
  const eFNL::ModuleID moduleId = design.getLibAcc().findModule(nameOrId);
  const eLIB::LibCell* libCell
      = moduleId.isValid() ? design.getLibAcc().getLibCell(moduleId) : nullptr;
  return libCell != nullptr
             ? &design.getLibAcc().getPhysLibCell(libCell->getId())
             : nullptr;
}

using CandidateIndex
    = std::map<Footprint, std::vector<const eLIB::PhysLibCell*>>;

CandidateIndex buildCandidateIndex(const Network& network)
{
  CandidateIndex result;
  for (const auto& [id, master] : network.getMasters()) {
    (void) id;
    if (master == nullptr || master->isFiller()
        || master->getPhysLibCell() == nullptr) {
      continue;
    }
    result[footprintOf(*master->getPhysLibCell())].push_back(
        master->getPhysLibCell());
  }
  return result;
}

bool validFillerChanges(const std::vector<CellChangeRecord>& changes,
                        const fillerSetting& setting)
{
  return std::all_of(
      changes.begin(), changes.end(), [&setting](const CellChangeRecord& change) {
        return change.op_ == OpType::Replace
               && std::holds_alternative<eUNL::LeafCellID>(change.cell_data_)
               && setting.isFillerCell(change.orig_lib_cell_)
               && setting.isFillerCell(change.new_lib_cell_);
      });
}

void printChanges(const std::vector<CellChangeRecord>& changes)
{
  for (const CellChangeRecord& change : changes) {
    const auto* id = std::get_if<eUNL::LeafCellID>(&change.cell_data_);
    std::cout << "    filler="
              << (id != nullptr ? id->getIndexValue() : -1) << " master="
              << change.orig_lib_cell_.getIndexValue() << " -> "
              << change.new_lib_cell_.getIndexValue() << '\n';
  }
}

bool runDump(const std::string& path,
             const std::string& instance,
             const std::string& master)
{
  FillerRepairDumpReplayOptions options;
  options.maxProposals = kMaxProposals;
  options.maxReportedLines = kMaxReportedLines;
  if (!instance.empty()
      && (!parseNonNegativeInt(instance, options.instanceId)
          || !parseNonNegativeInt(master, options.masterId))) {
    std::cout << "ERROR: -load requires numeric -inst/-master ids\n";
    return false;
  }
  const FillerRepairDumpReplayResult result
      = replayFillerRepairDump(path, options, std::cout);
  if (!result.completed) {
    std::cout << "ERROR: " << result.error << '\n';
    return false;
  }
  return result.passed;
}

}  // namespace

bool TestFillerRepairCmd::exec()
{
  const std::string instance = instOpt_.getValue();
  const std::string masterName = masterOpt_.getValue();
  const std::string dump = loadOpt_.getValue();
  if (instance.empty() != masterName.empty()) {
    std::cout << "ERROR: -inst and -master must be supplied together\n";
    return false;
  }
  if (!dump.empty()) {
    return runDump(dump, instance, masterName);
  }

  eUNL::Design* design = eUNL::Session::getSession().getCurrentDesign();
  DePlace* dePlace = DePlace::get();
  eUNL::PhysDesMgr* desMgr = dePlace->getDesMgr();
  Network* network = dePlace->getNetwork();
  Grid* grid = dePlace->getGrid();
  fillerSetting* setting = dePlace->getFillerSetting();
  if (design == nullptr || desMgr == nullptr || network == nullptr
      || grid == nullptr || setting == nullptr || grid->getPixelYSize() == 0) {
    std::cout << "ERROR: DePlace design/infrastructure is not initialized\n";
    return false;
  }
  if (setting->getFillerPhysCells().empty()) {
    std::cout << "ERROR: no configured filler masters\n";
    return false;
  }

  if (!instance.empty()) {
    Node* node = findNode(*network, *desMgr, instance);
    const eLIB::PhysLibCell* candidate
        = findMaster(*design, *network, masterName);
    if (node == nullptr || node->getMaster() == nullptr || !node->isStdCell()
        || candidate == nullptr || setting->isFillerCell(candidate->getLibCellId())
        || !(footprintOf(*node->getMaster()->getPhysLibCell())
             == footprintOf(*candidate))) {
      std::cout << "ERROR: target must be a standard cell and the replacement "
                   "must be a same-footprint standard-cell master\n";
      return false;
    }
    std::vector<CellChangeRecord> changes;
    const bool legal
        = dePlace->isLegal(node->getDbInst(), candidate->getLibCellId(), changes);
    if (!validFillerChanges(changes, *setting)) {
      std::cout << "ERROR: repair returned a non-Replace/non-filler record\n";
      return false;
    }
    std::cout << (legal ? "LEGAL" : "NO REPAIR") << ": node=" << node->getId()
              << " fillerChanges=" << changes.size() << '\n';
    printChanges(changes);
    return legal;
  }

  const CandidateIndex candidates = buildCandidateIndex(*network);
  int proposals = 0;
  int legal = 0;
  int repaired = 0;
  int invalid = 0;
  for (const auto& [id, node] : network->getNodes()) {
    (void) id;
    if (node == nullptr || node->getMaster() == nullptr || !node->isStdCell()
        || node->isFixed() || node->getMaster()->getPhysLibCell() == nullptr) {
      continue;
    }
    const auto found
        = candidates.find(footprintOf(*node->getMaster()->getPhysLibCell()));
    if (found == candidates.end()) {
      continue;
    }
    for (const eLIB::PhysLibCell* candidate : found->second) {
      if (proposals >= kMaxProposals) {
        break;
      }
      if (candidate == nullptr
          || candidate->getLibCellId() == node->getMaster()->getDbMaster()) {
        continue;
      }
      ++proposals;
      std::vector<CellChangeRecord> changes;
      const bool accepted = dePlace->isLegal(
          node->getDbInst(), candidate->getLibCellId(), changes);
      if (!validFillerChanges(changes, *setting)) {
        ++invalid;
      } else if (accepted) {
        ++legal;
        repaired += !changes.empty();
      }
    }
    if (proposals >= kMaxProposals) {
      break;
    }
  }

  std::cout << "proposals=" << proposals << " legal=" << legal
            << " repaired=" << repaired << " invalidResults=" << invalid
            << '\n';
  return proposals > 0 && invalid == 0;
}

}  // namespace dpl2
