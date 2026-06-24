// FillerVtRepair: fix inter-row implant MW/MS by REPLACING filler VT (no move).
//
// Phase II of the filler repair (see docs/filler_insertion.md §7).  Where
// FillerRepair re-tiles a dirty gap on a single row, FillerVtRepair works on
// the 2D implant picture: it treats every FILLER site as a VT variable and
// every CELL site as a fixed boundary, then chooses filler VTs so the site-grid
// MW (min-width) / MS (min-spacing) violation count is minimized, and applies
// the change by deleting the old filler and creating one of the target VT in
// place.  Cells are never moved.
//
// Site-grid implant model (what MW/MS mean here):
//   - A "present" site = Cell or CleanFiller; it carries a VT.  Empty/Blocked
//     sites carry no implant.
//   - MW: a present site is OK if it sits in a same-VT run of length >=
//   min_width
//     in EITHER the horizontal or the vertical direction; otherwise it is a
//     narrow neck (e.g. a 1-wide inter-row staircase) and counts as 1.
//   - MS: two orthogonally-adjacent present sites of DIFFERENT VT count as 1
//     (different implant regions must not touch when min_spacing >= 1).
//   This is the same site-grid abstraction the rest of the pipeline uses; exact
//   foundry polygon DRC is upstream.
#pragma once

#include <string>
#include <vector>

#include "FillerRepair.h"  // Vt, SiteKind, Filler, PlacedFiller, FillerGrid

namespace dpl_fr {

struct VtRules
{
  int min_width = 1;    // ωMW, in sites (same-VT implant min width)
  int min_spacing = 1;  // ωMS, in sites (>=1 => different VTs may not touch)
};

struct VtRepairResult
{
  int violations_before = 0;
  int violations_after = 0;
  std::vector<PlacedFiller> replaced;  // fillers re-created with a new VT
  int unresolved = 0;                  // violations still present at the end
};

class FillerVtRepair
{
 public:
  FillerVtRepair(std::vector<Filler> lib, VtRules rules);

  // Reads `grid`, picks filler VTs minimizing MW/MS, and applies the change by
  // delete+create (only fillers change; cells stay fixed).
  VtRepairResult repair(FillerGrid& grid) const;

  // Count MW+MS violations of a labeled site grid (exposed for tests).
  static int countViolations(const std::vector<std::vector<Vt>>& vt,
                             const std::vector<std::vector<char>>& present,
                             const VtRules& rules);

 private:
  std::vector<Vt> libVts() const;
  // Exact-fill `width` columns with height-1 `vt` fillers (backtracking).
  bool exactFill(int width, const Vt& vt, std::vector<Filler>& out) const;

  std::vector<Filler> lib_;
  VtRules rules_;
};

}  // namespace dpl_fr
