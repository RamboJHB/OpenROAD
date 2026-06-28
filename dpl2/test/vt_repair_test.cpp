// Stand-alone test for the weight-based VT repair (VtRepair).
//   g++ -std=c++17 -I dpl2/src dpl2/src/VtRepair.cpp \
//       dpl2/test/vt_repair_test.cpp -o /tmp/vtr && /tmp/vtr
#include <iostream>
#include <string>

#include "FakeDesign.h"
#include "VtRepair.h"

using namespace vtrepair;

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

int main()
{
  // ---- T1: island H surrounded by L fillers -> relabel island to L ----
  {
    FakeDesign d;
    d.resize(3, 3);
    FillerId island = NO_FILLER;
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        const Vt vt = (r == 1 && c == 1) ? "H" : "L";
        FillerId id = d.addFiller(r, c, 1, 1, vt);
        if (r == 1 && c == 1) {
          island = id;
        }
      }
    }
    d.addMaster("L", 1, 1);
    d.addMaster("H", 1, 1);
    d.addViolation(1, 1, 1, "MW");
    RunResult res = VtRepair().run(d, /*verbose=*/false);
    check(res.fixed == 1 && res.unfixable == 0, "T1 fixed island");
    check(d.filler(island).vt == "L", "T1 island H->L");
  }

  // ---- T2: 4-neighbour coverage: anchor is a clean L filler, the real island
  //          H is a neighbour -> the neighbour gets relabeled ----
  {
    FakeDesign d;
    d.resize(3, 4);
    FillerId island = NO_FILLER;
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 4; ++c) {
        const Vt vt = (r == 1 && c == 2) ? "H" : "L";
        FillerId id = d.addFiller(r, c, 1, 1, vt);
        if (r == 1 && c == 2) {
          island = id;
        }
      }
    }
    d.addMaster("L", 1, 1);
    d.addMaster("H", 1, 1);
    d.addViolation(2, 1, 1, "MW");  // anchored next to the island, not on it
    RunResult res = VtRepair().run(d, false);
    check(res.fixed == 1, "T2 fixed via neighbour candidate");
    check(d.filler(island).vt == "L", "T2 neighbour island H->L");
  }

  // ---- T3: filler between two fixed cells -> touches cell -> UNFIXABLE ----
  {
    FakeDesign d;
    d.resize(1, 3);
    d.addCell(0, 0, 1, 1, "L");
    FillerId f = d.addFiller(0, 1, 1, 1, "H");
    d.addCell(0, 2, 1, 1, "L");
    d.addMaster("L", 1, 1);
    d.addMaster("H", 1, 1);
    d.addViolation(3, 0, 1, "MS");
    RunResult res = VtRepair().run(d, false);
    check(res.unfixable == 1 && res.fixed == 0, "T3 unfixable (cell-bounded)");
    check(d.filler(f).vt == "H", "T3 filler untouched");
    check(!d.unfixable().empty()
              && d.unfixable()[0].second.find("cell") != std::string::npos,
          "T3 reason mentions cell");
  }

  // ---- T4: island but library lacks the needed same-size master -> UNFIXABLE
  {
    FakeDesign d;
    d.resize(3, 3);
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        d.addFiller(r, c, 1, 1, (r == 1 && c == 1) ? "H" : "L");
      }
    }
    // No (1,1) master in ANY VT -> no candidate has a realizable target.
    d.addViolation(4, 1, 1, "MW");
    RunResult res = VtRepair().run(d, false);
    check(res.unfixable == 1, "T4 unfixable (no same-size master)");
    check(!d.unfixable().empty()
              && d.unfixable()[0].second.find("master") != std::string::npos,
          "T4 reason mentions master");
  }

  // ---- T5: weighCandidate sanity (island vs cell-touch) ----
  {
    FakeDesign d;
    d.resize(3, 3);
    FillerId island = NO_FILLER;
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        FillerId id = d.addFiller(r, c, 1, 1, (r == 1 && c == 1) ? "H" : "L");
        if (r == 1 && c == 1) {
          island = id;
        }
      }
    }
    d.addMaster("L", 1, 1);
    CandidateWeight cw = VtRepair::weighCandidate(d, island);
    check(cw.weight == VtRepair::kIslandWeight, "T5 island -> max weight");
    check(cw.has_target && cw.target == "L", "T5 island target L");

    FakeDesign d2;
    d2.resize(1, 3);
    d2.addCell(0, 0, 1, 1, "L");
    FillerId f = d2.addFiller(0, 1, 1, 1, "H");
    d2.addCell(0, 2, 1, 1, "L");
    CandidateWeight cw2 = VtRepair::weighCandidate(d2, f);
    check(cw2.weight == 0 && cw2.touches_cell, "T5 cell-touch -> weight 0");
  }

  std::cout << "VtRepair test: " << g_pass << " passed, " << g_fail
            << " failed.\n";
  return g_fail == 0 ? 0 : 1;
}
