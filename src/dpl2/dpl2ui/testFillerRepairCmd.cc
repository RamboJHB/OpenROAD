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

#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace eUNL;

namespace dpl2 {

namespace {

// A real design has millions of cells; every proposal below is a full checker
// call plus, on a violation, a repair search. These bound one invocation to
// something interactive. Raise them for a soak run.
constexpr int kMaxProposals = 2000;
constexpr int kMaxReportedLines = 50;

// Same footprint means the Grid occupancy does not change when the master is
// swapped, so the proposal needs no unplace/place around it -- which is what
// keeps this command non-destructive.
struct Footprint
{
  int64_t width = 0;
  int64_t height = 0;

  bool operator<(const Footprint& other) const
  {
    return width != other.width ? width < other.width : height < other.height;
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

}  // namespace

bool TestFillerRepairCmd::exec()
{
  std::cout << "========================================\n";
  std::cout << "  test_filler_repair: implant + repair  \n";
  std::cout << "========================================\n";

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

  // ---------------------------------------------------------------------
  // Phase 1 -- baseline. Every movable standard cell at its CURRENT master.
  // A design that is already implant-dirty invalidates phase 2, because the
  // repair gate is a delta against a clean baseline.
  // ---------------------------------------------------------------------
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

  // ---------------------------------------------------------------------
  // Phase 2 -- the feature itself. Propose a same-footprint master swap (the
  // VT sibling opto would pick) and see whether the checker rejects it and
  // the repair engine can fix it with filler swaps.
  // ---------------------------------------------------------------------
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

      // Swap in the proposal, ask, put it back. Same footprint, so Grid
      // occupancy is untouched and no unplace/place is needed.
      network->updateNode(node.get(), desMgr, *candidate);
      std::vector<FillerCellRecord> fcRecord;
      const bool legal = checker.check(node.get(), grid->gridX(node.get()),
                                       grid->gridSnapDownY(node.get()),
                                       node->getOrient(), fcRecord);
      network->updateNode(node.get(), desMgr, *original);

      if (legal && fcRecord.empty()) {
        ++cleanRightAway;
      } else if (legal) {
        ++repaired;
        totalSwaps += static_cast<int>(fcRecord.size());
        if (reported++ < kMaxReportedLines) {
          std::cout << "  repairable: node=" << node->getId() << "  "
                    << nameOf(originalId) << " -> "
                    << nameOf(candidate->getLibCellId())
                    << "  swaps=" << fcRecord.size() << "\n";
          for (const FillerCellRecord& record : fcRecord) {
            std::cout << "      filler cell="
                      << record.cell_id_.getIndexValue() << " at ("
                      << record.origin_x_.getStorage() << ","
                      << record.origin_y_.getStorage() << ")  "
                      << nameOf(record.orig_lib_cell_) << " -> "
                      << nameOf(record.new_lib_cell_) << "\n";
          }
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
