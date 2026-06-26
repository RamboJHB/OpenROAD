#include "FillerVtRepairOracle.h"

namespace dpl_fr {

OracleRepairResult FillerVtRepairOracle::repair(ImplantChecker& chk) const
{
  OracleRepairResult res;
  res.violations_before = chk.violations();

  const std::vector<Vt> cand = chk.candidateVts();

  bool improved = true;
  int rounds = 0;
  while (improved && rounds++ < 100) {
    improved = false;
    // Re-fetch runs each round: a commit re-tiles a run (same cols, new VT), so
    // VTs of later runs in a stale list could be out of date.
    const std::vector<FillerRun> runs = chk.changeableRuns();
    for (const FillerRun& run : runs) {
      int best_cost = chk.violations();
      Vt best = run.vt;
      for (const Vt& v : cand) {
        if (v == run.vt) {
          continue;
        }
        const int cost = chk.evalReplaceRun(run, v);
        if (cost < best_cost) {
          best_cost = cost;
          best = v;
        }
      }
      if (best != run.vt && chk.commitReplaceRun(run, best)) {
        ++res.replaced_runs;
        improved = true;
      }
    }
  }

  res.violations_after = chk.violations();
  return res;
}

}  // namespace dpl_fr
