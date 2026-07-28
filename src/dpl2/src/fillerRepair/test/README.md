# Portable fillerRepair tests

Updated: 2026-07-28.

These move with `fillerRepair/` and are the migration gate: **146 tests**, none
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

## 86 planner cases — `RepairPlannerTest.cpp`

Database-free. `TestPlacementView.h`, `TestRepairOracle.*` and
`SyntheticMasterCatalog.*` implement the two seams (`PlacementView`,
`RepairOracle`) with no database behind them, which is what makes the pure
pipeline portable on its own (`dpl2::fillerRepairPlanner`, C++17).

Coverage: swap validity and candidate filtering; synthetic master metadata;
L0 window construction and adaptive expansion (including opposite-side
fallback when the primary side is blocked); ranking into filler domains;
subset enumeration and member caps; the oracle-gate protocol and its error
paths; both budgets; determinism; guard quantization; and cache invariants.

## 60 real-checker cases — `FillerRepairCheckerE2ETest.cpp`

Drive the **real `ImplantLayerChecker`** over `ImplantLayerCheckerHelper`-built
input. No fake checker, no fake placement view, no fake UDM. The small
`PortablePlacementView` in the source is only the planner's projection of the
same `ImplantInput` the helper owns.

- **26 direct checker overlay cases** — intra/inter-row WIDTH and SPACING
  accept/reject, plus target-related detection when the changed neighbour sits
  outside the guard (proving the checker uses its own snapshot and rule reach).
- **34 planner-to-checker cases** — the planner takes a checker snapshot,
  returns an overlay, and that overlay is re-verified by the checker.
  Clean/repair/failure, batching invariance down to batch size one, empty
  candidate universes, third-VT reachability, baseline consistency, budget
  exhaustion with no partial changes, multi-swap minimum width, and the cached
  baseline freeing window budget.

Each dense fixture is 8 rows x 200 sites. The density matrix runs every
width/spacing class at target-local filler:std ratios of 50:50, 30:70, 20:80,
10:90 and 5:95, holding implant geometry and the minimum required
editable/bridge fillers fixed while redistributing the rest.

## Fixture invariants

Four properties of the current checker; breaking any of them silently changes
what these cases test. They are documented with their consequences in
`../README.md` ("Portable fixture model"): rule and layer ids **are** container
indices, the band-polarity model, `getSnapshot` spanning
`colId ± maxRuleValue_` sites, and min width applying to every run.
