#include "FillerRepair.h"

#include <algorithm>
#include <map>
#include <tuple>

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

std::vector<int> FillerRepair::heightsInLib() const
{
  std::vector<int> out;
  for (const Filler& f : lib_) {
    if (std::find(out.begin(), out.end(), f.height) == out.end()) {
      out.push_back(f.height);
    }
  }
  std::sort(out.begin(), out.end(), std::greater<int>());  // tallest first
  return out;
}

// Exact-fill `width` columns using `vt`/`height` fillers (backtracking).  This
// is the "fitGap" behavior: it only succeeds on an exact fit, so a gap of 9
// with fillers {8,4,3,2} yields 4+3+2 and rejects 8 (which would orphan 1).
bool FillerRepair::packExact(int width,
                             const Vt& vt,
                             int height,
                             std::vector<Filler>& out) const
{
  const std::vector<Filler> cand = candidates(vt, height);

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

// Exact integer partition of H into stripe heights from `allowed` (DFS).
bool FillerRepair::partitionHeight(int H,
                                   const std::vector<int>& allowed,
                                   std::vector<int>& out) const
{
  if (H == 0) {
    return true;
  }
  for (int h : allowed) {
    if (h <= H) {
      out.push_back(h);
      if (partitionHeight(H - h, allowed, out)) {
        return true;
      }
      out.pop_back();
    }
  }
  return false;
}

bool FillerRepair::solveWindow(const MultiWindow& w,
                               std::vector<PlacedFiller>& out) const
{
  const int W = w.width;
  const int H = w.height;
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

  const std::vector<int> all_heights = heightsInLib();

  for (const std::vector<Seg>& plan : plans) {
    // A stripe height h is usable only if EVERY segment is exact-fillable with
    // height-h fillers of its VT.
    std::vector<int> allowed;
    for (int h : all_heights) {
      bool ok = true;
      for (const Seg& seg : plan) {
        std::vector<Filler> tmp;
        if (!packExact(seg.width, seg.vt, h, tmp)) {
          ok = false;
          break;
        }
      }
      if (ok) {
        allowed.push_back(h);
      }
    }
    std::vector<int> stripes;
    if (!partitionHeight(H, allowed, stripes)) {
      continue;
    }

    // Emit: place each stripe (top to bottom); a height-h filler covers h rows.
    out.clear();
    int rr = w.row0;
    for (int h : stripes) {
      for (const Seg& seg : plan) {
        std::vector<Filler> fl;
        packExact(seg.width, seg.vt, h, fl);
        int c = seg.col;
        for (const Filler& f : fl) {
          out.push_back(PlacedFiller{rr, c, f.width, h, seg.vt, f.name});
          c += f.width;
        }
      }
      rr += h;
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

  // 1) Per-row maximal dirty runs, keyed by (col0,width,left_vt,right_vt).
  using Key = std::tuple<int, int, Vt, Vt>;
  std::map<Key, std::vector<int>> by_key;  // -> sorted row indices
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
      Key key{start, c - start, fixedVt(r, start - 1), fixedVt(r, c)};
      by_key[key].push_back(r);
    }
  }

  // 2) Merge identical per-row windows in consecutive rows into rectangular
  //    multi-height windows (each maximal run of stacked rows).
  std::vector<MultiWindow> windows;
  for (auto& [key, rows] : by_key) {
    std::sort(rows.begin(), rows.end());
    size_t i = 0;
    while (i < rows.size()) {
      size_t j = i;
      while (j + 1 < rows.size() && rows[j + 1] == rows[j] + 1) {
        ++j;
      }
      MultiWindow w;
      w.row0 = rows[i];
      w.height = static_cast<int>(j - i + 1);
      w.col0 = std::get<0>(key);
      w.width = std::get<1>(key);
      w.left_vt = std::get<2>(key);
      w.right_vt = std::get<3>(key);
      windows.push_back(w);
      i = j + 1;
    }
  }

  // 3) Delete dirty + refill each window.
  for (const MultiWindow& w : windows) {
    for (int rr = w.row0; rr < w.row0 + w.height; ++rr) {
      for (int cc = w.col0; cc < w.col0 + w.width; ++cc) {
        grid.clearSite(rr, cc);
      }
    }
    std::vector<PlacedFiller> segs;
    if (solveWindow(w, segs)) {
      for (const PlacedFiller& p : segs) {
        grid.placeFiller(p);
        result.placed.push_back(p);
      }
    } else {
      result.unsolved.push_back(
          UnsolvedWindow{w.row0,
                         w.col0,
                         w.width,
                         w.height,
                         "cannot exact-fill window with available fillers"});
    }
  }

  return result;
}

}  // namespace dpl_fr
