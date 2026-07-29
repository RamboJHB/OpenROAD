# Portable fillerRepair tests

Updated: 2026-07-29.

These move with `fillerRepair/` and are the migration gate: **160 tests**, none
of which construct a UDM object, include the repository's fake UDM tree, or
need a fixture provider from the destination. They run before a design exists.

```sh
cmake -S <srcroot>/fillerRepair -B build-fr -DDPL2_FILLER_REPAIR_BUILD_TESTS=ON \
      -DDPL2_UDM_INCLUDE_DIRS='<real UDM includes>' \
      -DDPL2_UDM_LIBRARIES='<real UDM targets/libraries>' \
      -DDPL2_RUNTIME_LIBRARIES='<existing infra/checker targets>'
cmake --build build-fr && ctest --test-dir build-fr --output-on-failure
```

With `DPL2_RUNTIME_LIBRARIES` empty the E2E compiles the adjacent
infrastructure/checker sources itself, so the suite still runs before any
destination wiring exists.

## 85 planner cases — `RepairPlannerTest.cpp`

Database-free. The two seam doubles and the synthetic master catalog are folded
into that same file — it is self-contained. They implement the two seams
(`PlacementView`, `RepairOracle`) with no database behind them, which is what
makes the pure pipeline portable on its own (`dpl2::fillerRepairPlanner`,
C++17).

Coverage: swap validity and candidate filtering; synthetic master metadata;
L0 window construction and adaptive expansion (including opposite-side
fallback when the primary side is blocked); ranking into filler domains;
subset enumeration and member caps; the oracle-gate protocol and its error
paths; both budgets; determinism; guard quantization; and cache invariants.

## 75 real-checker cases — `FillerRepairCheckerE2ETest.cpp`

Drive the **real `ImplantLayerChecker`** over `ImplantLayerCheckerHelper`-built
input. No fake checker, no fake placement view, no fake UDM. The small
`PortablePlacementView` in the source is only the planner's projection of the
same `ImplantInput` the helper owns.

### The fixture: 7 rows x 200 sites, legal as built

Columns march in same-VT pairs — a std cell then a filler, cycling F1/F2/F3 —
so every implant run is exactly `MIN_RULE` (two sites) wide and the next run of
that family starts four sites later. Rows alternate R0 / MX. **Nothing is
planted**: the layout the checker is handed has no violation in it, which is
what makes the cases below evidence about the code rather than about the
fixture.

### A scenario is one thing opto does

Take one std cell and give it the VT of the pair on its right. That is the
whole edit, and it produces:

- the retargeted cell's own run collapses to one site — **min width**;
- it lands one site from the run it was meant to join — **min spacing**;
- both on the new family's N band and on its P partner, intra-row and across
  the row boundaries.

Ten violations from one realistic change. The repair is the filler between the
two runs — the **bridge**: recolour it to the new VT and they merge into one
four-site run.

Four scenarios place that edit at (row 3, site 50), (row 2, site 70) for the
other row parity, (row 3, site 4) against the left edge of the core, and
(row 3, site 150). Every target sits on an interior row of the seven, so each
case carries real rows of context above and below — enough for the guard
(window ± two rows) and for an adaptive step before it clamps. One further
pre-existing violation is planted at (row 0, site 190), far from all four, to
prove the checker never surfaces an unrelated finding on another target.

The density matrix reruns the checker-level cases at target-local filler:std
ratios of 50:50, 30:70, 20:80, 10:90 and 5:95. Only the **filler identity** of
a site changes, never its implant geometry, so the violations are identical at
every ratio and what moves is the size of the editable universe.

### What the cases assert

- **Checker level** — the built layout is clean before the edit; the edit
  produces width *and* spacing on both bands; the bridge swap clears every one
  of them; a same-size swap with the wrong VT clears none; a batch answers each
  candidate on its own merits, correlated by input order; and a violation is
  still detected when the changed filler lies outside the guard.
- **Repair window** — the L0 window is exactly five sites and three rows, holds
  nine editable fillers, reaches the bridge and excludes the retargeted std
  cell; the guard is the quantized power-of-two ring around the anchor and
  clears `getMaxRuleValue()` on both sides; one adaptive step strictly grows
  the editable universe without walking off the design.
- **Planner to checker** — every scenario repairs and re-verifies clean, at
  every density, with the input left untouched; the answer is the single
  bridge swap; determinism; batching invariance down to batch size one; an
  empty and a single-master candidate universe; baseline-consistency gates;
  both budgets, including the cached baseline that frees window budget; what
  `xWindow` means for width and for spacing; and a wide minimum width that
  cannot be met by fewer than two, then three, atomic swaps.

## Fixture invariants

Five properties of the current checker; breaking any of them silently changes
what these cases test. They are documented with their consequences in
`../README.md` ("Portable fixture model"): rule and layer ids **are** container
indices, the band-polarity model, `getSnapshot` spanning
`colId ± maxRuleValue_` sites, what `xWindow` means per rule kind, and min
width applying to every run.
