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

std::vector<Vt> FillerRepair::vtsInLib(int height) const
{
  std::vector<Vt> out;
  for (const Filler& f : lib_) {
    if (f.height != height || f.vt == VT_NONE) {
      continue;
    }
    if (std::find(out.begin(), out.end(), f.vt) == out.end()) {
      out.push_back(f.vt);
    }
  }
  return out;
}

// Exact-fill `width` sites using `vt`/`height` fillers (backtracking).  This is
// the "fitGap" behavior: it only succeeds on an exact fit, so e.g. a gap of 9
// with fillers {8,4,3,2} yields 4+3+2 and rejects 8 (which would orphan 1
// site).
bool FillerRepair::packExact(int width,
                             const Vt& vt,
                             int height,
                             std::vector<Filler>& out) const
{
  const std::vector<Filler> cand = candidates(vt, height);

  // Recursive DFS preserving left-to-right order in `out`.
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
                               int height,
                               std::vector<PlacedFiller>& out) const
{
  const int W = w.width;
  const Vt& L = w.left_vt;
  const Vt& R = w.right_vt;

  auto emit = [&](int col, const std::vector<Filler>& fillers) {
    int c = col;
    for (const Filler& f : fillers) {
      out.push_back(PlacedFiller{w.row, c, f.width, f.vt, f.name});
      c += f.width;
    }
  };

  // Fill the whole window with a single VT.  Valid when that VT merges into a
  // same-VT fixed neighbor (no narrow stand-alone strip); otherwise the strip
  // is stand-alone and must itself satisfy min_implant_width.
  auto trySingle = [&](const Vt& vt) -> bool {
    if (vt == VT_NONE) {
      return false;
    }
    const bool merges = (L == vt) || (R == vt);
    if (!merges && W < rules_.min_implant_width) {
      return false;
    }
    std::vector<Filler> fl;
    if (!packExact(W, vt, height, fl)) {
      return false;
    }
    out.clear();
    emit(w.col0, fl);
    return true;
  };

  // 1) Extend the left neighbor's VT across the window.
  if (L != VT_NONE && trySingle(L)) {
    return true;
  }
  // 2) Extend the right neighbor's VT across the window.
  if (R != VT_NONE && R != L && trySingle(R)) {
    return true;
  }
  // 3) Neighbors differ: split into L-segment then R-segment.  Each segment
  //    merges into its own same-VT neighbor, so only exact-fill must hold.
  if (L != VT_NONE && R != VT_NONE && L != R) {
    for (int s = 1; s < W; ++s) {
      std::vector<Filler> fl_l, fl_r;
      if (packExact(s, L, height, fl_l) && packExact(W - s, R, height, fl_r)) {
        out.clear();
        emit(w.col0, fl_l);
        emit(w.col0 + s, fl_r);
        return true;
      }
    }
  }
  // 4) Both neighbors unknown (window isolated): stand-alone strip, try any VT.
  if (L == VT_NONE && R == VT_NONE) {
    for (const Vt& vt : vtsInLib(height)) {
      if (W >= rules_.min_implant_width) {
        std::vector<Filler> fl;
        if (packExact(W, vt, height, fl)) {
          out.clear();
          emit(w.col0, fl);
          return true;
        }
      }
    }
  }
  return false;
}

RepairResult FillerRepair::repair(Layout& layout) const
{
  RepairResult result;

  for (int ri = 0; ri < static_cast<int>(layout.rows.size()); ++ri) {
    Row& row = layout.rows[ri];
    const int n = static_cast<int>(row.sites.size());

    // Collect maximal runs of DIRTY sites (windows) first, using the original
    // fixed neighbors (clean filler / cell) to determine boundary VT.
    std::vector<Window> windows;
    int c = 0;
    while (c < n) {
      if (row.sites[c].kind != SiteKind::DirtyFiller) {
        ++c;
        continue;
      }
      int start = c;
      while (c < n && row.sites[c].kind == SiteKind::DirtyFiller) {
        ++c;
      }
      Window w;
      w.row = ri;
      w.col0 = start;
      w.width = c - start;
      auto fixedVt = [&](int idx) -> Vt {
        if (idx < 0 || idx >= n) {
          return VT_NONE;
        }
        const Site& s = row.sites[idx];
        if (s.kind == SiteKind::Cell || s.kind == SiteKind::CleanFiller) {
          return s.vt;
        }
        return VT_NONE;
      };
      w.left_vt = fixedVt(start - 1);
      w.right_vt = fixedVt(c);
      windows.push_back(w);
    }

    for (Window& w : windows) {
      // Delete dirty fillers in this window.
      for (int i = w.col0; i < w.col0 + w.width; ++i) {
        row.sites[i] = Site{SiteKind::Empty, VT_NONE};
      }

      std::vector<PlacedFiller> segs;
      if (solveWindow(w, row.height, segs)) {
        for (const PlacedFiller& p : segs) {
          for (int i = p.col; i < p.col + p.width; ++i) {
            row.sites[i] = Site{SiteKind::CleanFiller, p.vt};
          }
          result.placed.push_back(p);
        }
      } else {
        w.reason = "cannot exact-fill window with available fillers";
        result.unsolved.push_back(w);
      }
    }
  }

  return result;
}

}  // namespace dpl_fr
