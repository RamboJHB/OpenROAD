// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors
//
// ImplantRepairPlanner
// ====================
// Read-only planner that decides *where to insert which-VT filler cells* in
// order to repair implant-layer DRC problems (minimum width / minimum area /
// minimum spacing), and reports which problems a filler *cannot* fix.
//
// This core is intentionally dependency-free (no ODB / no dpl internals) so it
// can be unit tested in isolation and reused. The OpenROAD/dpl adapter is
// responsible for translating the placed design into the simple site grid
// below (see docs/filler_insertion.md sec 3-4); all the DRC logic lives here.
//
// ---------------------------------------------------------------------------
// MODEL (verified against Chen 2021 / Zou 2023 - see docs/filler_insertion.md)
// ---------------------------------------------------------------------------
//   * The placement area is a grid of rows x sites.
//   * Each site is EMPTY, BLOCKED, or occupied by a CELL carrying exactly one
//     implant id (== its VT). A mixed-cell-height cell simply paints its
//     implant id onto the sites it covers in every row it spans, so this model
//     needs no special case for cell height.
//   * A filler also carries exactly one implant id and a width in sites.
//   * Implant DRC = 5 thresholds (Zou's Omega) + minimum area:
//       intra_mw : intra-row minimum implant width
//       inter_mw : inter-row minimum abutting (overlap) width  (staircase)
//       intra_ms : intra-row minimum spacing between same-implant regions
//       inter_ms : inter-row minimum spacing between same-implant regions
//       min_area : minimum implant area (in unit site-cells)
//       min_filler (MF) : a filler cannot be narrower than this
//
// ---------------------------------------------------------------------------
// LOGIC CHAIN (what plan() does, end to end)
// ---------------------------------------------------------------------------
//   1. detect(layout): scan the painted grid and list every implant violation
//      (the 6 kinds above), each tagged with the implant + the row/column
//      window it refers to.
//   2. For each violation, attempt a *filler-only* repair on a working copy:
//        - too-narrow / too-small region  -> grow it by filling adjacent EMPTY
//          sites with the SAME implant filler until the threshold is met;
//        - two same-implant regions too close (spacing) -> fill the EMPTY gap
//          between them with the SAME implant filler so they merge;
//        - narrow staircase overlap (inter_mw) -> widen the overlap by filling
//          EMPTY sites in the thinner row.
//      A repair is accepted only if (a) there is enough EMPTY space, (b) the
//      added width can be tiled by available fillers each >= min_filler, and
//      (c) re-detecting on the working copy shows the targeted violation gone
//      and no NEW violation introduced.
//   3. Accepted repairs -> suggestions + `repaired`; everything else ->
//      `remaining` with a human-readable reason (no whitespace / below MF /
//      needs cell movement). The DB is never modified.

#pragma once

#include <string>
#include <vector>

namespace dpl {

// Implant / VT identifier. Distinct non-negative ids are distinct VTs.
using ImplantId = int;
inline constexpr ImplantId kNoImplant = -1;  // empty / blockage carry no implant

enum class SiteKind { Empty, Blocked, Cell };

struct Site
{
  SiteKind kind = SiteKind::Empty;
  // Valid only when kind == Cell: the implant/VT painted on this site.
  ImplantId implant = kNoImplant;
};

// A filler master: one implant id, width measured in sites.
struct Filler
{
  ImplantId implant;
  int width;  // sites (>= 1)
};

// The 5 implant-layer thresholds (Omega) plus minimum implant area, all in
// site units (area in unit site-cells). See model note above.
struct Rules
{
  int intra_mw = 1;
  int inter_mw = 1;
  int intra_ms = 1;
  int inter_ms = 1;
  int min_area = 1;
  int min_filler = 1;  // MF
};

// rows[r][c] : the painted layout. row 0 is the bottom row.
struct Layout
{
  int num_rows = 0;
  int num_sites = 0;
  std::vector<std::vector<Site>> sites;  // [row][col], size num_rows x num_sites

  Layout() = default;
  Layout(int rows, int cols)
      : num_rows(rows),
        num_sites(cols),
        sites(rows, std::vector<Site>(cols))
  {
  }

  const Site& at(int row, int col) const { return sites[row][col]; }
  Site& at(int row, int col) { return sites[row][col]; }
};

enum class ViolType {
  IntraMW,     // intra-row min width
  InterMW,     // inter-row min abutting width (staircase)
  IntraMS,     // intra-row min spacing
  InterMS,     // inter-row min spacing
  MinArea,     // min implant area
  MinFiller    // a required fill is impossible because of MF
};

const char* toString(ViolType t);

struct Violation
{
  ViolType type;
  ImplantId implant;
  int row = 0;       // primary row of the violation
  int col_lo = 0;    // window [col_lo, col_hi) the violation refers to
  int col_hi = 0;
  bool repairable_by_filler = false;
  std::string reason;  // filled for remaining (unrepairable) violations
};

// One suggested filler insertion (read-only output; nothing is created).
struct FillerSpot
{
  ImplantId implant;
  int row;
  int col;     // left-most site
  int width;   // sites
};

struct RepairPlan
{
  std::vector<FillerSpot> suggestions;   // fillers to insert for the repairs
  std::vector<Violation> repaired;       // violations the suggestions resolve
  std::vector<Violation> remaining;      // violations filler cannot fix
};

class ImplantRepairPlanner
{
 public:
  ImplantRepairPlanner(Rules rules, std::vector<Filler> fillers);

  // Pure detection: list all implant violations in `layout` (read-only).
  std::vector<Violation> detect(const Layout& layout) const;

  // Full logic chain (detect -> attempt filler repair -> classify).
  // `layout` is not modified; planning happens on an internal working copy.
  RepairPlan plan(const Layout& layout) const;

 private:
  // ---- detection helpers (each returns violations of one family) ----
  void detectIntraRowWidth(const Layout&, std::vector<Violation>&) const;
  void detectIntraRowSpacing(const Layout&, std::vector<Violation>&) const;
  void detectInterRow(const Layout&, std::vector<Violation>&) const;
  void detectMinArea(const Layout&, std::vector<Violation>&) const;

  // ---- repair helpers ----
  // True if a span of exactly `width` sites can be tiled by the available
  // fillers of `implant`, every piece >= min_filler. Fills `pieces` with the
  // chosen filler widths (left to right) when true.
  bool canTile(ImplantId implant, int width, std::vector<int>& pieces) const;

  // Try to repair one violation on `work` (mutated on success). On success,
  // append the inserted fillers to `spots`. Returns false (work untouched) if
  // no filler-only repair exists; `reason` explains why.
  bool repairOne(Layout& work,
                 const Violation& v,
                 std::vector<FillerSpot>& spots,
                 std::string& reason) const;

  // Paint a same-implant filler run onto EMPTY sites [col, col+width) of row,
  // recording one FillerSpot per filler piece. Caller guarantees the sites are
  // EMPTY and the tiling is valid.
  void placeFillers(Layout& work,
                    ImplantId implant,
                    int row,
                    int col,
                    const std::vector<int>& pieces,
                    std::vector<FillerSpot>& spots) const;

  // smallest available filler width for an implant (or INT_MAX if none).
  int smallestFiller(ImplantId implant) const;

  Rules rules_;
  std::vector<Filler> fillers_;
};

}  // namespace dpl
