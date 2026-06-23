// Stand-alone unit test for FillerRepair (PHASE I, single-row), driven through
// the FakeFillerGrid implementation of the portable FillerGrid interface.
// Multi-row (multi-height + inter-row) is Phase II; see docs/filler_insertion.md.
//
// Build & run (sandbox-friendly):
//   g++ -std=c++17 -I src/dpl/src
//       src/dpl/src/FillerRepair.cpp src/dpl/test/filler_repair_test.cpp
//       -o /tmp/fr_test && /tmp/fr_test
//
// DRC is intentionally skipped: each test feeds a grid whose violating fillers
// are already marked DIRTY.
#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "FakeFillerGrid.h"
#include "FillerRepair.h"

using namespace dpl_fr;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& msg)
{
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cout << "  FAIL: " << msg << "\n";
  }
}

// ---- tiny row builder ------------------------------------------------------
struct RB
{
  std::vector<FakeSite> sites;
  RB& add(SiteKind k, const Vt& vt, int w)
  {
    for (int i = 0; i < w; ++i) {
      sites.push_back(FakeSite{k, vt});
    }
    return *this;
  }
  RB& cell(const Vt& vt, int w) { return add(SiteKind::Cell, vt, w); }
  RB& clean(const Vt& vt, int w) { return add(SiteKind::CleanFiller, vt, w); }
  RB& dirty(const Vt& vt, int w) { return add(SiteKind::DirtyFiller, vt, w); }
  RB& empty(int w) { return add(SiteKind::Empty, VT_NONE, w); }
};

static Filler F(int width, const Vt& vt, int height = 1)
{
  return Filler{
      width,
      height,
      vt,
      vt + "w" + std::to_string(width) + "h" + std::to_string(height)};
}

static std::vector<int> placedWidths(const RepairResult& r)
{
  std::vector<int> w;
  for (const auto& p : r.placed) {
    w.push_back(p.width);
  }
  std::sort(w.begin(), w.end());
  return w;
}

static bool noDirty(const FakeFillerGrid& g)
{
  for (const auto& row : g.rows) {
    for (const auto& s : row) {
      if (s.kind == SiteKind::DirtyFiller) {
        return false;
      }
    }
  }
  return true;
}

static bool noEmptyInRange(const FakeFillerGrid& g, int row, int c0, int c1)
{
  for (int i = c0; i < c1; ++i) {
    if (g.rows[row][i].kind == SiteKind::Empty) {
      return false;
    }
  }
  return true;
}

// =============================================================================
int main()
{
  // --- T1: exact-fill 9 -> 4+3+2, must NOT pick 8 (orphans 1, no 1-filler) ---
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L", 2).dirty("X", 9).cell("L", 2).sites);
    FillerRepair fr(
        {F(8, "L"), F(4, "L"), F(3, "L"), F(2, "L")}, false, Rules{});
    RepairResult r = fr.repair(g);
    check(r.unsolved.empty(), "T1 solvable");
    check(noDirty(g), "T1 dirty removed");
    check(noEmptyInRange(g, 0, 2, 11), "T1 window fully filled");
    int sum = 0;
    bool any8 = false;
    for (const auto& p : r.placed) {
      sum += p.width;
      any8 |= (p.width == 8);
    }
    check(sum == 9, "T1 widths sum to 9");
    check(!any8, "T1 must not use the 8-site filler");
  }

  // --- T2: unsolvable 1-site window, no 1-site filler ---
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L", 3).dirty("L", 1).cell("L", 3).sites);
    FillerRepair fr({F(2, "L"), F(3, "L"), F(4, "L")}, false, Rules{});
    RepairResult r = fr.repair(g);
    check(r.unsolved.size() == 1, "T2 one unsolved window");
    check(r.placed.empty(), "T2 nothing placed");
    check(noDirty(g), "T2 dirty still removed");
  }

  // --- T3: preserveUserOrder changes the chosen combination ---
  {
    auto run = [](bool preserve) {
      FakeFillerGrid g;
      g.addRow(RB().cell("L", 2).dirty("X", 6).cell("L", 2).sites);
      FillerRepair fr({F(2, "L"), F(3, "L"), F(4, "L")}, preserve, Rules{});
      RepairResult r = fr.repair(g);
      return placedWidths(r);
    };
    check((run(true) == std::vector<int>{2, 2, 2}),
          "T3 preserveUserOrder -> 2+2+2");
    check((run(false) == std::vector<int>{2, 4}),
          "T3 default widest-first -> 4+2");
  }

  // --- T4: VT continuity split (L | H) forced ---
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L", 2).dirty("X", 5).cell("H", 2).sites);
    FillerRepair fr({F(2, "L"), F(3, "H")}, false, Rules{/*min_w=*/1});
    RepairResult r = fr.repair(g);
    check(r.unsolved.empty(), "T4 forced split solvable");
    check(g.rows[0][2].vt == "L" && g.rows[0][3].vt == "L", "T4 left is L");
    check(g.rows[0][4].vt == "H" && g.rows[0][5].vt == "H"
              && g.rows[0][6].vt == "H",
          "T4 right is H");
  }

  // --- T4b: min-width binds on an isolated narrow window ---
  {
    FakeFillerGrid g;
    g.addRow(RB().empty(1).dirty("L", 2).empty(1).sites);
    FillerRepair fr({F(2, "L")}, false, Rules{/*min_w=*/3});
    RepairResult r = fr.repair(g);
    check(r.unsolved.size() == 1, "T4b min-width makes narrow window unsolved");
  }

  // --- T6: don't touch clean filler; it is a fixed boundary ---
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L", 2).clean("L", 2).dirty("X", 4).cell("L", 2).sites);
    FillerRepair fr({F(2, "L"), F(4, "L")}, false, Rules{});
    RepairResult r = fr.repair(g);
    check(r.unsolved.empty(), "T6 solvable");
    check(g.rows[0][2].kind == SiteKind::CleanFiller
              && g.rows[0][3].kind == SiteKind::CleanFiller,
          "T6 clean filler untouched");
    for (const auto& p : r.placed) {
      check(p.col >= 4, "T6 placements only inside dirty window");
    }
  }

  // --- T7: two independent dirty windows in one row ---
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L", 2)
                 .dirty("X", 4)
                 .cell("L", 2)
                 .dirty("X", 6)
                 .cell("L", 2)
                 .sites);
    FillerRepair fr({F(2, "L"), F(4, "L"), F(6, "L")}, false, Rules{});
    RepairResult r = fr.repair(g);
    check(r.unsolved.empty(), "T7 both windows solvable");
    check(noDirty(g), "T7 all dirty removed");
  }

  // --- T8: clean design (no dirty) is a no-op ---
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L", 2).clean("L", 4).cell("L", 2).sites);
    FillerRepair fr({F(2, "L"), F(4, "L")}, false, Rules{});
    RepairResult r = fr.repair(g);
    check(r.placed.empty() && r.unsolved.empty(), "T8 no-op on clean design");
  }

  std::cout << "FillerRepair test: " << g_pass << " checks passed, " << g_fail
            << " failed.\n";
  return g_fail == 0 ? 0 : 1;
}
