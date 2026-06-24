// DplFillerGrid: OpenROAD (odb/dpl) implementation of the portable FillerGrid.
//
// This is the dpl-side adapter that binds the shared FillerRepair algorithm
// (src/dpl/src/FillerRepair.{h,cpp}) to OpenROAD's database.  The algorithm is
// unchanged; only this adapter and the master/geometry mapping are
// dpl-specific.
//
// NOTE: depends on odb headers and is compiled only inside the OpenROAD build
// (it is NOT part of the stand-alone unit test).  Coordinates exposed to the
// algorithm are in sites/rows; this adapter owns the DBU<->site geometry and
// the (vt,width,height) -> dbMaster mapping.
//
// Model: a region of uniform-pitch rows.  Row index = (rowY - coreYmin)/rowH,
// column = (x - rowXmin)/siteWidth.  A height-h filler spans h rows.
#pragma once

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "FillerRepair.h"
#include "odb/db.h"

namespace utl {
class Logger;
}

namespace dpl_fr {

class DplFillerGrid : public FillerGrid
{
 public:
  // `dirty` is the set of filler instances marked dirty by the upstream DRC
  // step (this adapter does not run DRC).  `filler_masters` is the user filler
  // library, in user order (for preserveUserOrder).
  DplFillerGrid(odb::dbBlock* block,
                const std::set<odb::dbInst*>& dirty,
                const std::vector<odb::dbMaster*>& filler_masters,
                utl::Logger* logger);

  // FillerGrid interface (sites/rows).
  int numRows() const override;
  int numCols(int row) const override;
  SiteKind kindAt(int row, int col) const override;
  Vt vtAt(int row, int col) const override;
  void clearSite(int row, int col) override;
  void placeFiller(const PlacedFiller& f) override;

  // Build the FillerRepair library (vt/width/height in sites/rows) from the
  // user masters, preserving order.
  std::vector<Filler> buildLibrary() const;

 private:
  struct Cell
  {
    SiteKind kind = SiteKind::Empty;
    Vt vt = VT_NONE;
    odb::dbInst* inst = nullptr;  // owning instance (for clearSite)
  };

  int rowOf(int y_dbu) const;
  int colOf(int x_dbu) const;
  int xOfCol(int col) const;
  int yOfRow(int row) const;
  // (vt, width_sites, height_rows) -> master, built from filler_masters_.
  odb::dbMaster* masterFor(const Vt& vt, int width, int height) const;
  static Vt implantVt(odb::dbMaster* master);  // reuse of dpl getImplant idea

  odb::dbBlock* block_;
  utl::Logger* logger_;
  std::vector<odb::dbMaster*> filler_masters_;

  int site_w_ = 1;  // DBU
  int row_h_ = 1;   // DBU
  int core_x_ = 0;  // DBU, region origin
  int core_y_ = 0;  // DBU
  int n_rows_ = 0;
  int n_cols_ = 0;

  std::vector<std::vector<Cell>> grid_;  // [row][col]
  std::set<odb::dbInst*> cleared_;       // dirty insts already deleted
  std::map<std::tuple<Vt, int, int>, odb::dbMaster*> master_map_;
  int placed_seq_ = 0;
};

// Convenience driver (the dpl-side entry the Tcl command calls): build the
// grid + library, run the shared FillerRepair, and return the result.
RepairResult repairDirtyFillers(odb::dbBlock* block,
                                const std::set<odb::dbInst*>& dirty,
                                const std::vector<odb::dbMaster*>& masters,
                                bool preserve_user_order,
                                int min_implant_width,
                                utl::Logger* logger);

// Same, but the dirty fillers are given by instance name (resolved here).
RepairResult repairDirtyFillersByName(
    odb::dbBlock* block,
    const std::vector<odb::dbMaster*>& filler_masters,
    const std::vector<std::string>& dirty_names,
    bool preserve_user_order,
    int min_implant_width,
    utl::Logger* logger);

}  // namespace dpl_fr
