// Filler insertion: DRC-driven dirty-filler repair (core, zero-dependency).
//
// Scope (see docs/filler_insertion.md):
//   - Input: a site-grid whose violating fillers are already marked DIRTY
//     (the DRC step is upstream and intentionally NOT modeled here).
//   - Action: delete dirty fillers and refill their footprint with correct
//     fillers, fixing spacing / min-width by restoring implant (VT) continuity.
//   - Strict "only-dirty": clean fillers / cells are fixed boundaries.
//   - Exact-fill ("fitGap", fixed on): no 1-site filler, so a residue gap is
//     forbidden (gap 9 -> 4+3+2, never 8).
//   - preserveUserOrder, VT continuity, and true multi-height fillers.
//
// The algorithm runs against the abstract FillerGrid interface so it is
// portable: the fake in-memory FakeFillerGrid (see FakeFillerGrid.h) backs the
// unit tests, and a real database (OpenROAD dpl grid_, or another DB) plugs in
// by implementing the same interface.
#pragma once

#include <string>
#include <vector>

namespace dpl_fr {

// VT / implant identity.  Empty string == "none / don't care".
using Vt = std::string;
inline const Vt VT_NONE = "";

struct Filler
{
  int width = 1;   // in sites (columns)
  int height = 1;  // in rows (single-row pitch units)
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

struct Rules
{
  // Minimum width (sites) of a stand-alone same-VT implant strip (one that
  // does not merge into a same-VT neighbor).  1 == effectively off.
  int min_implant_width = 1;
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

struct UnsolvedWindow
{
  int row = 0;
  int col = 0;
  int width = 0;
  int height = 1;
  std::string reason;
};

struct RepairResult
{
  std::vector<PlacedFiller> placed;
  std::vector<UnsolvedWindow> unsolved;
};

// ---------------------------------------------------------------------------
// Portable grid abstraction.  Implement this against any database.
//
// Model: uniform single-row pitch.  A height-h filler / cell spans the h
// consecutive rows [row, row+h).  Coordinates are (row, col=site).
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
  // Delete the dirty filler covering this site (the site becomes Empty).
  virtual void clearSite(int row, int col) = 0;
  // Create one filler instance covering f's row/col/width/height block.
  virtual void placeFiller(const PlacedFiller& f) = 0;
};

class FillerRepair
{
 public:
  FillerRepair(std::vector<Filler> lib, bool preserve_user_order, Rules rules);

  // Deletes DIRTY fillers and refills solvable windows, mutating `grid`.
  RepairResult repair(FillerGrid& grid) const;

 private:
  // A rectangular block of dirty sites to refill.
  struct MultiWindow
  {
    int row0 = 0;
    int height = 1;
    int col0 = 0;
    int width = 0;
    Vt left_vt = VT_NONE;
    Vt right_vt = VT_NONE;
  };
  // One horizontal VT segment within a window.
  struct Seg
  {
    Vt vt;
    int col = 0;
    int width = 0;
  };

  std::vector<Filler> candidates(const Vt& vt, int height) const;
  std::vector<Vt> vtsInLib() const;
  std::vector<int> heightsInLib() const;
  // Exact-fill `width` columns with `vt`/`height` fillers (backtracking).
  bool packExact(int width,
                 const Vt& vt,
                 int height,
                 std::vector<Filler>& out) const;
  // Exact integer partition of H into stripe heights drawn from `allowed`.
  bool partitionHeight(int H,
                       const std::vector<int>& allowed,
                       std::vector<int>& out) const;
  // Resolve a rectangular window into placed (multi-height) fillers.
  bool solveWindow(const MultiWindow& w, std::vector<PlacedFiller>& out) const;

  std::vector<Filler> lib_;
  bool preserve_user_order_;
  Rules rules_;
};

}  // namespace dpl_fr
