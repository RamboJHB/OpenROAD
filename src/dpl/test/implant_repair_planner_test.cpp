// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Standalone (no-OpenROAD-deps) unit test for ImplantRepairPlanner.
//
// It builds small "implant layer" toy layouts as ASCII art and checks both the
// detector and the read-only repair planner. The cases are chosen to cover
// EVERY violation family and EVERY classification outcome:
//
//   - clean layout (no violations)
//   - intra-row min-width     : repairable, and unrepairable (no whitespace,
//                               blockage, and below-min-filler)
//   - min implant area        : repairable
//   - intra-row min-spacing   : repairable by merge, gap occupied (remaining),
//                               gap below MF (remaining)
//   - inter-row min-width     : repairable (widen staircase overlap)
//   - inter-row min-spacing   : remaining (needs cell movement)
//
// Build & run (standalone, no OpenROAD):
//   g++ -std=c++17 -I src/dpl/src src/dpl/src/ImplantRepairPlanner.cpp
//       src/dpl/test/implant_repair_planner_test.cpp -o /tmp/irp_test
//   /tmp/irp_test

#include <iostream>
#include <string>
#include <vector>

#include "ImplantRepairPlanner.h"

using namespace dpl;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                  \
  do {                                                    \
    if (cond) {                                           \
      ++g_pass;                                           \
    } else {                                              \
      ++g_fail;                                           \
      std::cout << "  FAIL: " << (msg) << "\n";           \
    }                                                     \
  } while (0)

// Build a Layout from ASCII rows. row 0 == first string.
//   '.' empty, '#' blockage, 'A'..'Z' a cell carrying implant id (ch-'A').
static Layout make(const std::vector<std::string>& rows)
{
  const int R = static_cast<int>(rows.size());
  const int C = static_cast<int>(rows[0].size());
  Layout l(R, C);
  for (int r = 0; r < R; ++r) {
    for (int c = 0; c < C; ++c) {
      const char ch = rows[r][c];
      if (ch == '.') {
        l.at(r, c) = {SiteKind::Empty, kNoImplant};
      } else if (ch == '#') {
        l.at(r, c) = {SiteKind::Blocked, kNoImplant};
      } else {
        l.at(r, c) = {SiteKind::Cell, ch - 'A'};
      }
    }
  }
  return l;
}

static std::vector<Filler> fillers(ImplantId imp, std::vector<int> widths)
{
  std::vector<Filler> f;
  for (int w : widths) {
    f.push_back({imp, w});
  }
  return f;
}

static int countType(const std::vector<Violation>& vs, ViolType t)
{
  int n = 0;
  for (const auto& v : vs) {
    n += (v.type == t);
  }
  return n;
}

static int suggestedSites(const RepairPlan& p)
{
  int n = 0;
  for (const auto& s : p.suggestions) {
    n += s.width;
  }
  return n;
}

int main()
{
  // ---- 1. clean layout: no violations -------------------------------------
  {
    Rules r;
    r.intra_mw = 3; r.inter_mw = 2; r.intra_ms = 2; r.inter_ms = 2;
    r.min_area = 4; r.min_filler = 1;
    ImplantRepairPlanner p(r, fillers(0, {1, 2}));
    auto v = p.detect(make({"AAAA", "AAAA"}));
    CHECK(v.empty(), "clean layout should have no violations");
  }

  // ---- 2. intra-row min-width: REPAIRABLE ---------------------------------
  {
    Rules r; r.intra_mw = 3; r.min_area = 1;
    ImplantRepairPlanner p(r, fillers(0, {1, 2}));
    Layout l = make({"AA..."});                 // run width 2 < 3, empties right
    auto v = p.detect(l);
    CHECK(countType(v, ViolType::IntraMW) == 1, "case2 detect 1 IntraMW");
    auto plan = p.plan(l);
    CHECK(plan.repaired.size() == 1 && plan.remaining.empty(),
          "case2 IntraMW repairable");
    CHECK(suggestedSites(plan) == 1, "case2 fills exactly 1 site");
  }

  // ---- 2b. intra-row min-width: UNREPAIRABLE (no whitespace) ---------------
  {
    Rules r; r.intra_mw = 3; r.min_area = 1;
    ImplantRepairPlanner p(r, fillers(0, {1, 2}));
    Layout l = make({"AA"});                    // width 2 < 3, no empty sites
    auto plan = p.plan(l);
    CHECK(plan.repaired.empty() && plan.remaining.size() == 1,
          "case2b IntraMW unrepairable (no whitespace)");
    CHECK(plan.remaining[0].reason.find("whitespace") != std::string::npos,
          "case2b reason mentions whitespace");
  }

  // ---- 2c. intra-row min-width: UNREPAIRABLE (blockage) --------------------
  {
    Rules r; r.intra_mw = 3; r.min_area = 1;
    ImplantRepairPlanner p(r, fillers(0, {1, 2}));
    Layout l = make({"AA#"});                   // gap blocked, not empty
    auto plan = p.plan(l);
    CHECK(plan.remaining.size() == 1, "case2c IntraMW blocked -> remaining");
  }

  // ---- 2d. intra-row min-width: UNREPAIRABLE (below min filler) ------------
  {
    Rules r; r.intra_mw = 3; r.min_area = 1; r.min_filler = 2;
    ImplantRepairPlanner p(r, fillers(0, {2}));  // no width-1 filler
    Layout l = make({"AA."});                    // need 1 site, but MF = 2
    auto plan = p.plan(l);
    CHECK(plan.remaining.size() == 1
              && plan.remaining[0].reason.find("tile") != std::string::npos,
          "case2d IntraMW below-MF -> remaining (cannot tile)");
  }

  // ---- 3. min implant area: REPAIRABLE ------------------------------------
  {
    Rules r; r.intra_mw = 1; r.min_area = 4;
    ImplantRepairPlanner p(r, fillers(0, {1, 2}));
    Layout l = make({"A...."});                 // area 1 < 4, grow by 3
    auto v = p.detect(l);
    CHECK(countType(v, ViolType::MinArea) == 1, "case3 detect 1 MinArea");
    auto plan = p.plan(l);
    CHECK(plan.repaired.size() == 1 && plan.remaining.empty(),
          "case3 MinArea repairable");
    CHECK(suggestedSites(plan) == 3, "case3 fills 3 sites to reach area 4");
  }

  // ---- 4. intra-row min-spacing: REPAIRABLE by merge ----------------------
  {
    Rules r; r.intra_mw = 1; r.intra_ms = 3; r.min_area = 1;
    ImplantRepairPlanner p(r, fillers(0, {1, 2}));
    Layout l = make({"A..A"});                  // gap 2 < 3, empty -> merge
    auto v = p.detect(l);
    CHECK(countType(v, ViolType::IntraMS) == 1, "case4 detect 1 IntraMS");
    auto plan = p.plan(l);
    CHECK(plan.repaired.size() == 1 && plan.remaining.empty(),
          "case4 IntraMS repairable by merge");
    CHECK(suggestedSites(plan) == 2, "case4 fills the 2-site gap");
  }

  // ---- 4b. intra-row min-spacing: gap occupied -> REMAINING ----------------
  {
    Rules r; r.intra_mw = 1; r.intra_ms = 2; r.min_area = 1;
    ImplantRepairPlanner p(r, fillers(0, {1}));
    Layout l = make({"ABA"});                   // A..A separated by a B cell
    auto v = p.detect(l);
    CHECK(countType(v, ViolType::IntraMS) == 1, "case4b detect 1 IntraMS (A)");
    auto plan = p.plan(l);
    CHECK(plan.remaining.size() == 1
              && plan.remaining[0].reason.find("movement") != std::string::npos,
          "case4b gap occupied -> remaining (needs movement)");
  }

  // ---- 4c. intra-row min-spacing: gap below MF -> REMAINING ----------------
  {
    Rules r; r.intra_mw = 1; r.intra_ms = 2; r.min_area = 1; r.min_filler = 2;
    ImplantRepairPlanner p(r, fillers(0, {2, 3}));  // no width-1 filler
    Layout l = make({"A.A"});                   // gap 1 < 2, but MF = 2
    auto plan = p.plan(l);
    CHECK(plan.remaining.size() == 1, "case4c gap below MF -> remaining");
  }

  // ---- 5. inter-row min-width: REPAIRABLE (widen staircase) ---------------
  {
    Rules r; r.intra_mw = 1; r.inter_mw = 2; r.min_area = 1;
    ImplantRepairPlanner p(r, fillers(0, {1}));
    Layout l = make({"AAA", "A.."});            // overlap 1 < 2, widen by 1
    auto v = p.detect(l);
    CHECK(countType(v, ViolType::InterMW) == 1, "case5 detect 1 InterMW");
    auto plan = p.plan(l);
    CHECK(plan.repaired.size() == 1 && plan.remaining.empty(),
          "case5 InterMW repairable");
    CHECK(suggestedSites(plan) == 1, "case5 fills 1 site to widen overlap");
  }

  // ---- 6. inter-row min-spacing: REMAINING (needs movement) ---------------
  {
    Rules r; r.intra_mw = 1; r.inter_ms = 2; r.min_area = 1;
    ImplantRepairPlanner p(r, fillers(0, {1}));
    Layout l = make({"A..", "..A"});            // adjacent-row A's, dist 1 < 2
    auto v = p.detect(l);
    CHECK(countType(v, ViolType::InterMS) == 1, "case6 detect 1 InterMS");
    auto plan = p.plan(l);
    CHECK(plan.repaired.empty() && plan.remaining.size() == 1,
          "case6 InterMS -> remaining");
    CHECK(plan.remaining[0].reason.find("movement") != std::string::npos,
          "case6 reason mentions movement");
  }

  std::cout << "\nImplantRepairPlanner test: " << g_pass << " checks passed, "
            << g_fail << " failed.\n";
  return g_fail == 0 ? 0 : 1;
}
