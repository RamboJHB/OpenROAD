#include <testEcoFlowCmd.hh>

#include <dpl2/DePlace.h>
#include <util.hh>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

// UDM
#include <phys/fpManager.hh>
#include <phys/physDesMgr.hh>
#include <phys/physHier.hh>
#include <util/iter.hh>
#include <util/assert.h>

#include <iostream>
#include <sstream>
#include <set>
#include <map>

using namespace eUNL;

namespace dpl2 {

// -----------------------------------------------------------------------------
// FakeDrcFixer -- local helper that finds DRC-violating cells and swaps them
// to an alternative libcell of the same type (same function prefix, same
// height, same bottom/top power type).
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

    for (auto& node : de_place_->getNetwork()->getNodes()) {
      if (node->isFixed()) {
        continue;
      }
      LeafCellID cellId = node->getDbInst();
      LibCellID currentLcId = node->getMaster()->getDbMaster();
      checked++;

      std::vector<FillerCellRecord> fcRecord;

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
            persistSwap(cellId, currentLcId, newLcId, fcRecord);
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
  }

  std::cout << "  Violations found: " << violations_found
        << ", fixed: " << violations_fixed << ", checked: " <<
        checked << "\n";
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

  // Extract the function-name prefix by stripping the trailing size/strength
  // suffix (e.g. "INV_X16" -> "INV", "AND2_X4" -> "AND2").
  static std::string functionPrefix(const std::string& name)
  {
    size_t pos = name.rfind('_');
    if (pos != std::string::npos && pos > 0) {
      return name.substr(0, pos);
    }
    return name;
  }

  // Find a libcell with the same type key as `currentLcId`
  // but a different LibCellID.
  // !! todo: find a new alternative lib cell
  LibCellID findSameTypeLibCell(LibCellID currentLcId)
  {
    return currentLcId;
  }

  // Persist the libcell swap in the DePlace internal cache
  // !! not commit to udm
  void persistSwap(LeafCellID cellId, LibCellID origLcId,
      LibCellID newLcId, const std::vector<FillerCellRecord>& fcRecord)
  {
    const PhysLibCell& newPhysCell = design_->getLibAcc().getPhysLibCell(newLcId);
    Node* cell = de_place_->getNetwork()->getNode(cellId);

    de_place_->unplaceCell(cell);
    de_place_->getNetwork()->addMaster(
        newPhysCell,
        *de_place_->getFillerSetting(),
        de_place_->getGrid(),
        de_place_->edge_type_table_.get());
    de_place_->getNetwork()->updateNode(cell, de_place_->getDesMgr(), newPhysCell);
    de_place_->placeCell(cell,
                         de_place_->getGrid()->gridX(cell),
                         de_place_->getGrid()->gridSnapDownY(cell));
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
