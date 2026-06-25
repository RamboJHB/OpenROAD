// Stand-alone test for FillerVtRepair (inter-row MW/MS via VT replacement).
//   g++ -std=c++17 -I dpl2/src dpl2/src/FillerVtRepair.cpp \
//       dpl2/test/filler_vt_repair_test.cpp -o /tmp/vt && /tmp/vt
#include <iostream>
#include <string>
#include <vector>

#include "FakeFillerGrid.h"
#include "FillerVtRepair.h"

using namespace dpl_fr;

static int g_pass = 0, g_fail = 0;
static void check(bool c, const std::string& m)
{
  if (c) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cout << "  FAIL: " << m << "\n";
  }
}

struct RB
{
  std::vector<FakeSite> s;
  RB& cell(const Vt& v) { return add(SiteKind::Cell, v); }
  RB& fill(const Vt& v) { return add(SiteKind::CleanFiller, v); }
  RB& empty() { return add(SiteKind::Empty, VT_NONE); }
  RB& add(SiteKind k, const Vt& v)
  {
    s.push_back({k, v});
    return *this;
  }
};

static Filler F(int w, const Vt& v)
{
  return {w, 1, v, v + std::to_string(w)};
}

int main()
{
  // Library: 1- and 2-site fillers for L and H (so runs are tileable).
  std::vector<Filler> lib = {F(1, "L"), F(2, "L"), F(1, "H"), F(2, "H")};

  // ---- T1: MW+MS staircase, fillers between fixed L cells -> fully fixed ----
  // row0: cellL  fillerH        row1: fillerH  cellL
  // The two fillers (wrong VT H) make a diagonal: necks (MW) + L-H touches
  // (MS).
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L").fill("H").s);
    g.addRow(RB().fill("H").cell("L").s);
    FillerVtRepair fr(lib, VtRules{/*min_width=*/2, /*min_spacing=*/1});
    VtRepairResult r = fr.repair(g);
    check(r.violations_before > 0, "T1 has violations before");
    check(r.violations_after == 0, "T1 fully fixed (0 after)");
    // both fillers became L
    check(g.rows[0][1].vt == "L" && g.rows[1][0].vt == "L",
          "T1 fillers replaced H->L");
    check(g.rows[0][0].vt == "L" && g.rows[1][1].vt == "L",
          "T1 cells untouched (still L)");
  }

  // ---- T2: inter-row MS, two filler rows L over H, bounded by L cells -------
  // row0: cellL fillerL fillerL cellL    row1: cellL fillerH fillerH cellL
  // L over H touches vertically -> MS; fix by making row1 fillers L (merge).
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L").fill("L").fill("L").cell("L").s);
    g.addRow(RB().cell("L").fill("H").fill("H").cell("L").s);
    FillerVtRepair fr(lib, VtRules{/*min_width=*/2, /*min_spacing=*/1});
    VtRepairResult r = fr.repair(g);
    check(r.violations_before > 0, "T2 MS before");
    check(r.violations_after == 0, "T2 MS fixed");
    check(g.rows[1][1].vt == "L" && g.rows[1][2].vt == "L",
          "T2 row1 fillers H->L (merge)");
  }

  // ---- T3: unfixable because library lacks the needed VT -> residual --------
  // Same staircase as T1 but lib has only H fillers (can't make L).
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L").fill("H").s);
    g.addRow(RB().fill("H").cell("L").s);
    FillerVtRepair fr({F(1, "H"), F(2, "H")},
                      VtRules{/*min_width=*/2, /*min_spacing=*/1});
    VtRepairResult r = fr.repair(g);
    check(r.violations_after > 0, "T3 stays violating (no L master)");
    check(r.replaced.empty(), "T3 nothing replaced");
  }

  // ---- T4: already clean -> no change ----
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L").fill("L").fill("L").cell("L").s);
    g.addRow(RB().cell("L").fill("L").fill("L").cell("L").s);
    FillerVtRepair fr(lib, VtRules{2, 1});
    VtRepairResult r = fr.repair(g);
    check(r.violations_before == 0 && r.violations_after == 0,
          "T4 clean no-op");
    check(r.replaced.empty(), "T4 nothing replaced");
  }

  // ---- T5: evaluator sanity (static) ----
  {
    // L over H, 1x2 each, touching -> 2 MS (down neighbors) + MW necks.
    std::vector<std::vector<Vt>> vt = {{"L", "L"}, {"H", "H"}};
    std::vector<std::vector<char>> pr = {{1, 1}, {1, 1}};
    int v = FillerVtRepair::countViolations(vt, pr, VtRules{2, 1});
    check(v > 0, "T5 evaluator flags L-over-H");
    std::vector<std::vector<Vt>> vt2 = {{"L", "L"}, {"L", "L"}};
    int v2 = FillerVtRepair::countViolations(vt2, pr, VtRules{2, 1});
    check(v2 == 0, "T5 evaluator clean on solid L block");
  }

  std::cout << "FillerVtRepair test: " << g_pass << " passed, " << g_fail
            << " failed.\n";
  return g_fail == 0 ? 0 : 1;
}
