// Filler insertion: DRC-driven dirty-filler repair (core, zero-dependency).
//
// Scope (see docs/filler_insertion.md):
//   - Input: a site-grid whose violating fillers are already marked DIRTY
//     (the DRC step is upstream and intentionally NOT modeled here).
//   - Action: delete dirty fillers and refill their footprint with correct
//     fillers, fixing spacing / min-width by restoring implant (VT) continuity.
//   - Strict "only-dirty": clean fillers / cells are fixed boundaries, never
//     moved.  A window that cannot be exact-filled is reported as unsolved.
//   - Packing is exact-fill ("fitGap" behavior, fixed on): the library has no
//     1-site filler, so a residue gap cannot be cleaned up and is forbidden.
//   - preserveUserOrder, VT continuity and multi-height (height-matched
//     fillers) are honored.  avoid_abutment_patterns is out of scope for now.
//
// This core is deliberately free of ODB/dpl so it can be unit-tested stand
// alone; the dpl/Tcl adapter maps grid_/getImplant() onto these types.
#pragma once

#include <string>
#include <vector>

namespace dpl_fr {

// VT / implant identity.  Empty string == "none / don't care".
using Vt = std::string;
inline const Vt VT_NONE = "";

struct Filler
{
  int width = 1;   // in sites
  int height = 1;  // in row-height units
  Vt vt;
  std::string name;
};

enum class SiteKind
{
  Empty,
  Cell,
  CleanFiller,
  DirtyFiller,
  Blocked
};

struct Site
{
  SiteKind kind = SiteKind::Empty;
  Vt vt = VT_NONE;
};

struct Row
{
  int height = 1;
  std::vector<Site> sites;
};

struct Layout
{
  std::vector<Row> rows;
};

struct PlacedFiller
{
  int row = 0;
  int col = 0;
  int width = 0;
  Vt vt;
  std::string name;
};

struct Window
{
  int row = 0;
  int col0 = 0;
  int width = 0;
  Vt left_vt = VT_NONE;   // fixed left neighbor VT (cell / clean filler)
  Vt right_vt = VT_NONE;  // fixed right neighbor VT
  std::string reason;     // filled when unsolved
};

struct Rules
{
  // Minimum width (in sites) of a stand-alone same-VT implant strip, i.e. one
  // that does not merge into a same-VT neighbor.  1 == effectively off.
  int min_implant_width = 1;
};

struct RepairResult
{
  std::vector<PlacedFiller> placed;  // newly created fillers
  std::vector<Window> unsolved;      // windows filler cannot repair
};

class FillerRepair
{
 public:
  FillerRepair(std::vector<Filler> lib, bool preserve_user_order, Rules rules);

  // Mutates `layout`: deletes DIRTY fillers and refills solvable windows.
  RepairResult repair(Layout& layout) const;

 private:
  std::vector<Filler> candidates(const Vt& vt, int height) const;
  std::vector<Vt> vtsInLib(int height) const;
  bool packExact(int width,
                 const Vt& vt,
                 int height,
                 std::vector<Filler>& out) const;
  // Resolve a window into left-to-right filler segments (single VT or a
  // 2-VT split honoring implant continuity).  Returns false if impossible.
  bool solveWindow(const Window& w,
                   int height,
                   std::vector<PlacedFiller>& out) const;

  std::vector<Filler> lib_;
  bool preserve_user_order_;
  Rules rules_;
};

}  // namespace dpl_fr
