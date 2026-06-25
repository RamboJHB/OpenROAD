// Portable filler-grid abstraction + shared value types for filler repair.
//
// The repair algorithm is database-agnostic: it only ever touches a design
// through the abstract FillerGrid interface below, in *site / row* coordinates
// (never DBU).  To port the repair to another database, implement FillerGrid
// against that database (see docs/filler_vt_repair_spec.md §7 and
// FakeFillerGrid.h for a reference in-memory implementation).
#pragma once

#include <string>
#include <vector>

namespace dpl_fr {

// VT / implant identity.  A site's VT is *which* IMPLANT-type layer its master
// carries (e.g. LVTN -> "L", HVTN -> "H").  Empty string == "none / no
// implant".
using Vt = std::string;
inline const Vt VT_NONE = "";

// One filler master in the library, measured in site/row units.
struct Filler
{
  int width = 1;   // in sites (columns)
  int height = 1;  // in rows (single-row pitch units)
  Vt vt;
  std::string name;
};

// What occupies a site on the grid.
enum class SiteKind
{
  Empty,        // free site, no implant
  Cell,         // standard cell (fixed boundary, carries a VT)
  CleanFiller,  // filler that is DRC-clean (changeable by the repair)
  DirtyFiller,  // filler an upstream DRC step flagged (Phase I territory)
  Blocked       // macro / blockage (fixed boundary)
};

// A placed filler instance covering rows [row, row+height) x cols
// [col, col+width).
struct PlacedFiller
{
  int row = 0;
  int col = 0;
  int width = 0;
  int height = 1;
  Vt vt;
  std::string name;
};

// ---------------------------------------------------------------------------
// Portable grid abstraction.  Implement this against any database to port the
// repair.  The algorithm only ever touches the grid through this interface.
//
// Coordinate model:
//   - Uniform single-row pitch.  Rows are indexed 0..numRows()-1; only
//     adjacency (row, row+1) matters for the vertical (inter-row) checks.
//   - A column is one site.  All distances here are in *sites / rows*, never
//     DBU -- the adapter owns the DBU<->site geometry and the
//     (vt,width,height) -> master mapping.
//   - A height-h filler / cell occupies rows [row, row+h) x cols [col, col+w).
//
// Contract the implementation must satisfy:
//   - kindAt/vtAt are valid for 0 <= col < numCols(row).
//   - vtAt is meaningful for Cell / CleanFiller sites; ignored otherwise.
//   - clearSite(r,c) must leave (r,c) as Empty (it deletes the filler instance
//     occupying that site in a real DB).
//   - placeFiller(f) must create exactly one instance covering f's block and
//     mark those sites occupied (CleanFiller); the algorithm never overlaps
//     placements and only ever rewrites filler runs it just cleared.
//   - The repair is "only-filler": it never clears or places over Cell /
//     Blocked sites, so those stay fixed.
// ---------------------------------------------------------------------------
class FillerGrid
{
 public:
  virtual ~FillerGrid() = default;

  // ---- read ----
  virtual int numRows() const = 0;
  virtual int numCols(int row) const = 0;
  virtual SiteKind kindAt(int row, int col) const = 0;
  virtual Vt vtAt(int row, int col) const = 0;

  // ---- mutate ----
  // Delete the filler covering this site (the site becomes Empty).
  virtual void clearSite(int row, int col) = 0;
  // Create one filler instance covering f's row/col/width/height block.
  virtual void placeFiller(const PlacedFiller& f) = 0;
};

}  // namespace dpl_fr
