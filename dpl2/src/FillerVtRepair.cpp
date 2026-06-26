#include "FillerVtRepair.h"

#include <algorithm>

namespace dpl_fr {

FillerVtRepair::FillerVtRepair(std::vector<Filler> lib, VtRules rules)
    : lib_(std::move(lib)), rules_(rules)
{
}

std::vector<Vt> FillerVtRepair::libVts() const
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

bool FillerVtRepair::exactFill(int width,
                               const Vt& vt,
                               std::vector<Filler>& out) const
{
  std::vector<Filler> cand;
  for (const Filler& f : lib_) {
    if (f.vt == vt && f.height == 1) {
      cand.push_back(f);
    }
  }
  std::stable_sort(
      cand.begin(), cand.end(), [](const Filler& a, const Filler& b) {
        return a.width > b.width;
      });
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
  return width > 0 && dfs.go(width);
}

// ---- the site-grid MW/MS evaluator -----------------------------------------
int FillerVtRepair::countViolations(
    const std::vector<std::vector<Vt>>& vt,
    const std::vector<std::vector<char>>& present,
    const VtRules& rules)
{
  const int R = static_cast<int>(vt.size());
  auto pres = [&](int r, int c) -> bool {
    return r >= 0 && r < R && c >= 0 && c < static_cast<int>(vt[r].size())
           && present[r][c];
  };
  int mw = 0;
  int ms = 0;
  for (int r = 0; r < R; ++r) {
    const int C = static_cast<int>(vt[r].size());
    for (int c = 0; c < C; ++c) {
      if (!present[r][c]) {
        continue;
      }
      const Vt& v = vt[r][c];
      // horizontal same-VT run through (r,c)
      int hl = c;
      while (pres(r, hl - 1) && vt[r][hl - 1] == v) {
        --hl;
      }
      int hr = c;
      while (pres(r, hr + 1) && vt[r][hr + 1] == v) {
        ++hr;
      }
      const int hrun = hr - hl + 1;
      // vertical same-VT run through (r,c)
      int vt0 = r;
      while (pres(vt0 - 1, c) && vt[vt0 - 1][c] == v) {
        --vt0;
      }
      int vb = r;
      while (pres(vb + 1, c) && vt[vb + 1][c] == v) {
        ++vb;
      }
      const int vrun = vb - vt0 + 1;
      // MW: narrow neck unless covered by a min_width bar either way.
      if (hrun < rules.min_width && vrun < rules.min_width) {
        ++mw;
      }
      // MS: touching different-VT regions (right + down to avoid double count).
      if (rules.min_spacing >= 1) {
        if (pres(r, c + 1) && vt[r][c + 1] != v) {
          ++ms;
        }
        if (pres(r + 1, c) && vt[r + 1][c] != v) {
          ++ms;
        }
      }
    }
  }

  // MW case (b): inter-row overlap neck.  Two adjacent rows whose same-VT
  // regions are each wider than min_width but offset, overlapping in too few
  // columns -> the vertical bridge between them is a narrow neck that case (a)
  // misses (the overlap columns have a long in-row horizontal run, so (a)
  // passes them).  Count one neck per such bridge.
  int neck = 0;
  for (int r = 0; r + 1 < R; ++r) {
    const int C = static_cast<int>(vt[r].size());
    auto overlap = [&](int cc) {
      return pres(r, cc) && pres(r + 1, cc) && vt[r][cc] == vt[r + 1][cc];
    };
    int c = 0;
    while (c < C) {
      if (!overlap(c)) {
        ++c;
        continue;
      }
      const Vt& v = vt[r][c];
      int e = c;
      while (e < C && overlap(e) && vt[r][e] == v) {
        ++e;
      }
      const int w = e - c;  // bridge (overlap) width
      if (w < rules.min_width) {
        // horizontal same-VT run width spanning the overlap in each row
        int l0 = c, r0 = e - 1;
        while (pres(r, l0 - 1) && vt[r][l0 - 1] == v) {
          --l0;
        }
        while (pres(r, r0 + 1) && vt[r][r0 + 1] == v) {
          ++r0;
        }
        int l1 = c, r1 = e - 1;
        while (pres(r + 1, l1 - 1) && vt[r + 1][l1 - 1] == v) {
          --l1;
        }
        while (pres(r + 1, r1 + 1) && vt[r + 1][r1 + 1] == v) {
          ++r1;
        }
        // Only a real neck if BOTH rows extend beyond the bridge (else the
        // narrow region is already a case-(a) violation in one row).
        if ((r0 - l0 + 1) > w && (r1 - l1 + 1) > w) {
          ++neck;
        }
      }
      c = e;
    }
  }
  return mw + ms + neck;
}

// Same-VT orthogonal adjacencies (used as a tie-breaker: merging same VT never
// adds a violation, so maximizing it escapes coordinate-descent plateaus where
// a single-site change needs a partner change to pay off).
static int mergeCount(const std::vector<std::vector<Vt>>& vt,
                      const std::vector<std::vector<char>>& present)
{
  const int R = static_cast<int>(vt.size());
  auto pres = [&](int r, int c) -> bool {
    return r >= 0 && r < R && c >= 0 && c < static_cast<int>(vt[r].size())
           && present[r][c];
  };
  int m = 0;
  for (int r = 0; r < R; ++r) {
    for (int c = 0; c < static_cast<int>(vt[r].size()); ++c) {
      if (!present[r][c]) {
        continue;
      }
      if (pres(r, c + 1) && vt[r][c + 1] == vt[r][c]) {
        ++m;
      }
      if (pres(r + 1, c) && vt[r + 1][c] == vt[r][c]) {
        ++m;
      }
    }
  }
  return m;
}

VtRepairResult FillerVtRepair::repair(FillerGrid& grid) const
{
  const int R = grid.numRows();
  std::vector<std::vector<Vt>> vt(R);
  std::vector<std::vector<char>> present(R);
  std::vector<std::vector<char>> filler(R);  // changeable
  std::vector<std::vector<Vt>> orig(R);
  for (int r = 0; r < R; ++r) {
    const int C = grid.numCols(r);
    vt[r].resize(C);
    present[r].resize(C, 0);
    filler[r].resize(C, 0);
    for (int c = 0; c < C; ++c) {
      const SiteKind k = grid.kindAt(r, c);
      const bool pres = (k == SiteKind::Cell || k == SiteKind::CleanFiller);
      present[r][c] = pres ? 1 : 0;
      vt[r][c] = pres ? grid.vtAt(r, c) : VT_NONE;
      filler[r][c] = (k == SiteKind::CleanFiller) ? 1 : 0;
    }
    orig[r] = vt[r];
  }

  VtRepairResult res;
  res.violations_before = countViolations(vt, present, rules_);

  const std::vector<Vt> cand_vts = libVts();

  // Combined cost: minimize violations first, then maximize same-VT merge to
  // break plateaus (a single-site change often needs a partner to pay off).
  auto score = [&]() -> long {
    return static_cast<long>(countViolations(vt, present, rules_)) * 1000L
           - mergeCount(vt, present);
  };

  // Tier-1 greedy coordinate descent over filler-site VTs.
  bool improved = true;
  int rounds = 0;
  while (improved && rounds++ < 100) {
    improved = false;
    for (int r = 0; r < R; ++r) {
      for (int c = 0; c < static_cast<int>(vt[r].size()); ++c) {
        if (!filler[r][c]) {
          continue;
        }
        const Vt cur = vt[r][c];
        Vt best = cur;
        long best_cost = score();
        for (const Vt& cv : cand_vts) {
          if (cv == cur) {
            continue;
          }
          vt[r][c] = cv;
          const long cost = score();
          if (cost < best_cost) {
            best_cost = cost;
            best = cv;
          }
        }
        vt[r][c] = best;
        if (best != cur) {
          improved = true;
        }
      }
    }
  }

  // Apply: for each row, re-emit maximal same-VT filler runs whose VT changed,
  // tiling with library masters.  Untileable runs are reverted (residual).
  for (int r = 0; r < R; ++r) {
    const int C = static_cast<int>(vt[r].size());
    int c = 0;
    while (c < C) {
      if (!filler[r][c]) {
        ++c;
        continue;
      }
      const Vt v = vt[r][c];
      int e = c;
      while (e < C && filler[r][e] && vt[r][e] == v) {
        ++e;
      }
      const int w = e - c;
      bool changed = false;
      for (int i = c; i < e; ++i) {
        if (orig[r][i] != v) {
          changed = true;
        }
      }
      if (changed) {
        std::vector<Filler> fl;
        if (exactFill(w, v, fl)) {
          for (int i = c; i < e; ++i) {
            grid.clearSite(r, i);
          }
          int cc = c;
          for (const Filler& f : fl) {
            PlacedFiller p{r, cc, f.width, 1, v, f.name};
            grid.placeFiller(p);
            res.replaced.push_back(p);
            cc += f.width;
          }
        } else {
          // cannot realize this VT here -> revert to keep grid consistent
          for (int i = c; i < e; ++i) {
            vt[r][i] = orig[r][i];
          }
        }
      }
      c = e;
    }
  }

  res.violations_after = countViolations(vt, present, rules_);
  res.unresolved = res.violations_after;
  return res;
}

}  // namespace dpl_fr
