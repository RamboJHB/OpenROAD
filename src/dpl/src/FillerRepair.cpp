#include "FillerRepair.h"

#include <algorithm>

namespace dpl_fr {

FillerRepair::FillerRepair(std::vector<Filler> lib,
                           bool preserve_user_order,
                           Rules rules)
    : lib_(std::move(lib)),
      preserve_user_order_(preserve_user_order),
      rules_(rules)
{
}

std::vector<Filler> FillerRepair::candidates(const Vt& vt, int height) const
{
  std::vector<Filler> out;
  for (const Filler& f : lib_) {
    if (f.vt == vt && f.height == height) {
      out.push_back(f);
    }
  }
  if (!preserve_user_order_) {
    // Default: widest-first (matches master fillerPlacement).
    std::stable_sort(
        out.begin(), out.end(), [](const Filler& a, const Filler& b) {
          return a.width > b.width;
        });
  }
  return out;
}

std::vector<Vt> FillerRepair::vtsInLib() const
{
  std::vector<Vt> out;
  for (const Filler& f : lib_) {
    if (f.vt != VT_NONE
        && std::find(out.begin(), out.end(), f.vt) == out.end()) {
      out.push_back(f.vt);
    }
  }
  return out;
}

// Exact-fill `width` columns using height-1 `vt` fillers (backtracking).  This
// is the "fitGap" behavior: it only succeeds on an exact fit, so a gap of 9
// with fillers {8,4,3,2} yields 4+3+2 and rejects 8 (which would orphan 1).
bool FillerRepair::packExact(int width,
                             const Vt& vt,
                             std::vector<Filler>& out) const
{
  const std::vector<Filler> cand = candidates(vt, /*height=*/1);

  struct Dfs
  {
    const std::vector<Filler>& cand;
    std::vector<Filler>& out;
    bool go(int rem)
    {
      if (rem == 0) {
        return true;
      }
      for (const Filler& f : cand) {
        if (f.width <= rem) {
          out.push_back(f);
          if (go(rem - f.width)) {
            return true;
          }
          out.pop_back();
        }
      }
      return false;
    }
  } dfs{cand, out};

  out.clear();
  if (width <= 0) {
    return false;
  }
  return dfs.go(width);
}

bool FillerRepair::solveWindow(const Window& w,
                               std::vector<PlacedFiller>& out) const
{
  const int W = w.width;
  const Vt& L = w.left_vt;
  const Vt& R = w.right_vt;

  // Candidate horizontal VT layouts (segment lists), in priority order.
  std::vector<std::vector<Seg>> plans;
  auto addSingle = [&](const Vt& vt) {
    if (vt == VT_NONE) {
      return;
    }
    const bool merges = (L == vt) || (R == vt);
    if (!merges && W < rules_.min_implant_width) {
      return;  // stand-alone narrow strip violates min-width
    }
    plans.push_back({Seg{vt, w.col0, W}});
  };
  // 1) extend left VT, 2) extend right VT.
  addSingle(L);
  if (R != L) {
    addSingle(R);
  }
  // 3) neighbors differ: split L | R at every column.
  if (L != VT_NONE && R != VT_NONE && L != R) {
    for (int s = 1; s < W; ++s) {
      plans.push_back({Seg{L, w.col0, s}, Seg{R, w.col0 + s, W - s}});
    }
  }
  // 4) isolated window: try any VT (stand-alone, needs min-width).
  if (L == VT_NONE && R == VT_NONE) {
    for (const Vt& vt : vtsInLib()) {
      if (W >= rules_.min_implant_width) {
        plans.push_back({Seg{vt, w.col0, W}});
      }
    }
  }

  // First plan whose every segment exact-fills with height-1 fillers wins.
  for (const std::vector<Seg>& plan : plans) {
    std::vector<std::vector<Filler>> fills(plan.size());
    bool ok = true;
    for (size_t i = 0; i < plan.size(); ++i) {
      if (!packExact(plan[i].width, plan[i].vt, fills[i])) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      continue;
    }
    out.clear();
    for (size_t i = 0; i < plan.size(); ++i) {
      int c = plan[i].col;
      for (const Filler& f : fills[i]) {
        out.push_back(
            PlacedFiller{w.row, c, f.width, /*height=*/1, plan[i].vt, f.name});
        c += f.width;
      }
    }
    return true;
  }
  return false;
}

RepairResult FillerRepair::repair(FillerGrid& grid) const
{
  RepairResult result;

  auto fixedVt = [&](int row, int col) -> Vt {
    if (col < 0 || col >= grid.numCols(row)) {
      return VT_NONE;
    }
    const SiteKind k = grid.kindAt(row, col);
    if (k == SiteKind::Cell || k == SiteKind::CleanFiller) {
      return grid.vtAt(row, col);
    }
    return VT_NONE;
  };

  // Per-row maximal dirty runs -> independent single-row windows.
  for (int r = 0; r < grid.numRows(); ++r) {
    const int nc = grid.numCols(r);
    int c = 0;
    while (c < nc) {
      if (grid.kindAt(r, c) != SiteKind::DirtyFiller) {
        ++c;
        continue;
      }
      const int start = c;
      while (c < nc && grid.kindAt(r, c) == SiteKind::DirtyFiller) {
        ++c;
      }
      Window w;
      w.row = r;
      w.col0 = start;
      w.width = c - start;
      w.left_vt = fixedVt(r, start - 1);
      w.right_vt = fixedVt(r, c);

      // Delete dirty, then refill.
      for (int cc = w.col0; cc < w.col0 + w.width; ++cc) {
        grid.clearSite(r, cc);
      }
      std::vector<PlacedFiller> segs;
      if (solveWindow(w, segs)) {
        for (const PlacedFiller& p : segs) {
          grid.placeFiller(p);
          result.placed.push_back(p);
        }
      } else {
        result.unsolved.push_back(
            UnsolvedWindow{w.row,
                           w.col0,
                           w.width,
                           1,
                           "cannot exact-fill window with available fillers"});
      }
    }
  }

  return result;
}

}  // namespace dpl_fr
