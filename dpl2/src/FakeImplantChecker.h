// FakeImplantChecker: in-memory ImplantChecker for tests / porting reference.
//
// Backs the oracle with a FakeFillerGrid + a filler library + VtRules, and
// answers using the same site-grid MW/MS evaluator the standalone path uses
// (FillerVtRepair::countViolations, which now includes case-B).  A real adapter
// would instead delegate to ecoPlace's ImplantLayerChecker; this fake lets the
// search layer (FillerVtRepairOracle) be unit-tested with no real checker.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "FakeFillerGrid.h"
#include "FillerVtRepair.h"  // VtRules, countViolations
#include "ImplantChecker.h"

namespace dpl_fr {

class FakeImplantChecker : public ImplantChecker
{
 public:
  FakeImplantChecker(FakeFillerGrid grid,
                     std::vector<Filler> lib,
                     VtRules rules)
      : grid_(std::move(grid)), lib_(std::move(lib)), rules_(rules)
  {
  }

  const FakeFillerGrid& grid() const { return grid_; }

  int violations() override
  {
    std::vector<std::vector<Vt>> vt;
    std::vector<std::vector<char>> present;
    extract(vt, present);
    return FillerVtRepair::countViolations(vt, present, rules_);
  }

  std::vector<FillerRun> changeableRuns() override
  {
    std::vector<FillerRun> out;
    const int R = grid_.numRows();
    for (int r = 0; r < R; ++r) {
      const int C = grid_.numCols(r);
      int c = 0;
      while (c < C) {
        if (grid_.kindAt(r, c) != SiteKind::CleanFiller) {
          ++c;
          continue;
        }
        const Vt v = grid_.vtAt(r, c);
        int e = c;
        while (e < C && grid_.kindAt(r, e) == SiteKind::CleanFiller
               && grid_.vtAt(r, e) == v) {
          ++e;
        }
        out.push_back(FillerRun{r, c, e - c, v});
        c = e;
      }
    }
    return out;
  }

  std::vector<Vt> candidateVts() override
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

  int evalReplaceRun(const FillerRun& run, const Vt& vt) override
  {
    std::vector<Filler> tiling;
    if (!exactFill(run.width, vt, tiling)) {
      return kUnrealizable;
    }
    std::vector<std::vector<Vt>> v;
    std::vector<std::vector<char>> present;
    extract(v, present);
    for (int c = run.col; c < run.col + run.width; ++c) {
      v[run.row][c] = vt;  // occupancy unchanged, only VT
    }
    return FillerVtRepair::countViolations(v, present, rules_);
  }

  bool commitReplaceRun(const FillerRun& run, const Vt& vt) override
  {
    std::vector<Filler> tiling;
    if (!exactFill(run.width, vt, tiling)) {
      return false;
    }
    for (int c = run.col; c < run.col + run.width; ++c) {
      grid_.clearSite(run.row, c);
    }
    int cc = run.col;
    for (const Filler& f : tiling) {
      grid_.placeFiller(PlacedFiller{run.row, cc, f.width, 1, vt, f.name});
      cc += f.width;
    }
    return true;
  }

 private:
  static constexpr int kUnrealizable = 1 << 20;

  void extract(std::vector<std::vector<Vt>>& vt,
               std::vector<std::vector<char>>& present) const
  {
    const int R = grid_.numRows();
    vt.assign(R, {});
    present.assign(R, {});
    for (int r = 0; r < R; ++r) {
      const int C = grid_.numCols(r);
      vt[r].resize(C);
      present[r].assign(C, 0);
      for (int c = 0; c < C; ++c) {
        const SiteKind k = grid_.kindAt(r, c);
        const bool p = (k == SiteKind::Cell || k == SiteKind::CleanFiller);
        present[r][c] = p ? 1 : 0;
        vt[r][c] = p ? grid_.vtAt(r, c) : VT_NONE;
      }
    }
  }

  // Exact-tile `width` with height-1 `vt` masters (backtracking, widest-first).
  bool exactFill(int width, const Vt& vt, std::vector<Filler>& out) const
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

  FakeFillerGrid grid_;
  std::vector<Filler> lib_;
  VtRules rules_;
};

}  // namespace dpl_fr
