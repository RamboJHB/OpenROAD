#include <testFillerRepairCmd.hh>

#include <dpl2/DePlace.h>
#include <drc/ImplantLayerChecker.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

// UDM
#include <phys/physDesMgr.hh>
#include <util/iter.hh>

#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace eUNL;

namespace dpl2 {

// ===========================================================================
// >>> ADAPT: command-framework option API <<<
//
// The two functions below are the ONLY code here that depends on how
// uvTCL::CciCommand declares and reads options. Everything else works off
// plain strings. If the framework spells these differently, this block is the
// single place to change -- the command logic does not move.
//
// Expected behaviour:
//   declareOptions()            registers "-inst" and "-master", each taking
//                               one string value, both optional.
//   readOption(name, value)     true and fills `value` when the user supplied
//                               that option; false when they did not.
// ===========================================================================

TestFillerRepairCmd::TestFillerRepairCmd()
    : uvTCL::CciCommand("test_filler_repair",
          "check implant DRC and report the filler swaps the repair engine "
          "proposes; -inst <inst> -master <name> for one specific VT swap",
          false /*echo*/, false /*hidden*/, false /*internal*/)
{
  declareOptions();
}

void TestFillerRepairCmd::declareOptions()
{
  addOption("-inst", "std cell instance name, or the node id the sweep prints");
  addOption("-master", "replacement master cell name (same width and height)");
}

bool TestFillerRepairCmd::readOption(const char* name, std::string& value) const
{
  if (!isOptionSet(name)) {
    return false;
  }
  value = getStringOption(name);
  return true;
}

// ===========================================================================
// Below this line: no command-framework dependency.
// ===========================================================================

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
    if (cell == nullptr || isFillerMaster(*cell)) {
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
// cell -> physical lib cell.
const eLIB::PhysLibCell* findMaster(eUNL::Design* design,
                                    const std::string& masterName)
{
  const eFNL::ModuleID moduleId = design->getLibAcc().findModule(masterName);
  const eLIB::LibCell* libCell = design->getLibAcc().getLibCell(moduleId);
  if (libCell == nullptr) {
    return nullptr;
  }
  return &design->getLibAcc().getPhysLibCell(libCell->getId());
}

// Everything one proposal needs, so the sweep and the targeted path share
// exactly one definition of "swap it in, ask, put it back".
struct ProposalResult
{
  bool legal = false;
  std::vector<FillerCellRecord> changes;
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
  network->updateNode(node, desMgr, candidate);
  result.legal = checker.check(node, grid->gridX(node),
                               grid->gridSnapDownY(node), node->getOrient(),
                               result.changes);
  network->updateNode(node, desMgr, original);
  return result;
}

void printChanges(const std::vector<FillerCellRecord>& changes,
                  const std::function<std::string(LibCellID)>& nameOf,
                  const char* indent)
{
  for (const FillerCellRecord& record : changes) {
    std::cout << indent << "filler cell=" << record.cell_id_.getIndexValue()
              << " at (" << record.origin_x_.getStorage() << ","
              << record.origin_y_.getStorage() << ")  "
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

  std::string instanceOpt;
  std::string masterOpt;
  const bool haveInstance = readOption("-inst", instanceOpt);
  const bool haveMaster = readOption("-master", masterOpt);
  if (haveInstance != haveMaster) {
    std::cout << "ERROR: -inst and -master must be given together "
                 "(omit both to sweep the design)\n";
    return false;
  }
  const bool targeted = haveInstance;

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
  if (!setting || setting->getFillerPhysCells().empty()) {
    std::cout << "ERROR: no filler masters configured -- run "
                 "set_filler_option first\n";
    return false;
  }
  std::cout << "configured filler masters: "
            << setting->getFillerPhysCells().size() << "\n";

  const auto nameOf = [design](LibCellID lcId) -> std::string {
    return design->getLibAcc().getPhysLibCell(lcId).getLibCell().getName();
  };

  // One checker for the whole run, bound to THIS design rather than to
  // whatever Session considers current, and pre-loaded with the repair
  // context so the command does not depend on the DePlace-registered
  // provider having been installed.
  ipl::ImplantLayerChecker checker(grid, network, desMgr);
  checker.setFillerRepairContext(desMgr, setting);
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

  // =====================================================================
  // Targeted mode: one instance, one replacement master.
  // =====================================================================
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

    const eLIB::PhysLibCell* candidate = findMaster(design, masterOpt);
    if (candidate == nullptr) {
      std::cout << "ERROR: no such master: " << masterOpt << "\n";
      return false;
    }
    if (isFillerMaster(*candidate)) {
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

    std::cout << "\n--- Result ---\n";
    if (proposal.legal && proposal.changes.empty()) {
      std::cout << "  LEGAL as-is: the VT change needs no filler repair\n";
    } else if (proposal.legal) {
      std::cout << "  REPAIRED: " << proposal.changes.size()
                << " filler swap(s), checker-verified\n";
      printChanges(proposal.changes, nameOf, "      ");
    } else {
      std::cout << "  ILLEGAL: no filler swap set makes this VT change "
                   "legal\n";
    }
    std::cout << "  set FR_VERBOSE=0 to silence the [fr] decision "
                 "transcript\n";
    std::cout << "\n========================================\n";
    // In targeted mode the user asked one question, and every answer -- legal,
    // repaired, or not repairable -- is a valid one. Only a bad request fails,
    // and those returned above.
    std::cout << "  test_filler_repair DONE\n";
    std::cout << "========================================\n";
    return true;
  }

  // =====================================================================
  // Phase 1 -- baseline. Every movable standard cell at its CURRENT master.
  // A design that is already implant-dirty invalidates phase 2, because the
  // repair gate is a delta against a clean baseline.
  // =====================================================================
  std::cout << "\n--- phase 1: baseline (current masters) ---\n";
  int baselineChecked = 0;
  int baselineIllegal = 0;
  int baselineReported = 0;
  for (auto& node : network->getNodes()) {
    if (!node || node->isFixed() || !node->isStdCell()) {
      continue;
    }
    ++baselineChecked;
    std::vector<FillerCellRecord> fcRecord;
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

  // =====================================================================
  // Phase 2 -- the feature itself. Propose a same-footprint master swap (the
  // VT sibling opto would pick) and see whether the checker rejects it and
  // the repair engine can fix it with filler swaps.
  // =====================================================================
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
          printChanges(proposal.changes, nameOf, "      ");
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

  // ---------------------------------------------------------------------
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

  // A dirty baseline means phase 2 was measured against the wrong reference,
  // so it is a failure even if every proposal happened to come back legal.
  // `no repair found` is a legitimate outcome (some VT changes are simply not
  // fixable by swapping fillers), so it does not fail the command.
  const bool passed = baselineIllegal == 0;
  std::cout << "\n========================================\n";
  std::cout << (passed ? "  test_filler_repair PASSED\n"
                       : "  test_filler_repair FAILED (dirty baseline)\n");
  std::cout << "========================================\n";
  return passed;
}

}  // namespace dpl2
