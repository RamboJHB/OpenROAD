#include <testFillerRepairCmd.hh>

#include <FillerRepairDumpReplay.hh>

#include <dpl2/DePlace.h>
#include <drc/ImplantLayerChecker.h>
#include <fillerRepair/FillerRepairEngine.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

// UDM
#include <phys/physDesMgr.hh>
#include <util/iter.hh>

#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace eUNL;

namespace dpl2 {

// =============================================================================
// Below this line: no command-framework dependency.
// =============================================================================

namespace {

// A real design has millions of cells; every proposal is a full checker call
// plus, on a violation, a repair search. These bound the sweep to something
// interactive. Raise them for a soak run. Targeted mode ignores both.
constexpr int kMaxProposals = 2000;
constexpr int kMaxReportedLines = 50;

// Same footprint means the Grid occupancy does not change when the master is
// swapped, so a proposal needs no unplace/place around it -- which is what
// keeps this command non-destructive.
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
  return Footprint{cell.getWidth().getStorage(),
                   cell.getHeight().getStorage()};
}

// Candidates are drawn ONLY from masters already registered in Network. Two
// reasons: Network::updateNode requires the master to be present, and
// registering new ones here would grow shared state (and, with no
// EdgeTypeTable to hand, grow it undecorated) for a command that is supposed
// to leave the database exactly as it found it.
std::map<Footprint, std::vector<const eLIB::PhysLibCell*>> buildCandidateIndex(
    Network* network)
{
  std::map<Footprint, std::vector<const eLIB::PhysLibCell*>> index;
  for (const auto& master : network->getMasters()) {
    if (!master) {
      continue;
    }
    const eLIB::PhysLibCell* cell = master->getPhysLibCell();
    if (cell == nullptr || master->isFiller()) {
      continue;
    }
    index[footprintOf(*cell)].push_back(cell);
  }
  return index;
}

bool isAllDigits(const std::string& text)
{
  return !text.empty()
      && text.find_first_not_of("0123456789") == std::string::npos;
}

bool parseNonNegativeInt(const std::string& text, int& value)
{
  if (!isAllDigits(text)) {
    return false;
  }
  try {
    const unsigned long long parsed = std::stoull(text);
    if (parsed > static_cast<unsigned long long>(
                     std::numeric_limits<int>::max())) {
      return false;
    }
    value = static_cast<int>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// `-inst` accepts either form: the node id the sweep prints (always available,
// nothing to look up) or the instance name (what a user reading their own
// netlist has to hand).
Node* findNode(Network* network,
               PhysDesMgr* desMgr,
               const std::string& instance)
{
  if (isAllDigits(instance)) {
    return network->getNode(std::stoi(instance));
  }
  for (auto& node : network->getNodes()) {
    if (!node) {
      continue;
    }
    const PhysCell cell = desMgr->getPhysCell(node->getDbInst());
    if (cell.isValid() && cell.getName() == instance) {
      return node.get();
    }
  }
  return nullptr;
}

// Same resolution fillerSetting::addFillerCell uses: name -> module -> lib
// cell -> physical lib cell. Also accepts a numeric master ID (as printed by
// ImplantLayerChecker::printStats) when Network is non-null.
const eLIB::PhysLibCell* findMaster(eUNL::Design* design,
                                    Network* network,
                                    const std::string& masterName)
{
  if (isAllDigits(masterName) && network != nullptr) {
    const int masterId = std::stoi(masterName);
    Master* master = network->getMaster(masterId);
    if (master != nullptr) {
      return master->getPhysLibCell();
    }
    return nullptr;
  }
  const eFNL::ModuleID moduleId = design->getLibAcc().findModule(masterName);
  const eLIB::LibCell* libCell = moduleId.isValid()
      ? design->getLibAcc().getLibCell(moduleId) : nullptr;
  if (libCell == nullptr) {
    return nullptr;
  }
  return &design->getLibAcc().getPhysLibCell(libCell->getId());
}

// Everything one proposal needs, so the sweep and the targeted path share
// exactly one definition of "swap it in, ask, put it back".
struct ProposalResult
{
  bool evaluated = false;
  bool legal = false;
  std::vector<CellChangeRecord> changes;
};

ProposalResult evaluateProposal(const ipl::ImplantLayerChecker& checker,
                                Grid* grid,
                                Network* network,
                                PhysDesMgr* desMgr,
                                Node* node,
                                const eLIB::PhysLibCell& original,
                                const eLIB::PhysLibCell& candidate)
{
  ProposalResult result;
  if (!network->updateNode(node, desMgr, candidate)) {
    return result;
  }
  result.evaluated = true;
  result.legal = checker.check(node, grid->gridX(node),
                               grid->gridSnapDownY(node), node->getOrient(),
                               result.changes);
  // The command is observational: restore the shared Network view even when
  // the proposal is illegal. UDM was never changed.
  if (!network->updateNode(node, desMgr, original)) {
    result.evaluated = false;
    result.legal = false;
    result.changes.clear();
  }
  return result;
}

void printChanges(const std::vector<CellChangeRecord>& changes,
                  const std::function<std::string(LibCellID)>& nameOf,
                  const char* indent)
{
  for (const CellChangeRecord& record : changes) {
    const LeafCellID* leafId = std::get_if<LeafCellID>(&record.cell_data_);
    std::cout << indent << "filler cell="
        << (leafId != nullptr ? leafId->getIndexValue() : -1)
        << " at (" << record.x_.getStorage() << ","
        << record.y_.getStorage() << ") "
        << nameOf(record.orig_lib_cell_) << " -> "
        << nameOf(record.new_lib_cell_) << "\n";
  }
}

}  // namespace

bool TestFillerRepairCmd::exec()
{
  std::cout << "========================================\n";
  std::cout << "  test_filler_repair: implant + repair  \n";
  std::cout << "========================================\n";

  const std::string instanceOpt = instOpt_.getValue();
  const std::string masterOpt = masterOpt_.getValue();
  const std::string loadOpt = loadOpt_.getValue();
  const bool haveInstance = !instanceOpt.empty();
  const bool haveMaster = !masterOpt.empty();
  if (haveInstance != haveMaster) {
    std::cout << "ERROR: -inst and -master must be given together "
                 "(omit both to sweep the design)\n";
    return false;
  }
  const bool targeted = haveInstance;

  // A helper dump is self-contained: rebuild its Grid/Network/checker and
  // run the pure planner before touching Session, DePlace, or UDM.
  if (!loadOpt.empty()) {
    FillerRepairDumpReplayOptions options;
    options.maxProposals = kMaxProposals;
    options.maxReportedLines = kMaxReportedLines;
    if (targeted
        && (!parseNonNegativeInt(instanceOpt, options.instanceId)
            || !parseNonNegativeInt(masterOpt, options.masterId))) {
      std::cout << "ERROR: with -load, -inst and -master must be numeric "
                   "node/master ids stored in the dump\n";
      return false;
    }
    const FillerRepairDumpReplayResult replay
        = replayFillerRepairDump(loadOpt, options, std::cout);
    if (!replay.completed) {
      std::cout << "ERROR: " << replay.error << "\n";
      return false;
    }
    std::cout << "\n========================================\n";
    std::cout << (replay.passed
                      ? "  test_filler_repair PASSED\n"
                      : "  test_filler_repair FAILED (dirty baseline)\n");
    std::cout << "========================================\n";
    return replay.passed;
  }

  eUNL::Session& sess = eUNL::Session::getSession();
  eUNL::Design* design = sess.getCurrentDesign();
  if (!design) {
    std::cout << "ERROR: no design loaded\n";
    return false;
  }

  DePlace* de_place = DePlace::get();
  PhysDesMgr* desMgr = de_place->getDesMgr();
  if (!desMgr) {
    std::cout << "ERROR: DePlace not initialized (no desMgr)\n";
    return false;
  }
  Grid* grid = de_place->getGrid();
  Network* network = de_place->getNetwork();
  if (!grid || !network || grid->getPixelYSize() == 0) {
    std::cout << "ERROR: empty placement grid\n";
    return false;
  }
  std::cout << "grid: " << grid->getRowCount() << " x "
            << grid->getRowSiteCount() << "\n";

  if (!grid->isFullUtil()) {
    std::cout << "ERROR: placement grid is not fully utilized; filler repair "
                 "assumes every legal site is occupied\n";
    return false;
  }

  fillerSetting* setting = de_place->getFillerSetting();

  // set all filler as candidate
  //setting->addAllFillerCells();

  if (!setting || setting->getFillerPhysCells().empty()) {
    std::cout << "ERROR: no filler masters configured -- run "
                 "set_filler_option first\n";
    return false;
  }
  std::cout << "configured filler masters: "
            << setting->getFillerPhysCells().size() << "\n";
  if (!de_place->registerFillerRepairMasters()) {
    std::cout << "ERROR: could not register configured filler masters in "
                 "Network with the DePlace edge table\n";
    return false;
  }

  const auto nameOf = [design](LibCellID lcId) -> std::string {
    return design->getLibAcc().getPhysLibCell(lcId).getLibCell().getName();
  };

  // The command owns both objects. The checker only borrows the initialized
  // engine; the engine borrows this checker as its DRC oracle.
  ipl::ImplantLayerChecker checker(grid, network);
  if (!checker.getDiags().empty()) {
    std::cout << "checker init diagnostics: " << checker.getDiags().size()
              << "\n";
    int shown = 0;
    for (const ipl::Diagnostic& diagnostic : checker.getDiags()) {
      if (shown++ >= kMaxReportedLines) {
        std::cout << "  ... (more suppressed)\n";
        break;
      }
      std::cout << "  " << diagnostic.status << ": " << diagnostic.message
                << "\n";
    }
  }
  fillerRepair::FillerRepairEngine repairEngine(grid, network);
  if (!repairEngine.init(checker)) {
    std::cout << "ERROR: filler repair engine initialization failed\n";
    return false;
  }
  checker.setFillerRepairEngine(&repairEngine);

  // =============================================================================
  // Targeted mode: one instance, one replacement master.
  // =============================================================================
  if (targeted) {
    std::cout << "\n--- targeted: " << instanceOpt << " -> " << masterOpt
              << " ---\n";

    Node* node = findNode(network, desMgr, instanceOpt);
    if (node == nullptr) {
      std::cout << "ERROR: no such instance: " << instanceOpt << "\n";
      return false;
    }
    if (!node->isStdCell()) {
      std::cout << "ERROR: instance is not a standard cell\n";
      return false;
    }
    if (node->isFixed()) {
      std::cout << "ERROR: instance is fixed\n";
      return false;
    }
    Master* master = node->getMaster();
    const eLIB::PhysLibCell* original =
        master != nullptr ? master->getPhysLibCell() : nullptr;
    if (original == nullptr) {
      std::cout << "ERROR: instance has no physical master\n";
      return false;
    }

    const eLIB::PhysLibCell* candidate = findMaster(design, network, masterOpt);
    if (candidate == nullptr) {
      std::cout << "ERROR: no such master: " << masterOpt << "\n";
      return false;
    }
    if (setting->isFillerCell(candidate->getLibCellId())) {
      std::cout << "ERROR: replacement master is a filler; the target of a "
                   "repair is a standard cell\n";
      return false;
    }
    if (!(footprintOf(*candidate) == footprintOf(*original))) {
      std::cout << "ERROR: replacement changes the footprint ("
                << original->getWidth().getStorage() << "x"
                << original->getHeight().getStorage() << " -> "
                << candidate->getWidth().getStorage() << "x"
                << candidate->getHeight().getStorage()
                << "); repair supports same-size swaps only\n";
      return false;
    }
    // updateNode resolves the master through Network, so it has to be there
    // already. Registering it here would leave an undecorated master behind
    // in shared state.
    if (network->getMaster(candidate->getLibCellId()) == nullptr) {
      std::cout << "ERROR: master " << masterOpt << " is not registered in "
                   "Network (no placed instance uses it)\n";
      return false;
    }

    std::cout << "  node=" << node->getId() << "  pos=(" << node->getLeft().v
              << "," << node->getBottom().v << ")  row="
              << grid->gridSnapDownY(node).v << "\n";
    std::cout << "  master: " << nameOf(original->getLibCellId()) << " -> "
              << nameOf(candidate->getLibCellId()) << "\n";

    const ProposalResult proposal = evaluateProposal(
        checker, grid, network, desMgr, node, *original, *candidate);
    if (!proposal.evaluated) {
      std::cout << "ERROR: could not apply and restore the Network proposal\n";
      return false;
    }

    std::cout << "\n--- Result ---\n";
    if (proposal.legal && proposal.changes.empty()) {
      std::cout << "  LEGAL as-is: the VT change needs no filler repair\n";
    } else if (proposal.legal) {
      std::cout << "  REPAIRED: " << proposal.changes.size()
                << " filler swap(s), checker-verified\n";
      printChanges(proposal.changes, nameOf, "    ");
    } else {
      std::cout << "  ILLEGAL: no filler swap set makes this VT change "
                   "legal\n";
    }
    std::cout << "  set FR_VERBOSE=0 to silence the [fr] decision "
                 "transcript\n";
    std::cout << "\n========================================\n";
    std::cout << "  test_filler_repair DONE\n";
    std::cout << "========================================\n";
    return true;
  }

  // =============================================================================
  // Phase 1 -- baseline. Every movable standard cell at its CURRENT master.
  // =============================================================================
  std::cout << "\n--- phase 1: baseline (current masters) ---\n";
  int baselineChecked = 0;
  int baselineIllegal = 0;
  int baselineReported = 0;
  for (auto& node : network->getNodes()) {
    if (!node || node->isFixed() || !node->isStdCell()) {
      continue;
    }
    ++baselineChecked;
    std::vector<CellChangeRecord> fcRecord;
    const bool legal = checker.check(node.get(), grid->gridX(node.get()),
                                     grid->gridSnapDownY(node.get()),
                                     node->getOrient(), fcRecord);
    if (legal && fcRecord.empty()) {
      continue;
    }
    ++baselineIllegal;
    if (baselineReported++ < kMaxReportedLines) {
      std::cout << "  dirty as placed: node=" << node->getId() << " master="
                << nameOf(node->getMaster()->getDbMaster()) << " pos=("
                << node->getLeft().v << "," << node->getBottom().v << ")"
                << (legal ? "  (repairable)" : "  (no repair found)") << "\n";
    }
  }
  std::cout << "  checked: " << baselineChecked
            << "  not clean: " << baselineIllegal << "\n";

  // =============================================================================
  // Phase 2 -- propose same-footprint master swaps.
  // =============================================================================
  std::cout << "\n--- phase 2: proposed VT swaps (max " << kMaxProposals
            << ") ---\n";
  const auto candidates = buildCandidateIndex(network);

  int proposals = 0;
  int cleanRightAway = 0;
  int repaired = 0;
  int unrepairable = 0;
  int totalSwaps = 0;
  int reported = 0;
  bool truncated = false;

  for (auto& node : network->getNodes()) {
    if (proposals >= kMaxProposals) {
      truncated = true;
      break;
    }
    if (!node || node->isFixed() || !node->isStdCell()) {
      continue;
    }
    Master* master = node->getMaster();
    const eLIB::PhysLibCell* original =
        master != nullptr ? master->getPhysLibCell() : nullptr;
    if (original == nullptr) {
      continue;
    }
    const auto slot = candidates.find(footprintOf(*original));
    if (slot == candidates.end()) {
      continue;
    }
    const LibCellID originalId = original->getLibCellId();
    for (const eLIB::PhysLibCell* candidate : slot->second) {
      if (candidate->getLibCellId() == originalId) {
        continue;
      }
      ++proposals;
      const ProposalResult proposal = evaluateProposal(
          checker, grid, network, desMgr, node.get(), *original, *candidate);
      if (!proposal.evaluated) {
        std::cout << "ERROR: could not apply and restore proposal for node="
                  << node->getId() << "\n";
        return false;
      }

      if (proposal.legal && proposal.changes.empty()) {
        ++cleanRightAway;
      } else if (proposal.legal) {
        ++repaired;
        totalSwaps += static_cast<int>(proposal.changes.size());
        if (reported++ < kMaxReportedLines) {
          std::cout << "  repairable: node=" << node->getId() << "  "
                    << nameOf(originalId) << " -> "
                    << nameOf(candidate->getLibCellId())
                    << "  swaps=" << proposal.changes.size() << "\n";
          printChanges(proposal.changes, nameOf, "    ");
        }
      } else {
        ++unrepairable;
        if (reported++ < kMaxReportedLines) {
          std::cout << "  NO repair: node=" << node->getId() << "  "
                    << nameOf(originalId) << " -> "
                    << nameOf(candidate->getLibCellId()) << "\n";
        }
      }
      break;  // one proposal per cell keeps the sweep linear
    }
  }

  // ----------------------------------------------------------------------------
  std::cout << "\n--- Result ---\n";
  std::cout << "  baseline cells checked : " << baselineChecked << "\n";
  std::cout << "  baseline not clean     : " << baselineIllegal << "\n";
  std::cout << "  VT proposals evaluated : " << proposals
            << (truncated ? "  (stopped at the cap)" : "") << "\n";
  std::cout << "    legal without repair : " << cleanRightAway << "\n";
  std::cout << "    repaired by fillers  : " << repaired << "  ("
            << totalSwaps << " filler swaps proposed)\n";
  std::cout << "    no repair found      : " << unrepairable << "\n";
  std::cout << "  re-run with -inst <node id> -master <name> to drill into "
               "one case\n";
  std::cout << "  set FR_VERBOSE=0 to silence the [fr] decision transcript\n";

  const bool passed = baselineIllegal == 0;
  std::cout << "\n========================================\n";
  std::cout << (passed ? "  test_filler_repair PASSED\n"
                       : "  test_filler_repair FAILED (dirty baseline)\n");
  std::cout << "========================================\n";
  return passed;
}

}  // namespace dpl2
