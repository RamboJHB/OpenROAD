// ImplantChecker: portable binding to an incremental implant-DRC oracle.
//
// The production upstream is ecoPlace's `ImplantLayerChecker`, which is not
// just a data source but an incremental check / commit engine (initialize ->
// checkPlace / checkDirect -> commitPlace).  Instead of re-implementing MW/MS
// (and P/N, PRL, abutment, case-B...) ourselves, the repair uses the checker as
// a COST ORACLE: it proposes candidate filler-VT replacements, asks the oracle
// how many violations each would leave, keeps the improving ones, and commits
// them.
//
// This abstract interface is the seam.  It is expressed in *site / row* units
// (the adapter owns DBU geometry), and each method documents how it binds to
// the real ImplantLayerChecker API.  FakeImplantChecker (see
// FakeImplantChecker.h) implements it in memory for tests; a real adapter
// implements it over ImplantLayerChecker.
//
// See docs/filler_vt_repair_spec.md Part B.
#pragma once

#include <string>
#include <vector>

#include "FillerGrid.h"  // Vt, Filler, VT_NONE

namespace dpl_fr {

// A changeable filler run on one row: cols [col, col+width), current VT.
// (Derived by the adapter from the checker's placedInsts() + masters.)
struct FillerRun
{
  int row = 0;
  int col = 0;
  int width = 0;
  Vt vt;
};

class ImplantChecker
{
 public:
  virtual ~ImplantChecker() = default;

  // Current total MW/MS violation count in the managed region.
  //   binds to: ImplantLayerChecker::checkDirect(...) over the region.
  virtual int violations() = 0;

  // All changeable (filler) runs, left-to-right per row.
  //   binds to: derived from ImplantLayerChecker::placedInsts() (isFiller).
  virtual std::vector<FillerRun> changeableRuns() = 0;

  // Candidate VTs the filler library can realize.
  //   binds to: derived from the isFiller masters in ImplantInput.
  virtual std::vector<Vt> candidateVts() = 0;

  // Hypothetical: total violations if `run` were re-tiled as `vt`
  // (NON-committing).  Returns a large sentinel if the library cannot
  // exact-tile that run as `vt`.
  //   binds to: ImplantLayerChecker::checkPlace(...) on the replacement.
  virtual int evalReplaceRun(const FillerRun& run, const Vt& vt) = 0;

  // Apply the replacement (delete the old run's fillers, place new-VT fillers).
  // Returns false and leaves state unchanged if not realizable.
  //   binds to: removeInstance(...) + commitPlace(...)  (RD: remove must be
  //   public -- see spec Part B "open questions").
  virtual bool commitReplaceRun(const FillerRun& run, const Vt& vt) = 0;
};

}  // namespace dpl_fr
