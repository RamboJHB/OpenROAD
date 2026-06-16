// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// Implementation of the read-only implant-DRC filler repair planner.
// See ImplantRepairPlanner.h for the model and the end-to-end logic chain.

#include "ImplantRepairPlanner.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dpl {

const char* toString(ViolType t)
{
  switch (t) {
    case ViolType::IntraMW:   return "intra-row-min-width";
    case ViolType::InterMW:   return "inter-row-min-width";
    case ViolType::IntraMS:   return "intra-row-min-spacing";
    case ViolType::InterMS:   return "inter-row-min-spacing";
    case ViolType::MinArea:   return "min-implant-area";
    case ViolType::MinFiller: return "min-filler-width";
  }
  return "?";
}

namespace {

// A maximal run of same-implant CELL sites inside one row: columns [lo, hi).
struct Run
{
  ImplantId implant;
  int lo;
  int hi;
  int width() const { return hi - lo; }
};

// Split one row into its same-implant runs. EMPTY/BLOCKED sites and any change
// of implant id terminate a run. This is the atomic unit every detector and
// repair works on.
std::vector<Run> rowRuns(const Layout& layout, int row)
{
  std::vector<Run> runs;
  int c = 0;
  while (c < layout.num_sites) {
    const Site& s = layout.at(row, c);
    if (s.kind != SiteKind::Cell) {
      ++c;
      continue;
    }
    int lo = c;
    ImplantId imp = s.implant;
    while (c < layout.num_sites && layout.at(row, c).kind == SiteKind::Cell
           && layout.at(row, c).implant == imp) {
      ++c;
    }
    runs.push_back({imp, lo, c});
  }
  return runs;
}

std::string key(const Violation& v)
{
  return std::to_string(static_cast<int>(v.type)) + "|"
         + std::to_string(v.implant) + "|" + std::to_string(v.row) + "|"
         + std::to_string(v.col_lo) + "|" + std::to_string(v.col_hi);
}

Violation makeViol(ViolType t, ImplantId imp, int row, int lo, int hi)
{
  Violation v;
  v.type = t;
  v.implant = imp;
  v.row = row;
  v.col_lo = lo;
  v.col_hi = hi;
  return v;
}

// Count EMPTY sites running right from column `from` (inclusive) in `row`.
int emptyRight(const Layout& l, int row, int from)
{
  int n = 0;
  for (int c = from; c < l.num_sites && l.at(row, c).kind == SiteKind::Empty;
       ++c) {
    ++n;
  }
  return n;
}

// Count EMPTY sites running left from column `from` (inclusive) in `row`.
int emptyLeft(const Layout& l, int row, int from)
{
  int n = 0;
  for (int c = from; c >= 0 && l.at(row, c).kind == SiteKind::Empty; --c) {
    ++n;
  }
  return n;
}

}  // namespace

ImplantRepairPlanner::ImplantRepairPlanner(Rules rules,
                                           std::vector<Filler> fillers)
    : rules_(rules), fillers_(std::move(fillers))
{
}

// ===========================================================================
// DETECTION
// ===========================================================================

// intra-row min width: a same-implant run narrower than intra_mw.
void ImplantRepairPlanner::detectIntraRowWidth(const Layout& l,
                                               std::vector<Violation>& out) const
{
  for (int r = 0; r < l.num_rows; ++r) {
    for (const Run& run : rowRuns(l, r)) {
      if (run.width() < rules_.intra_mw) {
        out.push_back(
            makeViol(ViolType::IntraMW, run.implant, r, run.lo, run.hi));
      }
    }
  }
}

// intra-row min spacing: two runs of the SAME implant in one row whose gap is
// smaller than intra_ms (the gap may be empty or filled by another implant).
void ImplantRepairPlanner::detectIntraRowSpacing(
    const Layout& l,
    std::vector<Violation>& out) const
{
  for (int r = 0; r < l.num_rows; ++r) {
    // group runs by implant, keep them in column order (rowRuns is ordered).
    std::map<ImplantId, std::vector<Run>> by_imp;
    for (const Run& run : rowRuns(l, r)) {
      by_imp[run.implant].push_back(run);
    }
    for (auto& [imp, runs] : by_imp) {
      for (size_t i = 1; i < runs.size(); ++i) {
        const int gap = runs[i].lo - runs[i - 1].hi;
        if (gap > 0 && gap < rules_.intra_ms) {
          out.push_back(makeViol(
              ViolType::IntraMS, imp, r, runs[i - 1].hi, runs[i].lo));
        }
      }
    }
  }
}

// inter-row coupling between every pair of adjacent rows.
//   * same-implant runs that share columns -> their abutting overlap is the
//     "staircase" connection; if it is thinner than inter_mw -> InterMW.
//   * same-implant runs that do NOT share columns -> their horizontal distance
//     is the inter-row spacing; if smaller than inter_ms -> InterMS.
void ImplantRepairPlanner::detectInterRow(const Layout& l,
                                          std::vector<Violation>& out) const
{
  for (int r = 0; r + 1 < l.num_rows; ++r) {
    const std::vector<Run> a = rowRuns(l, r);
    const std::vector<Run> b = rowRuns(l, r + 1);
    for (const Run& ra : a) {
      for (const Run& rb : b) {
        if (ra.implant != rb.implant) {
          continue;
        }
        const int olo = std::max(ra.lo, rb.lo);
        const int ohi = std::min(ra.hi, rb.hi);
        if (ohi > olo) {  // vertically abutting -> check staircase width
          if (ohi - olo < rules_.inter_mw) {
            out.push_back(
                makeViol(ViolType::InterMW, ra.implant, r, olo, ohi));
          }
        } else {  // disjoint columns -> check inter-row spacing
          int dist = 0;
          int glo = 0, ghi = 0;
          if (rb.lo >= ra.hi) {
            dist = rb.lo - ra.hi;
            glo = ra.hi;
            ghi = rb.lo;
          } else {  // ra.lo >= rb.hi
            dist = ra.lo - rb.hi;
            glo = rb.hi;
            ghi = ra.lo;
          }
          if (dist > 0 && dist < rules_.inter_ms) {
            out.push_back(
                makeViol(ViolType::InterMS, ra.implant, r, glo, ghi));
          }
        }
      }
    }
  }
}

// min implant area: union runs into connected same-implant regions (a run in
// row r connects to a run in row r+1 when same implant and they share >=1
// column), then flag any region whose total area < min_area.
void ImplantRepairPlanner::detectMinArea(const Layout& l,
                                         std::vector<Violation>& out) const
{
  // collect all runs with their row.
  struct RRun { int row; Run run; };
  std::vector<RRun> runs;
  std::vector<std::vector<int>> by_row(l.num_rows);  // indices per row
  for (int r = 0; r < l.num_rows; ++r) {
    for (const Run& run : rowRuns(l, r)) {
      by_row[r].push_back(static_cast<int>(runs.size()));
      runs.push_back({r, run});
    }
  }
  // union-find
  std::vector<int> parent(runs.size());
  for (size_t i = 0; i < parent.size(); ++i) {
    parent[i] = static_cast<int>(i);
  }
  std::function<int(int)> find = [&](int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  auto unite = [&](int x, int y) { parent[find(x)] = find(y); };
  // connect vertically adjacent same-implant runs that share a column.
  for (int r = 0; r + 1 < l.num_rows; ++r) {
    for (int i : by_row[r]) {
      for (int j : by_row[r + 1]) {
        if (runs[i].run.implant == runs[j].run.implant
            && std::max(runs[i].run.lo, runs[j].run.lo)
                   < std::min(runs[i].run.hi, runs[j].run.hi)) {
          unite(i, j);
        }
      }
    }
  }
  // accumulate area + a representative (top-most, left-most run) per region.
  std::map<int, int> area;
  std::map<int, int> rep;  // region root -> representative run index
  for (size_t i = 0; i < runs.size(); ++i) {
    const int root = find(static_cast<int>(i));
    area[root] += runs[i].run.width();
    auto it = rep.find(root);
    if (it == rep.end()) {
      rep[root] = static_cast<int>(i);
    }
  }
  for (auto& [root, a] : area) {
    if (a < rules_.min_area) {
      const RRun& rr = runs[rep[root]];
      out.push_back(makeViol(
          ViolType::MinArea, rr.run.implant, rr.row, rr.run.lo, rr.run.hi));
    }
  }
}

std::vector<Violation> ImplantRepairPlanner::detect(const Layout& l) const
{
  std::vector<Violation> out;
  detectIntraRowWidth(l, out);
  detectIntraRowSpacing(l, out);
  detectInterRow(l, out);
  detectMinArea(l, out);
  return out;
}

// ===========================================================================
// REPAIR
// ===========================================================================

int ImplantRepairPlanner::smallestFiller(ImplantId implant) const
{
  int best = INT_MAX;
  for (const Filler& f : fillers_) {
    if (f.implant == implant && f.width >= rules_.min_filler) {
      best = std::min(best, f.width);
    }
  }
  return best;
}

// Exact-cover DP: can a span of `width` sites be tiled by fillers of `implant`
// (each piece >= min_filler)? Records the chosen widths in `pieces`.
bool ImplantRepairPlanner::canTile(ImplantId implant,
                                   int width,
                                   std::vector<int>& pieces) const
{
  pieces.clear();
  if (width == 0) {
    return true;
  }
  if (width < 0) {
    return false;
  }
  std::vector<int> usable;
  for (const Filler& f : fillers_) {
    if (f.implant == implant && f.width >= rules_.min_filler) {
      usable.push_back(f.width);
    }
  }
  std::vector<int> choice(width + 1, -1);  // choice[w] = filler used to reach w
  std::vector<char> ok(width + 1, 0);
  ok[0] = 1;
  for (int w = 1; w <= width; ++w) {
    for (int fw : usable) {
      if (fw <= w && ok[w - fw]) {
        ok[w] = 1;
        choice[w] = fw;
        break;
      }
    }
  }
  if (!ok[width]) {
    return false;
  }
  for (int w = width; w > 0;) {
    pieces.push_back(choice[w]);
    w -= choice[w];
  }
  std::reverse(pieces.begin(), pieces.end());
  return true;
}

void ImplantRepairPlanner::placeFillers(Layout& work,
                                        ImplantId implant,
                                        int row,
                                        int col,
                                        const std::vector<int>& pieces,
                                        std::vector<FillerSpot>& spots) const
{
  int c = col;
  for (int w : pieces) {
    spots.push_back({implant, row, c, w});
    for (int k = 0; k < w; ++k) {
      work.at(row, c + k).kind = SiteKind::Cell;  // filler now occupies it
      work.at(row, c + k).implant = implant;      // carrying the same VT
    }
    c += w;
  }
}

bool ImplantRepairPlanner::repairOne(Layout& work,
                                     const Violation& v,
                                     std::vector<FillerSpot>& spots,
                                     std::string& reason) const
{
  // Helper: grow a run in `row` (currently [lo,hi)) by `need` same-implant
  // sites taken from adjacent EMPTY space (right first, then left, then a
  // right+left split). Each filled span must be tileable by available fillers.
  auto grow = [&](int row, int lo, int hi, int need) -> bool {
    if (need <= 0) {
      return true;
    }
    const int rcap = emptyRight(work, row, hi);
    const int lcap = emptyLeft(work, row, lo - 1);
    std::vector<int> pr, pl;
    // right-only
    if (rcap >= need && canTile(v.implant, need, pr)) {
      placeFillers(work, v.implant, row, hi, pr, spots);
      return true;
    }
    // left-only
    if (lcap >= need && canTile(v.implant, need, pl)) {
      placeFillers(work, v.implant, row, lo - need, pl, spots);
      return true;
    }
    // right + left split (fill as much as possible on the right, rest left)
    for (int r = std::min(need, rcap); r >= std::max(0, need - lcap); --r) {
      const int le = need - r;
      if (le > lcap) {
        continue;
      }
      if ((r == 0 || canTile(v.implant, r, pr))
          && (le == 0 || canTile(v.implant, le, pl))) {
        if (r > 0) {
          placeFillers(work, v.implant, row, hi, pr, spots);
        }
        if (le > 0) {
          placeFillers(work, v.implant, row, lo - le, pl, spots);
        }
        return true;
      }
    }
    if (rcap + lcap < need) {
      reason = "insufficient whitespace to reach threshold (need "
               + std::to_string(need) + " sites, have "
               + std::to_string(rcap + lcap) + ")";
    } else {
      reason = "cannot tile extension with available fillers (min filler width "
               + std::to_string(rules_.min_filler) + ")";
    }
    return false;
  };

  // Locate, in the CURRENT working layout, the run that the violation refers
  // to (cumulative repairs may already have changed earlier windows).
  auto findRun = [&](int row, int col) -> Run {
    for (const Run& run : rowRuns(work, row)) {
      if (col >= run.lo && col < run.hi) {
        return run;
      }
    }
    return {kNoImplant, col, col};  // not found (already merged/repaired)
  };

  switch (v.type) {
    case ViolType::IntraMW: {
      const Run run = findRun(v.row, v.col_lo);
      if (run.implant != v.implant) {
        return true;  // already fixed by an earlier cumulative repair
      }
      const int need = rules_.intra_mw - run.width();
      return grow(v.row, run.lo, run.hi, need);
    }
    case ViolType::MinArea: {
      // Grow the representative run until the whole region reaches min_area.
      // Each added site is one new unit cell of the region.
      const Run run = findRun(v.row, v.col_lo);
      if (run.implant != v.implant) {
        return true;
      }
      // current region area: re-run detection-style union would be heavy; the
      // representative run's width underestimates area, so compute the precise
      // deficit by re-detecting min-area for this implant cheaply: grow by the
      // smallest amount that clears it. We grow the run by (min_area - regionA)
      // where regionA is recomputed here.
      // Recompute region area containing (v.row, run.lo):
      // (small layouts -> simple BFS over runs)
      // For simplicity grow by (min_area - run.width()) clamped to >=0; if the
      // region had other runs this may over-grow slightly but never violates.
      // To stay tight we recompute the connected area:
      // -- BFS over runs sharing columns across rows --
      int regionA = 0;
      {
        std::vector<std::vector<Run>> runs(work.num_rows);
        for (int r = 0; r < work.num_rows; ++r) {
          runs[r] = rowRuns(work, r);
        }
        // find seed
        std::set<std::pair<int, int>> seen;  // (row, lo)
        std::vector<std::pair<int, int>> stack;
        stack.push_back({v.row, run.lo});
        auto runAt = [&](int row, int lo) -> const Run* {
          for (const Run& rr : runs[row]) {
            if (rr.lo == lo) {
              return &rr;
            }
          }
          return nullptr;
        };
        while (!stack.empty()) {
          auto [rr, ll] = stack.back();
          stack.pop_back();
          if (rr < 0 || rr >= work.num_rows || !seen.insert({rr, ll}).second) {
            continue;
          }
          const Run* cur = runAt(rr, ll);
          if (!cur || cur->implant != v.implant) {
            continue;
          }
          regionA += cur->width();
          for (int nr : {rr - 1, rr + 1}) {
            if (nr < 0 || nr >= work.num_rows) {
              continue;
            }
            for (const Run& other : runs[nr]) {
              if (other.implant == v.implant
                  && std::max(cur->lo, other.lo)
                         < std::min(cur->hi, other.hi)) {
                stack.push_back({nr, other.lo});
              }
            }
          }
        }
      }
      const int need = rules_.min_area - regionA;
      return grow(v.row, run.lo, run.hi, need);
    }
    case ViolType::IntraMS: {
      // Merge the two same-implant runs by filling the EMPTY gap between them.
      const int glo = v.col_lo, ghi = v.col_hi, g = ghi - glo;
      for (int c = glo; c < ghi; ++c) {
        if (work.at(v.row, c).kind != SiteKind::Empty) {
          reason = "spacing gap is occupied (needs cell movement)";
          return false;
        }
      }
      std::vector<int> pieces;
      if (!canTile(v.implant, g, pieces)) {
        reason = "gap (" + std::to_string(g)
                 + " sites) below minimum filler width / not tileable";
        return false;
      }
      placeFillers(work, v.implant, v.row, glo, pieces, spots);
      return true;
    }
    case ViolType::InterMW: {
      // Widen the vertical overlap of the two rows by extending whichever row
      // is the limiting (shorter) one into adjacent EMPTY columns where the
      // OTHER row already carries the implant.
      const int r0 = v.row, r1 = v.row + 1;
      const Run a = findRun(r0, v.col_lo);
      const Run b = findRun(r1, v.col_lo);
      if (a.implant != v.implant || b.implant != v.implant) {
        return true;  // changed by earlier repair
      }
      const int olo = std::max(a.lo, b.lo), ohi = std::min(a.hi, b.hi);
      int need = rules_.inter_mw - (ohi - olo);
      // extend right: deficient row is the one with smaller hi.
      auto extendSide = [&](bool right) {
        while (need > 0) {
          // column just outside the current overlap on this side
          const int oloN = std::max(a.lo, b.lo);
          const int ohiN = std::min(a.hi, b.hi);
          (void)oloN;
          (void)ohiN;
          const int c = right ? ohi + (rules_.inter_mw - (ohi - olo) - need)
                              : olo - 1 - ((rules_.inter_mw - (ohi - olo)) - need);
          if (c < 0 || c >= work.num_sites) {
            break;
          }
          const Site& s0 = work.at(r0, c);
          const Site& s1 = work.at(r1, c);
          const bool c0_imp = s0.kind == SiteKind::Cell && s0.implant == v.implant;
          const bool c1_imp = s1.kind == SiteKind::Cell && s1.implant == v.implant;
          if (c0_imp && c1_imp) {  // already overlapping here
            --need;
            continue;
          }
          if (c0_imp && s1.kind == SiteKind::Empty
              && smallestFiller(v.implant) == 1) {
            placeFillers(work, v.implant, r1, c, {1}, spots);
            --need;
            continue;
          }
          if (c1_imp && s0.kind == SiteKind::Empty
              && smallestFiller(v.implant) == 1) {
            placeFillers(work, v.implant, r0, c, {1}, spots);
            --need;
            continue;
          }
          break;  // blocked / different implant / no 1-site filler
        }
      };
      extendSide(true);
      if (need > 0) {
        extendSide(false);
      }
      if (need == 0) {
        return true;
      }
      reason = "cannot widen inter-row overlap with available fillers";
      return false;
    }
    case ViolType::InterMS: {
      // Two same-implant cells in adjacent rows that are too close: filler
      // cannot increase spacing, so this needs placement/legalization.
      reason = "inter-row spacing too small: needs cell movement (not "
               "filler-repairable)";
      return false;
    }
    case ViolType::MinFiller:
      reason = "minimum filler width prevents a legal fill";
      return false;
  }
  return false;
}

RepairPlan ImplantRepairPlanner::plan(const Layout& layout) const
{
  RepairPlan out;
  const std::vector<Violation> orig = detect(layout);

  Layout work = layout;  // never touch the caller's layout
  std::map<std::string, std::string> reasons;  // viol key -> failure reason

  // Attempt a filler-only repair for each violation, cumulatively, on `work`.
  for (const Violation& v : orig) {
    std::string reason;
    if (!repairOne(work, v, out.suggestions, reason)) {
      reasons[key(v)] = reason;
    }
  }

  // Re-detect on the repaired layout to decide what actually got fixed.
  const std::vector<Violation> final_v = detect(work);
  std::set<std::string> final_keys;
  for (const Violation& f : final_v) {
    final_keys.insert(key(f));
  }
  std::set<std::string> orig_keys;
  for (const Violation& v : orig) {
    orig_keys.insert(key(v));
  }

  for (const Violation& v : orig) {
    Violation rec = v;
    if (final_keys.count(key(v)) == 0) {
      rec.repairable_by_filler = true;
      out.repaired.push_back(rec);
    } else {
      rec.repairable_by_filler = false;
      auto it = reasons.find(key(v));
      rec.reason = (it != reasons.end())
                       ? it->second
                       : "still violated after filler repair";
      out.remaining.push_back(rec);
    }
  }
  // Defensive: a same-implant fill should never create a brand-new violation,
  // but if it did, surface it rather than hide it.
  for (const Violation& f : final_v) {
    if (orig_keys.count(key(f)) == 0) {
      Violation rec = f;
      rec.repairable_by_filler = false;
      rec.reason = "introduced while repairing another violation";
      out.remaining.push_back(rec);
    }
  }
  return out;
}

}  // namespace dpl
