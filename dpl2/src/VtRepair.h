// VtRepair: weight-based, single-pass filler-VT repair for implant MW/MS.
//
// CONTRACT (see docs/filler_vt_repair_spec.md):
//   - Premise: the design is 100% utility (every site is a filler or a cell;
//     no empty sites).
//   - We ONLY change a filler's VT (implant type).  A filler's width / height /
//     position never change; a relabel is a 1:1 swap to a same-(width,height)
//     master of the new VT (orientation follows the row so P/N aligns).
//   - One pass, NO DRC re-check: violations come from the upstream checker; for
//     each violation we pick one neighbouring filler to relabel.  Anything we
//     cannot fix is reported as UNFIXABLE.
//
// PORTING:  all database access is isolated behind the DesignIO interface below
// (the "PORTING SEAM").  To move this to another database, re-implement those
// methods; the algorithm in VtRepair.cpp does not change.  FakeDesign.h is a
// reference in-memory implementation used by the tests.
#pragma once

#include <string>
#include <vector>

namespace vtrepair {

// VT / implant identity.  Empty string == "none".
using Vt = std::string;
inline const Vt VT_NONE = "";

// Opaque id of a filler instance.  -1 == "no filler".
using FillerId = int;
inline constexpr FillerId NO_FILLER = -1;

enum class SiteKind
{
  Empty,   // no occupant (should not occur under 100% utility)
  Filler,  // a filler instance (changeable: VT only)
  Cell,    // a standard cell (FIXED boundary)
  Blocked  // macro / blockage (FIXED boundary)
};

// One violation as handed to us by the upstream checker.  We do NOT re-derive
// MW/MS; we only consume the location and look at its neighbourhood.
struct Violation
{
  int id = 0;
  int row = 0;       // anchor site row (the flagged spot)
  int col = 0;       // anchor site col
  std::string type;  // "MW" | "MS" (informational only)
};

// A filler instance's rectangle (rows [row,row+height) x cols [col,col+width)).
struct FillerBox
{
  int row = 0;
  int col = 0;
  int width = 0;
  int height = 0;
  Vt vt;
};

// ===========================================================================
// PORTING SEAM -- re-implement every method below against your database.
// The repair algorithm only ever touches the design through this interface.
// Coordinates are *sites / rows* (integers), never DBU; the implementation owns
// DBU geometry, orientation, and the (vt,width,height) -> master mapping.
// ===========================================================================
class DesignIO
{
 public:
  virtual ~DesignIO() = default;

  // -------------------- READ --------------------
  // The violation list from the upstream checker (one-shot snapshot).
  virtual std::vector<Violation> readViolations() = 0;

  virtual int numRows() const = 0;
  virtual int numCols(int row) const = 0;

  // Site classification and the VT of whatever occupies the site.
  virtual SiteKind kindAt(int row, int col) const = 0;
  virtual Vt vtAt(int row, int col) const = 0;

  // The filler instance occupying a site (NO_FILLER if the site is not a
  // filler), and that instance's rectangle.
  virtual FillerId fillerIdAt(int row, int col) const = 0;
  virtual FillerBox fillerBox(FillerId id) const = 0;

  // Candidate VTs the filler library provides.
  virtual std::vector<Vt> candidateVts() const = 0;

  // Realizability: does the library have a master of EXACTLY this size in `vt`?
  // (We never resize; a relabel must reuse the same width/height.)
  virtual bool masterExists(const Vt& vt, int width, int height) const = 0;

  // -------------------- WRITE --------------------
  // Relabel a filler's VT in place: delete the old instance and create a
  // same-(width,height) master of `newVt` at the same location, orientation
  // following the row.  Nothing else changes.
  virtual void replaceFillerVt(FillerId id, const Vt& newVt) = 0;

  // Report a violation we could not fix (handed back upstream).
  virtual void reportUnfixable(const Violation& v, const std::string& reason)
      = 0;
};

// ---------------------------------------------------------------------------
// The algorithm.
// ---------------------------------------------------------------------------
struct Action
{
  int violation_id = 0;
  bool fixed = false;           // true: relabeled; false: unfixable
  FillerId filler = NO_FILLER;  // relabeled filler (if fixed)
  Vt new_vt;                    // its new VT (if fixed)
  std::string reason;           // unfixable reason (if !fixed)
};

struct RunResult
{
  int violations = 0;
  int fixed = 0;
  int unfixable = 0;
  std::vector<Action> actions;
};

// Per-candidate weight bundle (exposed for testing / debug).
struct CandidateWeight
{
  FillerId filler = NO_FILLER;
  long weight = 0;   // higher == more island-like == fix this one first
  int width = 0;     // tie-break #1: narrower wins
  long same_vt = 0;  // y = # of same-VT neighbour adjacencies; tie-break #2:
                     // smaller wins
  int col = 0;       // tie-break #3: leftmost (smaller col, then row) wins
  int row = 0;
  bool touches_cell = false;
  Vt target;  // VT to relabel into (majority neighbour, realizable)
  bool has_target = false;
};

class VtRepair
{
 public:
  static constexpr long kIslandWeight = 1
                                        << 20;  // "no same-VT neighbour" weight

  // Run the single-pass repair.  `verbose` prints the full decision chain.
  RunResult run(DesignIO& io, bool verbose = true) const;

  // Compute the weight bundle for one candidate filler (exposed for tests).
  static CandidateWeight weighCandidate(DesignIO& io, FillerId id);

  // Pick the winner among usable candidates by the tie-break order:
  //   higher weight -> narrower width -> smaller same_vt (y) -> leftmost
  //   (smaller col, then smaller row).
  // Returns the index into `usable`, or -1 only if `usable` is empty.
  static int chooseCandidate(const std::vector<CandidateWeight>& usable);
};

}  // namespace vtrepair
