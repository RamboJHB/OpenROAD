// FillerGridAdapter.example.h  --  PORTING TEMPLATE (not compiled / not built).
//
// Copy this file, rename it, and fill in the TODOs to bind FillerRepair to your
// database.  The FillerRepair algorithm is unchanged; you only implement the
// FillerGrid interface and provide the filler library.
//
// Two things the adapter owns that the core does not model:
//   1. Geometry: (row, col) site coordinates <-> physical DBU, row pitch, row
//      Y, and per-row orientation (R0/MX) so power rails/implant line up.
//   2. Masters: a map (vt, width_sites, height_rows) -> your filler master,
//      and the reverse (master -> {vt, width, height}) to build the library.
//
// See docs/filler_repair_porting.md for the full step-by-step.

#if 0  // <-- remove once you have wired this to a real DB

#include <string>
#include <vector>

#include "FillerRepair.h"

namespace your_db {

using namespace dpl_fr;

// ---------------------------------------------------------------------------
// 1) Implement the grid against your database.
// ---------------------------------------------------------------------------
class MyDbFillerGrid : public FillerGrid
{
 public:
  explicit MyDbFillerGrid(/* MyDb* db, region, ... */) { /* TODO cache rows */ }

  int numRows() const override
  {
    return 0;  // TODO: number of placement rows in the region
  }
  int numCols(int row) const override
  {
    return 0;  // TODO: number of sites in `row`
  }
  SiteKind kindAt(int row, int col) const override
  {
    // TODO: classify the instance occupying site (row,col):
    //   - a placed std cell                  -> SiteKind::Cell
    //   - a filler NOT marked dirty          -> SiteKind::CleanFiller
    //   - a filler marked dirty (by upstream)-> SiteKind::DirtyFiller
    //   - blockage / macro / fixed keep-out  -> SiteKind::Blocked
    //   - free site                          -> SiteKind::Empty
    return SiteKind::Empty;
  }
  Vt vtAt(int row, int col) const override
  {
    // TODO: VT / implant id of the master at (row,col); "" if none.
    // Reuse your implant-layer lookup (OpenROAD dpl: getImplant(master)).
    return VT_NONE;
  }

  void clearSite(int row, int col) override
  {
    // TODO: delete the dirty filler instance covering (row,col).  Deleting it
    // once for any of its sites is fine; guard against double-delete.
  }
  void placeFiller(const PlacedFiller& f) override
  {
    // TODO: create ONE filler instance:
    //   master = masterFor(f.vt, f.width, f.height);   // your reverse map
    //   x = regionX + f.col * siteWidth;
    //   y = rowY(f.row);
    //   inst = makeInstance(master, x, y, orientOfRow(f.row), physical_only);
  }
};

// ---------------------------------------------------------------------------
// 2) Build the filler library from your masters (preserve user order!).
// ---------------------------------------------------------------------------
inline std::vector<Filler> buildLibrary(/* user filler master list */)
{
  std::vector<Filler> lib;
  // for (master : user_core_filler_list_in_user_order) {
  //   lib.push_back(Filler{ widthInSites(master),
  //                         heightInRows(master),
  //                         vtOf(master),
  //                         master->name() });
  // }
  return lib;  // order matters when preserve_user_order == true
}

// ---------------------------------------------------------------------------
// 3) Drive the repair.
// ---------------------------------------------------------------------------
inline RepairResult runRepair(/* MyDb* db, region, ... */)
{
  std::vector<Filler> lib = buildLibrary(/* ... */);
  const bool preserve_user_order = true;   // from setFillerMode -preserveUserOrder
  Rules rules;
  rules.min_implant_width = 1;             // TODO: from your implant rule deck

  MyDbFillerGrid grid(/* db, region */);
  FillerRepair repair(lib, preserve_user_order, rules);
  RepairResult result = repair.repair(grid);  // mutates the DB via the grid

  // result.placed   -> the new fillers (already created via placeFiller)
  // result.unsolved -> windows filler could not fix; hand back to upstream
  return result;
}

}  // namespace your_db

#endif
