# Functional Spec — Filler Repair (DRC-driven, replacement-only)

Status: implemented & tested. Last updated 2026-06.
Owner: filler-insertion. Related: `docs/filler_insertion.md`,
`docs/filler_repair_dpl.md`, `docs/filler_repair_porting.md`.

---

## 1. Purpose

After place & route, optimization (`opto`) and ECO legalization perturb the
layout; this disturbs fillers and creates **implant-layer DRC violations**. This
module repairs those violations **by editing fillers only** — it never moves a
standard cell. Two stages:

- **Phase I** — repair fillers an upstream DRC step flagged **dirty**: delete
  them and refill their footprint with correct fillers (fixes intra-row
  spacing / min-width). **(done)**
- **Phase II** — repair **inter-row MW / MS** between fillers by **replacing
  filler VT** (no move): choose filler implant types so the 2D implant picture
  has as few MW/MS violations as possible. **(prototype done)**

Non-goal: this is not foundry-grade polygon DRC. It operates on a **site-grid
implant model** (each site carries a VT); exact DRC is upstream.

---

## 2. Context (where it sits)

```
… route → opto → ECO legalization → filler DRC check → mark dirty → [THIS MODULE]
                  └──── disturbance ────┘   └──── upstream detection/marking ────┘
```
The DRC check and dirty-marking are **upstream**. This module consumes their
output and edits fillers. Anything it cannot fix is reported back (the upstream
solver decides cell-move / implant-edit, which are out of scope here).

---

## 3. Scope

In scope:
- Violation types: **spacing** and **min-width** (implant / base-layer), incl.
  inter-row MW/MS.
- Repair actions: **delete filler**, **create filler**, **replace filler VT**.
  All "only-filler"; cells, macros, blockages are fixed boundaries.

Out of scope (→ reported as residual/unsolved, handed upstream):
- moving / resizing standard cells (e.g. LEF Fig 3-1 case "B");
- min-area and other implant rules;
- decap / M2, trim-spacing, signal-DRC re-run;
- the DRC check and dirty-marking themselves.

---

## 4. Inputs / Outputs

### 4.1 Common inputs
- **Design grid**: rows / sites, placed instances, per-site kind
  (`Cell` / `CleanFiller` / `DirtyFiller` / `Blocked` / `Empty`) and per-site VT.
- **Filler library**: masters with `{VT, width(sites), height(rows)}`, in user
  order. Note: the production lib has **no 1-site filler** (forces exact-fill).
- **Implant rule values** (sites): `min_implant_width` (ωMW), and for Phase II
  `min_spacing` (ωMS).
- **VT ↔ implant-layer map**: implant layer name → VT id (a filler's VT is *which*
  IMPLANT-type layer its master carries, e.g. `LVTN→L`).

### 4.2 Phase I extra input
- **dirty markers**: which filler instances the upstream DRC flagged dirty.

### 4.3 Phase II extra input (per MW/MS violation)
- `type` (MW|MS), `rule_value` (ωMW|ωMS), `vts` (1 for MW, 2 for MS),
  `region` (per-row column intervals — geometry is 2D/irregular),
  `participants` (the owner instance + kind of each implant region forming the
  violation; used to keep "filler-filler" and locate fillers).

### 4.4 Outputs / side effects
- DB mutated: dirty fillers deleted; new/replaced fillers created (physical-only,
  orient follows row).
- A result record: list of placed/replaced fillers + list of **unsolved**
  windows/violations (with reason) to hand upstream.

---

## 5. Functional requirements

FR-1 **Only-filler**: never move/resize/delete cells, macros, blockages. Cells
are fixed VT boundaries.

FR-2 **Phase I — dirty refill**: for each maximal run of dirty fillers (merged
across rows into a rectangle when identical), delete and refill the footprint so
the marked spacing/min-width violation is removed, by restoring implant (VT)
continuity with the fixed neighbors.

FR-3 **Exact-fill (fitGap)**: a window must be tiled exactly by library masters;
because there is no 1-site filler, no residue gap may be left (gap 9 → 4+3+2,
never 8). Unfillable → unsolved.

FR-4 **preserveUserOrder**: filler selection follows the user-given master order
when set; otherwise widest-first.

FR-5 **Multi-height**: identical per-row windows stacked over consecutive rows
become one rectangular window; a height-h filler spans h rows as one instance.
Non-rectangular dirty regions fall back to per-row (height-1).

FR-6 **Phase II — VT replacement for MW/MS**: treat each filler site as a VT
variable and each cell site as fixed; choose filler VTs to **minimize** the 2D
MW/MS violation count; apply by delete+create with the new VT. No move.

FR-7 **Maximize, don't require全清**: the objective is to **minimize residual**
violations, not to fully clear or fail. Residual (no master for needed VT,
cell-blocked, would create new violations) → unsolved → upstream.

FR-8 **Realizability**: any chosen VT/width layout per row must be exact-fillable
by available masters; otherwise that choice is rejected/reverted.

---

## 6. Algorithms

### 6.1 Phase I — `FillerRepair`
1. Scan each row for maximal `DirtyFiller` runs; record `(row, col0, width,
   left_vt, right_vt)` where left/right VT come from the fixed neighbors.
2. Merge identical runs in consecutive rows → rectangular `MultiWindow`.
3. Per window: `clearSite` the dirty block, then `solveWindow`:
   - build VT plans (extend left VT / extend right VT / `L|R` split / isolated
     any-VT, with min-width guard);
   - partition window height into stripe heights (`partitionHeight`);
   - per (segment × stripe) `packExact` the width with height-matched masters;
   - first plan that tiles wins → emit `placeFiller`s; else → unsolved.

### 6.2 Phase II — `FillerVtRepair` (replace filler VT)
1. Read grid → `vt[r][c]`, `present[r][c]` (Cell|CleanFiller), `filler[r][c]`
   (changeable = CleanFiller).
2. **2D MW/MS evaluator** `countViolations`:
   - MW: a present site is OK iff its same-VT **horizontal OR vertical run** ≥
     ωMW; otherwise it is a narrow neck → +1 (catches 1-wide inter-row
     staircases).
   - MS: two orthogonally-adjacent present sites of **different VT** → +1 (when
     ωMS ≥ 1, different implant regions may not touch).
3. **Greedy coordinate descent** over filler sites: for each filler site pick the
   library VT minimizing a combined cost `violations*1000 − sameVTadjacencies`.
   The second term (merge) breaks single-site plateaus where a change only pays
   off together with a neighbor change.
4. **Apply**: for each maximal same-VT filler run whose VT changed, `exactFill`
   with masters → `clearSite` + `placeFiller`; untileable → revert (residual).
5. `violations_after` = residual; report it.

> Tier-2 (future): a column-DP (paper "Toward Optimal Filler Cell Insertion",
> Algorithm 4, generalized to a few rows) for patch-optimal assignment. The
> greedy already covers staircase-MW and inter-row-MS cases.

---

## 7. Interfaces & data model (portable)

The algorithm only touches the DB through `FillerGrid`:
```cpp
class FillerGrid {
  int numRows(); int numCols(int row);
  SiteKind kindAt(int r,int c); Vt vtAt(int r,int c);   // read
  void clearSite(int r,int c);                          // delete dirty
  void placeFiller(const PlacedFiller&);                // create one inst
};
```
Coordinates are **sites/rows**, never DBU; the adapter owns DBU geometry and the
`(vt,width,height) → master` mapping. Two backends share the same algorithm:
- **dpl** (`DplFillerGrid`, real odb) — built into `openroad`, command
  `repair_dirty_fillers`;
- **portable** (`FakeFillerGrid` + `FillerGridAdapter.example.h` in `dpl2/`) —
  copy & implement `FillerGrid` against another database.

---

## 8. Behavior catalogue (site-grid cases)

| # | Situation | Action |
|---|---|---|
| 1 | dirty between same-VT cells | refill single VT (Phase I) |
| 2 | dirty between different-VT cells | VT split `L\|R` (Phase I) |
| 3 | dirty next to clean filler | clean is fixed boundary; only dirty refilled |
| 4 | isolated dirty (empty sides) | any VT; needs ≥ ωMW else unsolved |
| 5 | window not exact-fillable | unsolved |
| 6 | multiple dirty windows / row | independent |
| 7 | rectangular multi-row dirty | one multi-height filler |
| 8 | non-rectangular dirty | per-row fallback |
| 9 | inter-row MW staircase (filler) | replace fillers to widen same-VT (Phase II) |
| 10 | inter-row MS touch (filler) | replace one side's VT to merge/separate (Phase II) |
| 11 | needs cell move (LEF Fig 3-1 "B") | unsolved → upstream |
| 12 | needed VT absent in library | unsolved → upstream |

---

## 9. Constraints & assumptions

- No 1-site filler ⇒ exact-fill is mandatory (FR-3).
- Upstream **contract**: if a repair needs a neighboring filler too (e.g. merge
  two 1-site to clear min-width), upstream must also mark that filler dirty so
  "only-dirty" stays self-sufficient.
- `avoid_abutment_patterns {1:1}` currently inactive (no 1-site filler) —
  deferred.
- The implant model is **site-grid**, not polygon DRC.

---

## 10. Verification

- Phase I core (`dpl2/test/filler_repair_test.cpp`): **39/39** incl. exact-fill
  9→4+3+2, unsolvable 1-site, preserveUserOrder, VT split, min-width,
  multi-height (MH1–MH5).
- Phase II (`dpl2/test/filler_vt_repair_test.cpp`): **13/13** — staircase MW+MS
  fully fixed, inter-row MS merge, unfixable-without-VT residual, clean no-op,
  evaluator sanity.
- dpl adapter logic (`src/dpl/test/filler_repair_mock`): **11/11** vs an odb
  mock.
- Full `openroad` **builds** with all modules; `repair_dirty_fillers` runs
  end-to-end on a real LEF/DEF (dirty filler deleted, replacement created,
  `[INFO DPL-0206] placed 1, unsolved 0`).

Build commands (sandbox-friendly) in the porting / dpl docs.

---

## 11. Open items

- Phase II Tcl command (`repair_filler_mwms`) + real LEF/DEF integration test.
- Tier-2 column-DP for patch-optimal MW/MS.
- Per-VT ωMW/ωMS and ωMS > 1 (gap across empties) in the evaluator.
- Confirm DRC-data schema (§4.3) with the DRC team; confirm the upstream
  dirty-marking contract (§9).
