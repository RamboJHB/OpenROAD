// Filler insertion: DRC-driven dirty-filler repair (core, zero-dependency).
//
// PHASE I (this file): SINGLE-ROW repair only.
//   - Input: a site-grid whose violating fillers are already marked DIRTY
//     (the DRC step is upstream and intentionally NOT modeled here).
//   - Action: per row, delete dirty fillers and refill their footprint with
//     correct (height-1) fillers, fixing intra-row spacing / min-width by
//     restoring same-row implant (VT) continuity.
//   - Strict "only-dirty": clean fillers / cells are fixed boundaries.
//   - Exact-fill ("fitGap", fixed on): no 1-site filler, so a residue gap is
//     forbidden (gap 9 -> 4+3+2, never 8).
//   - preserveUserOrder honored.  Each dirty window is solved independently.
//
// PHASE II (later, see docs/filler_insertion.md): multi-row.
//   - true multi-height fillers spanning rows;
//   - inter-row MW/MS constraints (the paper's cost-table idea: take the
//     up/down neighbor rows as fixed context and reject VT choices that
//     create cross-row violations).
//   The Filler/PlacedFiller `height` field and the FillerGrid interface are
//   kept multi-row-ready so Phase II is an extension, not a rewrite.
//
// The algorithm runs against the abstract FillerGrid interface so it is
// portable (see FakeFillerGrid.h / docs/filler_repair_porting.md).
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
  int height = 1;  // in rows; PHASE I uses only height-1 fillers
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
// [col, col+width).  PHASE I always emits height == 1.
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
// Portable grid abstraction.  Implement this against any database to port the
// repair (see docs/filler_repair_porting.md).  The FillerRepair algorithm only
// ever touches the grid through this interface.
//
// Coordinate model: uniform single-row pitch; a column is one site; a
// height-h filler / cell occupies rows [row, row+h) x cols [col, col+w).
// Distances are in sites / rows, never DBU (the adapter owns DBU geometry).
//
// Contract:
//   - kindAt/vtAt valid for 0<=col<numCols(row); vtAt meaningful for Cell /
//     CleanFiller.
//   - clearSite(r,c) leaves (r,c) Empty (deletes the dirty filler there).
//   - placeFiller(f) creates one instance over f's block (CleanFiller).
//   - "only-dirty": the algorithm never clears/places over Cell / CleanFiller /
//     Blocked sites.
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
  virtual void clearSite(int row, int col) = 0;
  virtual void placeFiller(const PlacedFiller& f) = 0;
};

class FillerRepair
{
 public:
  FillerRepair(std::vector<Filler> lib, bool preserve_user_order, Rules rules);

  // Deletes DIRTY fillers and refills solvable windows (per row), mutating
  // `grid`.
  RepairResult repair(FillerGrid& grid) const;

 private:
  // A single-row run of dirty sites to refill.
  struct Window
  {
    int row = 0;
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
  // Exact-fill `width` columns with height-1 `vt` fillers (backtracking).
  bool packExact(int width, const Vt& vt, std::vector<Filler>& out) const;
  // Resolve one single-row window into placed (height-1) fillers.
  bool solveWindow(const Window& w, std::vector<PlacedFiller>& out) const;

  std::vector<Filler> lib_;
  bool preserve_user_order_;
  Rules rules_;
};

}  // namespace dpl_fr
