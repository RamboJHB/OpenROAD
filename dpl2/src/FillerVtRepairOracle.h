// FillerVtRepairOracle: the decision / search layer of the filler VT repair.
//
// This is "our" half of the production solution (Part B): the checker
// (ImplantChecker) is the cost oracle; this class decides WHICH filler VTs to
// try and in what order.  It is intentionally tiny and database-agnostic -- it
// talks only to the ImplantChecker interface, never to a DB or the site grid.
//
// Strategy (Tier-1): greedy descent over changeable filler runs.  For each run,
// try every candidate VT, ask the oracle for the resulting violation count, and
// commit the best improving replacement.  Iterate to a fixed point.  (Tier-2
// window-DP can replace the greedy here without touching the oracle seam.)
//
// See docs/filler_vt_repair_spec.md Part B.
#pragma once

#include "ImplantChecker.h"

namespace dpl_fr {

struct OracleRepairResult
{
  int violations_before = 0;
  int violations_after = 0;
  int replaced_runs = 0;
};

class FillerVtRepairOracle
{
 public:
  OracleRepairResult repair(ImplantChecker& chk) const;
};

}  // namespace dpl_fr
