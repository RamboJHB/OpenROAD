#include "VtRepair.h"

#include <algorithm>
#include <iostream>
#include <map>

namespace vtrepair {

namespace {

bool inRange(DesignIO& io, int r, int c)
{
  return r >= 0 && r < io.numRows() && c >= 0 && c < io.numCols(r);
}

// Single filler neighbour just outside one site (NO_FILLER if not a filler).
struct Nb
{
  SiteKind kind = SiteKind::Empty;
  Vt vt;
  bool filler = false;
};
Nb probe(DesignIO& io, int r, int c)
{
  Nb n;
  if (!inRange(io, r, c)) {
    return n;  // boundary -> treated as "nothing"
  }
  n.kind = io.kindAt(r, c);
  n.filler = (n.kind == SiteKind::Filler);
  if (n.filler) {
    n.vt = io.vtAt(r, c);
  }
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Weight for one candidate filler.
//   - touches a fixed cell on any edge            -> weight 0 (do not flip it)
//   - no same-VT neighbour on any edge (island)   -> weight = kIslandWeight
//   - else weight = sum of different-VT adjacency: left/right capped 1 each,
//     top/bottom counted per cell (+x).
//   - target VT = the neighbour VT with the largest tally that the library can
//     realize at this filler's exact size.
// ---------------------------------------------------------------------------
CandidateWeight VtRepair::weighCandidate(DesignIO& io, FillerId id)
{
  CandidateWeight cw;
  cw.filler = id;
  const FillerBox box = io.fillerBox(id);
  const Vt self = box.vt;

  std::map<Vt, long> tally;  // tally[t] = adjacency count to VT t
  bool any_filler_nb = false;

  auto addEdgeCell = [&](const Nb& n) {
    if (n.kind == SiteKind::Cell || n.kind == SiteKind::Blocked) {
      cw.touches_cell = true;
    } else if (n.filler) {
      any_filler_nb = true;
      tally[n.vt] += 1;
    }
  };

  // left / right: a single neighbour each (filler edges are full-height here).
  addEdgeCell(probe(io, box.row, box.col - 1));
  addEdgeCell(probe(io, box.row, box.col + box.width));
  // top / bottom: every cell along the width.
  for (int c = box.col; c < box.col + box.width; ++c) {
    addEdgeCell(probe(io, box.row - 1, c));
    addEdgeCell(probe(io, box.row + box.height, c));
  }

  // RULE: a candidate touching a fixed cell is anchored -> weight 0.
  if (cw.touches_cell) {
    cw.weight = 0;
    return cw;
  }
  if (!any_filler_nb) {
    cw.weight = 0;  // isolated by boundary only -> nothing to merge with
    return cw;
  }

  const long same = tally.count(self) ? tally[self] : 0;
  long diff = 0;
  for (const auto& kv : tally) {
    if (kv.first != self) {
      diff += kv.second;
    }
  }

  // RULE: no same-VT neighbour anywhere -> island -> max weight.
  cw.weight = (same == 0) ? kIslandWeight : diff;

  // Target VT = largest-tally neighbour VT (!= self) realizable at this size.
  long best = 0;
  for (const auto& kv : tally) {
    if (kv.first == self) {
      continue;
    }
    if (kv.second > best && io.masterExists(kv.first, box.width, box.height)) {
      best = kv.second;
      cw.target = kv.first;
      cw.has_target = true;
    }
  }
  return cw;
}

RunResult VtRepair::run(DesignIO& io, bool verbose) const
{
  auto dbg = [&](const std::string& m) {
    if (verbose) {
      std::cout << "[vtr] " << m << "\n";
    }
  };

  RunResult res;
  const std::vector<Violation> viols = io.readViolations();
  res.violations = static_cast<int>(viols.size());
  dbg("read " + std::to_string(res.violations) + " violations");

  for (const Violation& v : viols) {
    dbg("--- violation #" + std::to_string(v.id) + " type=" + v.type + " at ("
        + std::to_string(v.row) + "," + std::to_string(v.col) + ")");

    // Candidate fillers: the filler at the anchor + its 4 neighbours (dedup).
    std::vector<FillerId> cands;
    const int dr[5] = {0, -1, 1, 0, 0};
    const int dc[5] = {0, 0, 0, -1, 1};
    for (int k = 0; k < 5; ++k) {
      const int r = v.row + dr[k], c = v.col + dc[k];
      if (inRange(io, r, c) && io.kindAt(r, c) == SiteKind::Filler) {
        const FillerId id = io.fillerIdAt(r, c);
        if (id != NO_FILLER
            && std::find(cands.begin(), cands.end(), id) == cands.end()) {
          cands.push_back(id);
        }
      }
    }
    dbg("  candidate fillers: " + std::to_string(cands.size()));

    // Weigh candidates and pick the best usable one.
    CandidateWeight best;
    bool any_cell = false, any_target_missing = false;
    for (FillerId id : cands) {
      CandidateWeight cw = weighCandidate(io, id);
      const FillerBox b = io.fillerBox(id);
      dbg("    filler " + std::to_string(id) + " vt=" + b.vt
          + " size=" + std::to_string(b.width) + "x" + std::to_string(b.height)
          + " weight=" + std::to_string(cw.weight)
          + (cw.touches_cell ? " [touches cell -> 0]" : "")
          + (cw.has_target ? (" -> target " + cw.target)
                           : " [no realizable target]"));
      if (cw.touches_cell) {
        any_cell = true;
      }
      if (!cw.touches_cell && !cw.has_target) {
        any_target_missing = true;
      }
      const bool usable = !cw.touches_cell && cw.has_target && cw.weight > 0;
      if (usable) {
        bool take;
        if (best.filler == NO_FILLER) {
          take = true;
        } else if (cw.weight != best.weight) {
          take = cw.weight > best.weight;
        } else {
          // Tie on weight: prefer the NARROWER filler (more island-like, and a
          // smaller change).
          const int bw = io.fillerBox(best.filler).width;
          if (b.width != bw) {
            take = b.width < bw;
          } else {
            // OPEN QUESTION (see spec): equal weight AND equal width -- which
            // to pick is undecided.  Deterministic lowest-id fallback for now.
            take = id < best.filler;
          }
        }
        if (take) {
          best = cw;
        }
      }
    }

    Action act;
    act.violation_id = v.id;
    if (best.filler != NO_FILLER) {
      const FillerBox b = io.fillerBox(best.filler);
      dbg("  => relabel filler " + std::to_string(best.filler) + " " + b.vt
          + " -> " + best.target + " (weight " + std::to_string(best.weight)
          + ")");
      io.replaceFillerVt(best.filler, best.target);
      act.fixed = true;
      act.filler = best.filler;
      act.new_vt = best.target;
      ++res.fixed;
    } else {
      std::string reason;
      if (cands.empty()) {
        reason = "no filler neighbour (cells/boundary only)";
      } else if (any_target_missing && !any_cell) {
        reason = "no same-size master for the needed VT";
      } else if (any_cell) {
        reason = "all candidate fillers abut a fixed cell";
      } else {
        reason = "no improving relabel";
      }
      dbg("  => UNFIXABLE: " + reason);
      io.reportUnfixable(v, reason);
      act.fixed = false;
      act.reason = reason;
      ++res.unfixable;
    }
    res.actions.push_back(act);
  }

  dbg("done: " + std::to_string(res.fixed) + " fixed, "
      + std::to_string(res.unfixable) + " unfixable");
  return res;
}

}  // namespace vtrepair
