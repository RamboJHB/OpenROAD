// Stand-alone test for the checker-oracle path (FillerVtRepairOracle driving an
// ImplantChecker).  Uses FakeImplantChecker as the in-memory oracle.
//   g++ -std=c++17 -I dpl2/src dpl2/src/FillerVtRepair.cpp \
//       dpl2/src/FillerVtRepairOracle.cpp \
//       dpl2/test/filler_vt_repair_oracle_test.cpp -o /tmp/orc && /tmp/orc
#include <iostream>
#include <string>
#include <vector>

#include "FakeImplantChecker.h"
#include "FillerVtRepairOracle.h"

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
  std::vector<Filler> lib = {F(1, "L"), F(2, "L"), F(1, "H"), F(2, "H")};

  // ---- O1: staircase MW+MS, via the oracle -> fully fixed ----
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L").fill("H").s);
    g.addRow(RB().fill("H").cell("L").s);
    FakeImplantChecker chk(g, lib, VtRules{/*mw=*/2, /*ms=*/1});
    FillerVtRepairOracle oracle;
    OracleRepairResult r = oracle.repair(chk);
    check(r.violations_before > 0, "O1 has violations before");
    check(r.violations_after == 0, "O1 fully fixed via oracle");
    check(r.replaced_runs > 0, "O1 replaced at least one run");
    check(chk.grid().rows[0][1].vt == "L" && chk.grid().rows[1][0].vt == "L",
          "O1 fillers H->L");
    check(chk.grid().rows[0][0].vt == "L" && chk.grid().rows[1][1].vt == "L",
          "O1 cells untouched");
  }

  // ---- O2: inter-row MS merge, via the oracle ----
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L").fill("L").fill("L").cell("L").s);
    g.addRow(RB().cell("L").fill("H").fill("H").cell("L").s);
    FakeImplantChecker chk(g, lib, VtRules{2, 1});
    FillerVtRepairOracle oracle;
    OracleRepairResult r = oracle.repair(chk);
    check(r.violations_before > 0, "O2 MS before");
    check(r.violations_after == 0, "O2 MS fixed via oracle");
    check(chk.grid().rows[1][1].vt == "L" && chk.grid().rows[1][2].vt == "L",
          "O2 row1 fillers H->L");
  }

  // ---- O3: library lacks needed VT -> residual, nothing replaced ----
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L").fill("H").s);
    g.addRow(RB().fill("H").cell("L").s);
    FakeImplantChecker chk({g}, {F(1, "H"), F(2, "H")}, VtRules{2, 1});
    FillerVtRepairOracle oracle;
    OracleRepairResult r = oracle.repair(chk);
    check(r.violations_after > 0, "O3 stays violating (no L master)");
    check(r.replaced_runs == 0, "O3 nothing replaced");
  }

  // ---- O4: already clean -> no-op ----
  {
    FakeFillerGrid g;
    g.addRow(RB().cell("L").fill("L").fill("L").cell("L").s);
    g.addRow(RB().cell("L").fill("L").fill("L").cell("L").s);
    FakeImplantChecker chk(g, lib, VtRules{2, 1});
    FillerVtRepairOracle oracle;
    OracleRepairResult r = oracle.repair(chk);
    check(r.violations_before == 0 && r.violations_after == 0,
          "O4 clean no-op");
    check(r.replaced_runs == 0, "O4 nothing replaced");
  }

  std::cout << "FillerVtRepairOracle test: " << g_pass << " passed, " << g_fail
            << " failed.\n";
  return g_fail == 0 ? 0 : 1;
}
