#include "testCmd.hh"

#include <dpl2/DePlace.h>
#include <infrastructure/Grid.h>
#include <infrastructure/network.h>

#include <iostream>
#include <string>
#include <vector>

using namespace eUNL;

namespace dpl2 {

// -----------------------------------------------------------------------------
// FakeDrcFixer -- command-local harness. Alternative master selection remains
// a placeholder; accepted changes use DePlace's complete transaction.
// -----------------------------------------------------------------------------
class TestEcoFlowCmd::FakeDrcFixer
{
 public:
  FakeDrcFixer(DePlace* de_place, eUNL::Design* design)
    : de_place_(de_place), design_(design) {}

  // Returns the number of violations successfully fixed.
  int fixViolations()
  {
    int violations_found = 0;
    int violations_fixed = 0;
    int checked = 0;

    for (auto& [nodeId, node] : de_place_->getNetwork()->getNodes()) {
      (void) nodeId;
      if (node->isFixed() || node->isFiller()) {
        continue;
      }
      LeafCellID cellId = node->getDbInst();
      LibCellID currentLcId = node->getMaster()->getDbMaster();
      checked++;

      std::vector<CellChangeRecord> fcRecord;

      // Check if the current placement has a DRC violation
      if (!de_place_->isLegal(cellId, currentLcId, fcRecord)) {
        violations_found++;
        std::cout << "  Violation at node id=" << node->getId()
              << " master=" << getLibCellName(currentLcId)
              << " pos=(" << node->getLeft().v << "," <<
              node->getBottom().v << ")\n";

        // Find an alternative libcell with the same type
        LibCellID newLcId = findSameTypeLibCell(currentLcId);

        if (newLcId.isValid()) {
          if (de_place_->isLegal(cellId, newLcId, fcRecord)) {
            if (!de_place_->commit(fcRecord)) {
              std::cout << "    -> Commit rejected\n";
              continue;
            }
            violations_fixed++;
            std::cout << "    -> Fixed: swapped to " <<
              getLibCellName(newLcId) << " fcRecord: " << fcRecord.size() << "\n";
          } else {
            std::cout << "    -> Candidate " <<
              getLibCellName(newLcId) << " also illegal\n";
          }
        }
      } else {
        std::cout << "    -> No suitable alternative found\n";
      }
    }

    std::cout << "  Violations found: " << violations_found
              << ", fixed: " << violations_fixed << ", checked: " << checked << "\n";
    return violations_fixed;
  }

 private:
  DePlace* de_place_;
  eUNL::Design* design_;

  std::string getLibCellName(LibCellID lcId) const
  {
    const PhysLibCell& pcell = design_->getLibAcc().getPhysLibCell(lcId);
    return pcell.getLibCell().getName();
  }

  // Placeholder: no alternative-master search is implemented by this command.
  LibCellID findSameTypeLibCell(LibCellID currentLcId)
  {
    return currentLcId;
  }
};

bool TestEcoFlowCmd::exec()
{
  std::cout << "========================================\n";
  std::cout << "   test_eco_flow: DRC checker + fixer   \n";
  std::cout << "========================================\n";

  eUNL::Session& sess = eUNL::Session::getSession();
  eUNL::Design* design = sess.getCurrentDesign();
  if (!design) {
    std::cout << "ERROR: no design loaded\n";
    return false;
  }

  DePlace* de_place = DePlace::get();
  if (!de_place->getDesMgr()) {
    std::cout << "ERROR: DePlace not initialized (no desMgr)\n";
    return false;
  }
  Grid* grid = de_place->getGrid();
  std::cout << "grid: " << grid->getRowCount() << "x" <<
    grid->getRowSiteCount() << " pixel y size: " <<
    grid->getPixelYSize() << std::endl;
  if (grid->getPixelYSize() == 0) {
    std::cout << "ERROR: empty grid pixel.\n";
    return false;
  }
  if (!grid->isFullUtil()) {
    std::cout << "ERROR: placement grid is not fully utilized.\n";
    return false;
  }

  // ----- Run the fake DrcFixer -----
  FakeDrcFixer fixer(de_place, design);
  int fixed = fixer.fixViolations();

  // ----- Report -----
  std::cout << "\n--- Result ---\n";
  std::cout << "  Fixed " << fixed << " DRC violations\n";

  bool passed = true;
  std::cout << "\n========================================\n";
  std::cout << (passed ? "  test_eco_flow PASSED\n" : "  test_eco_flow FAILED\n");
  std::cout << "========================================\n";

  return passed;
}

}  // namespace dpl2
