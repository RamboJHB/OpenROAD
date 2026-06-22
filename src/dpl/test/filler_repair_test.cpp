// Stand-alone unit test for FillerRepair (no ODB/dpl needed).
//
// Build & run (sandbox-friendly):
//   g++ -std=c++17 -I src/dpl/src
//       src/dpl/src/FillerRepair.cpp src/dpl/test/filler_repair_test.cpp
//       -o /tmp/fr_test && /tmp/fr_test
//
// DRC is intentionally skipped: each test feeds a site-grid whose violating
// fillers are already marked DIRTY.
#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

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

// ---- tiny grid builder ------------------------------------------------------
struct RB
{
  Row row;
  explicit RB(int h = 1) { row.height = h; }
  RB& add(SiteKind k, const Vt& vt, int w)
  {
    for (int i = 0; i < w; ++i) {
      row.sites.push_back(Site{k, vt});
    }
    return *this;
  }
  RB& cell(const Vt& vt, int w) { return add(SiteKind::Cell, vt, w); }
  RB& clean(const Vt& vt, int w) { return add(SiteKind::CleanFiller, vt, w); }
  RB& dirty(const Vt& vt, int w) { return add(SiteKind::DirtyFiller, vt, w); }
  RB& empty(int w) { return add(SiteKind::Empty, VT_NONE, w); }
  RB& blocked(int w) { return add(SiteKind::Blocked, VT_NONE, w); }
};

static Filler F(int width, const Vt& vt, int height = 1)
{
  return Filler{width,
                height,
                vt,
                vt + std::to_string(width) + "h" + std::to_string(height)};
}

// counts of filler widths placed, by vt
static std::map<Vt, std::vector<int>> widthsByVt(const RepairResult& r)
{
  std::map<Vt, std::vector<int>> m;
  for (const auto& p : r.placed) {
    m[p.vt].push_back(p.width);
  }
  for (auto& kv : m) {
    std::sort(kv.second.begin(), kv.second.end());
  }
  return m;
}

static bool noDirty(const Layout& l)
{
  for (const auto& row : l.rows) {
    for (const auto& s : row.sites) {
      if (s.kind == SiteKind::DirtyFiller) {
        return false;
      }
    }
  }
  return true;
}

static bool noEmptyInRange(const Row& row, int c0, int c1)
{
  for (int i = c0; i < c1; ++i) {
    if (row.sites[i].kind == SiteKind::Empty) {
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
    Layout lay;
    lay.rows.push_back(RB().cell("L", 2).dirty("X", 9).cell("L", 2).row);
    FillerRepair fr({F(8, "L"), F(4, "L"), F(3, "L"), F(2, "L")},
                    /*preserve=*/false,
                    Rules{});
    RepairResult r = fr.repair(lay);
    check(r.unsolved.empty(), "T1 should be solvable");
    check(noDirty(lay), "T1 dirty removed");
    check(noEmptyInRange(lay.rows[0], 2, 11), "T1 window fully filled");
    auto w = widthsByVt(r)["L"];
    int sum = 0;
    bool any8 = false;
    for (int x : w) {
      sum += x;
      if (x == 8) {
        any8 = true;
      }
    }
    check(sum == 9, "T1 widths sum to 9");
    check(!any8, "T1 must not use the 8-site filler (would orphan 1 site)");
  }

  // --- T2: unsolvable 1-site window, no 1-site filler ---
  {
    Layout lay;
    lay.rows.push_back(RB().cell("L", 3).dirty("L", 1).cell("L", 3).row);
    FillerRepair fr({F(2, "L"), F(3, "L"), F(4, "L")}, false, Rules{});
    RepairResult r = fr.repair(lay);
    check(r.unsolved.size() == 1, "T2 one unsolved window");
    check(r.placed.empty(), "T2 nothing placed");
    check(noDirty(lay), "T2 dirty still removed");
  }

  // --- T3: preserveUserOrder changes the chosen combination ---
  {
    // width-6 window, fillers {2,3,4}
    auto run = [](bool preserve) {
      Layout lay;
      lay.rows.push_back(RB().cell("L", 2).dirty("X", 6).cell("L", 2).row);
      FillerRepair fr({F(2, "L"), F(3, "L"), F(4, "L")}, preserve, Rules{});
      RepairResult r = fr.repair(lay);
      return widthsByVt(r)["L"];
    };
    auto pres = run(true);   // tries 2 first -> 2+2+2
    auto desc = run(false);  // widest-first -> 4+2
    check((pres == std::vector<int>{2, 2, 2}), "T3 preserveUserOrder -> 2+2+2");
    check((desc == std::vector<int>{2, 4}), "T3 default widest-first -> 4+2");
  }

  // --- T4: VT continuity split (L | H) is FORCED ---
  {
    // width-5 window; L lib={2}, H lib={3} -> neither VT alone fits 5, so the
    // solver must split: L(2) | H(3), each merging into its own neighbor.
    Layout lay;
    lay.rows.push_back(RB().cell("L", 2).dirty("X", 5).cell("H", 2).row);
    FillerRepair fr({F(2, "L"), F(3, "H")}, false, Rules{/*min_w=*/1});
    RepairResult r = fr.repair(lay);
    check(r.unsolved.empty(), "T4 forced split solvable");
    check(lay.rows[0].sites[2].vt == "L" && lay.rows[0].sites[3].vt == "L",
          "T4 left 2 sites are L");
    check(lay.rows[0].sites[4].vt == "H" && lay.rows[0].sites[5].vt == "H"
              && lay.rows[0].sites[6].vt == "H",
          "T4 right 3 sites are H");
  }

  // --- T4b: min-width binds on an isolated (no same-VT neighbor) window ---
  {
    // window between two EMPTY sides -> stand-alone strip; width 2 < min 3.
    Layout lay;
    lay.rows.push_back(RB().empty(1).dirty("L", 2).empty(1).row);
    FillerRepair fr({F(2, "L")}, false, Rules{/*min_w=*/3});
    RepairResult r = fr.repair(lay);
    check(r.unsolved.size() == 1,
          "T4b min-width makes isolated narrow window unsolved");
  }

  // --- T5: multi-height, fillers must be height-matched ---
  {
    Layout lay;
    lay.rows.push_back(RB(1).cell("L", 2).dirty("X", 4).cell("L", 2).row);
    lay.rows.push_back(RB(2).cell("L", 2).dirty("X", 4).cell("L", 2).row);
    // h1 lib: a 4; h2 lib: 2+2.  Cross-height use must be impossible.
    FillerRepair fr({F(4, "L", 1), F(2, "L", 2)}, false, Rules{});
    RepairResult r = fr.repair(lay);
    check(r.unsolved.empty(), "T5 both rows solvable");
    // row0 (h1) filled by one width-4; row1 (h2) by two width-2
    std::vector<int> r0, r1;
    for (const auto& p : r.placed) {
      (p.row == 0 ? r0 : r1).push_back(p.width);
    }
    std::sort(r0.begin(), r0.end());
    std::sort(r1.begin(), r1.end());
    check((r0 == std::vector<int>{4}), "T5 h1 row uses height-1 filler (4)");
    check((r1 == std::vector<int>{2, 2}), "T5 h2 row uses height-2 fillers");
  }

  // --- T6: don't touch clean filler; it acts as a fixed boundary ---
  {
    Layout lay;
    // L cell | clean L (2) | dirty (4) | L cell
    lay.rows.push_back(
        RB().cell("L", 2).clean("L", 2).dirty("X", 4).cell("L", 2).row);
    FillerRepair fr({F(2, "L"), F(4, "L")}, false, Rules{});
    RepairResult r = fr.repair(lay);
    check(r.unsolved.empty(), "T6 solvable");
    // clean sites [2,4) stay CleanFiller and were not re-placed
    check(lay.rows[0].sites[2].kind == SiteKind::CleanFiller
              && lay.rows[0].sites[3].kind == SiteKind::CleanFiller,
          "T6 clean filler untouched");
    for (const auto& p : r.placed) {
      check(p.col >= 4, "T6 placements only inside dirty window (col>=4)");
    }
  }

  // --- T7: two independent dirty windows in one row ---
  {
    Layout lay;
    lay.rows.push_back(RB().cell("L", 2)
                           .dirty("X", 4)
                           .cell("L", 2)
                           .dirty("X", 6)
                           .cell("L", 2)
                           .row);
    FillerRepair fr({F(2, "L"), F(4, "L"), F(6, "L")}, false, Rules{});
    RepairResult r = fr.repair(lay);
    check(r.unsolved.empty(), "T7 both windows solvable");
    check(noDirty(lay), "T7 all dirty removed");
    check(noEmptyInRange(lay.rows[0], 2, 6), "T7 window-1 filled");
    check(noEmptyInRange(lay.rows[0], 8, 14), "T7 window-2 filled");
  }

  // --- T8: clean design (no dirty) is a no-op ---
  {
    Layout lay;
    lay.rows.push_back(RB().cell("L", 2).clean("L", 4).cell("L", 2).row);
    FillerRepair fr({F(2, "L"), F(4, "L")}, false, Rules{});
    RepairResult r = fr.repair(lay);
    check(r.placed.empty() && r.unsolved.empty(), "T8 no-op on clean design");
  }

  std::cout << "FillerRepair test: " << g_pass << " checks passed, " << g_fail
            << " failed.\n";
  return g_fail == 0 ? 0 : 1;
}
