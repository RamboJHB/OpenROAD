# Functional Spec — Filler VT Repair (inter-row MW/MS, replacement-only)

Status: prototype implemented & tested (13/13). Last updated 2026-06.
Scope of this document: the **VT-replacement** repair only. It is database-
agnostic and ported by implementing one interface (§7).

---

## 1. Purpose

After place & route, optimization (`opto`) and ECO legalization perturb the
layout and disturb fillers, producing **implant-layer DRC violations** between
adjacent rows. This module repairs the **inter-row min-width (MW)** and
**min-spacing (MS)** violations **by replacing the VT (implant type) of
fillers** — it never moves, resizes, or deletes a standard cell, and it never
moves a filler; it only swaps a filler for one of a different implant type in
the same place.

Intuition: filler sites are free implant variables, cell sites are fixed implant
boundaries. Choose the filler implant types so the 2D implant picture has as few
MW/MS violations as possible, then realize that choice by delete+create.

Non-goal: this is not foundry-grade polygon DRC. It operates on a **site-grid
implant model** (each occupied site carries a VT); exact DRC is upstream.

---

## 2. Context (where it sits)

```
… route → opto → ECO legalization → filler DRC check → mark violations → [THIS MODULE]
                  └──── disturbance ────┘   └──── upstream detection ────┘
```

The DRC check and violation marking are **upstream**. This module consumes the
grid + rule values, edits fillers, and reports anything it cannot fix as
residual (handed back upstream, which decides cell-move / implant-edit — out of
scope here).

---

## 3. Scope

In scope:
- Violation types: **inter-row (and intra-row) min-width (MW)** and
  **min-spacing (MS)** on the implant layer, modeled on a site grid.
- Repair action: **replace filler VT** (delete the old filler, create one of the
  target implant type covering the same sites). Cells / macros / blockages are
  fixed boundaries.

Out of scope (→ reported as residual/unsolved, handed upstream):
- moving / resizing / deleting standard cells;
- moving fillers or changing the set of occupied sites;
- min-area and other implant rules;
- the DRC check and violation marking themselves.

---

## 4. Inputs / Outputs

### 4.1 Inputs
- **Design grid** (through the `FillerGrid` interface, §7): rows / sites, and
  per site a **kind** (`Cell` / `CleanFiller` / `Empty` / `Blocked`) and a
  **VT** (for occupied sites).
- **Filler library**: masters with `{VT, width(sites), height(rows)}`. A VT is
  realizable on the grid only if the library can exact-tile the needed width
  with that VT (the prototype uses height-1 fillers when re-tiling a run).
- **Implant rule values** (in sites): `min_width` (ωMW) and `min_spacing` (ωMS).
- **VT ↔ implant-layer map**: a filler's VT is *which* IMPLANT-type layer its
  master carries (e.g. `LVTN → "L"`). The adapter owns this mapping.

### 4.2 Outputs / side effects
- DB mutated: fillers whose chosen VT changed are deleted and re-created with the
  new VT (physical-only, orientation follows the row).
- A `VtRepairResult` record: `violations_before`, `violations_after`,
  the list of `replaced` fillers, and `unresolved` (= `violations_after`,
  the residual to hand upstream).

---

## 5. Functional requirements

- **FR-1 Only-filler**: never move/resize/delete cells, macros, blockages, and
  never move a filler. Cells are fixed VT boundaries; only the implant type of a
  filler site may change.
- **FR-2 MW/MS objective**: choose filler VTs to **minimize** the site-grid
  MW + MS violation count over the whole grid.
- **FR-3 Maximize, don't require full clear**: the goal is to **minimize
  residual** violations, not to fully clear or fail. Residual (library lacks the
  needed VT, cell-blocked, or a change would create new violations) → reported.
- **FR-4 Realizability**: any chosen VT for a filler run must be exact-tileable
  by available masters; otherwise that run is reverted to its original VT
  (counts as residual). No partial / leftover gaps.

---

## 6. Algorithm — `FillerVtRepair`

1. **Read** the grid into three site-grid layers: `vt[r][c]`,
   `present[r][c]` (`Cell | CleanFiller`), and `filler[r][c]` (changeable ==
   `CleanFiller`). Keep `orig` = the original VTs.
2. **2D MW/MS evaluator** `countViolations(vt, present, rules)`:
   - **MW**: a present site is OK iff its same-VT **horizontal OR vertical run**
     ≥ ωMW; otherwise it is a narrow neck → +1 (this catches a 1-wide inter-row
     staircase that looks fine within each single row).
   - **MS**: two orthogonally-adjacent present sites of **different VT** → +1
     (when ωMS ≥ 1, different implant regions may not touch). Counted on the
     right and down neighbors only, to avoid double counting.
3. **Greedy coordinate descent** over filler sites: for each filler site try
   every library VT and keep the one minimizing a combined cost
   `violations*1000 − sameVT_adjacencies`. The second term (merge) breaks
   single-site plateaus where a change only pays off **together** with a
   neighbor change. Iterate to a fixed point (capped rounds).
4. **Apply**: for each maximal same-VT filler run whose VT changed, `exactFill`
   the width with library masters of that VT → `clearSite` the old fillers,
   `placeFiller` the new ones. A run that cannot be exact-tiled is **reverted**
   (residual).
5. `violations_after` = residual; report it.

> Tier-2 (future): a column-DP (paper "Toward Optimal Filler Cell Insertion",
> Algorithm 4, generalized to a few rows) for patch-optimal assignment. The
> greedy already covers staircase-MW and inter-row-MS cases.

---

## 7. Interfaces & data model (portable) — what a new database must provide

The algorithm only ever touches the database through the abstract `FillerGrid`
(`dpl2/src/FillerGrid.h`). Porting = implementing this one class against the new
DB. Coordinates are **sites / rows**, never DBU; the adapter owns DBU geometry
and the `(vt, width, height) → master` mapping.

### 7.1 The `FillerGrid` interface (6 methods)

```cpp
class FillerGrid {
 public:
  virtual ~FillerGrid() = default;
  // ---- read ----
  virtual int      numRows() const = 0;                  // number of rows
  virtual int      numCols(int row) const = 0;           // sites in a row
  virtual SiteKind kindAt(int row, int col) const = 0;   // Empty/Cell/CleanFiller/Blocked
  virtual Vt       vtAt(int row, int col) const = 0;     // implant id of an occupied site
  // ---- mutate ----
  virtual void clearSite(int row, int col) = 0;          // delete the filler here -> Empty
  virtual void placeFiller(const PlacedFiller& f) = 0;   // create one filler instance
};
```

What each method must do, and what the adapter has to compute from the new DB:

| Method | Returns / does | Data the new DB must expose to implement it |
|---|---|---|
| `numRows()` | row count | the placement rows of the core area |
| `numCols(row)` | sites in that row | row width / site width |
| `kindAt(r,c)` | site classification | for the instance covering a site: is it a **filler** (CORE SPACER-equivalent), a **cell**, a **macro/blockage**, or is the site **empty** |
| `vtAt(r,c)` | implant id of an occupied site | the **IMPLANT-type layer** carried by that instance's master → mapped to a VT id (string) |
| `clearSite(r,c)` | delete the filler at `(r,c)`, leave it empty | ability to **delete a (filler) instance** |
| `placeFiller(f)` | create one filler instance over `f.row/col/width/height` with VT `f.vt` | a `(vt, width, height) → master` lookup, plus **create instance + set location + set orientation** (orientation follows the row); place as physical-only |

### 7.2 Other inputs the caller must supply

- **Filler library**: `std::vector<Filler>` with `{vt, width(sites),
  height(rows), name}` for every available filler master (the same masters
  `placeFiller` can instantiate). Needs at least the widths required to
  exact-tile the runs (no 1-site filler ⇒ exact-fill mandatory).
- **`VtRules`**: `min_width` (ωMW) and `min_spacing` (ωMS), in **sites**.
- **VT ↔ implant-layer map**: how an IMPLANT-type layer name maps to a VT id —
  used by both `vtAt` and the library build. Owned by the adapter.
- (Optional, for a future per-violation mode) per-MW/MS-violation DRC data:
  `type` (MW|MS), `rule_value`, the participating implant regions and their
  owners — to drive a targeted repair instead of a whole-grid pass.

### 7.3 Geometry the adapter owns (not the algorithm)

- core-area origin, site width, row height (DBU ↔ site/row conversion);
- per-row orientation (R0 / MX …) so a created filler aligns rails / implant;
- the `(vt, width, height) → master` map for `placeFiller`.

A reference implementation is `dpl2/src/FakeFillerGrid.h` (in-memory) used by the
unit test; copy its shape and back each method with the real DB.

---

## 8. Behavior catalogue (site-grid cases)

| # | Situation | Action |
|---|---|---|
| 1 | inter-row MW staircase (1-wide diagonal of fillers) | replace fillers to widen the same-VT run (H/V) until ≥ ωMW |
| 2 | inter-row MS: filler row of VT-A directly over filler row of VT-B | replace one side's fillers so the touching regions share a VT (merge) |
| 3 | filler bounded by same-VT cells | replace filler to that VT (extends the cell's implant) |
| 4 | clean filler next to the target run | it is a fixed boundary unless itself changeable; only filler sites change |
| 5 | needed VT absent in library | run reverted → residual → upstream |
| 6 | a VT change would create a new violation | rejected by the cost function → stays |
| 7 | needs a cell move to fix | out of scope → residual → upstream |
| 8 | already clean | no-op (0 before, 0 after) |

---

## 9. Constraints & assumptions

- The implant model is **site-grid**, not polygon DRC.
- Exact-fill is mandatory when re-tiling a changed run (no 1-site filler ⇒ no
  leftover gap); an untileable run is reverted rather than left partial.
- A single uniform `min_width` / `min_spacing` (per-VT ωMW/ωMS and ωMS > 1 across
  empties are future work).
- The prototype re-tiles changed runs with **height-1** fillers.

---

## 10. Verification

- `dpl2/test/filler_vt_repair_test.cpp`: **13/13** —
  T1 staircase MW+MS fully fixed (fillers H→L, cells untouched),
  T2 inter-row MS merge, T3 library lacks VT → residual (nothing replaced),
  T4 clean no-op, T5 evaluator sanity (flags L-over-H, clean on solid block).

Build / run (no database, no Phase I dependency):
```
g++ -std=c++17 -I dpl2/src dpl2/src/FillerVtRepair.cpp \
    dpl2/test/filler_vt_repair_test.cpp -o /tmp/vt && /tmp/vt
```

---

## 11. Open items

- Real-DB adapter on the new database (implement §7.1) + an end-to-end test.
- Tier-2 column-DP for patch-optimal MW/MS.
- Per-VT ωMW/ωMS and ωMS > 1 (gap across empties) in the evaluator.
- Per-violation targeted mode (consume §7.2 DRC data) instead of whole-grid.
- Multi-height re-tiling on apply (currently height-1).
