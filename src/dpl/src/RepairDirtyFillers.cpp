// Tcl-command glue for `repair_dirty_fillers`: resolves dirty instance names to
// dbInst*, then runs the shared FillerRepair via DplFillerGrid.
//
// Compiled only inside the OpenROAD build (depends on odb/utl). The SWIG/Tcl
// wiring is in docs/filler_repair_dpl.md.
#include <set>
#include <string>
#include <vector>

#include "DplFillerGrid.h"
#include "odb/db.h"
#include "utl/Logger.h"

namespace dpl_fr {

// Entry the SWIG command calls. `filler_masters` is already resolved (e.g. via
// dpl::get_masters_arg); `dirty_names` are instance names the upstream DRC step
// flagged (passed through from Tcl).
RepairResult repairDirtyFillersByName(
    odb::dbBlock* block,
    const std::vector<odb::dbMaster*>& filler_masters,
    const std::vector<std::string>& dirty_names,
    bool preserve_user_order,
    int min_implant_width,
    utl::Logger* logger)
{
  std::set<odb::dbInst*> dirty;
  for (const std::string& name : dirty_names) {
    odb::dbInst* inst = block->findInst(name.c_str());
    if (inst == nullptr) {
      logger->warn(
          utl::DPL, 203, "repair_dirty_fillers: instance {} not found.", name);
      continue;
    }
    dirty.insert(inst);
  }
  return repairDirtyFillers(block,
                            dirty,
                            filler_masters,
                            preserve_user_order,
                            min_implant_width,
                            logger);
}

}  // namespace dpl_fr
