# Portable fillerRepair tests

This directory moves with `fillerRepair/` and contains 116 portable tests:
82 database-free planner unit tests plus 34 final-checker/precheck E2E tests.
No source below this directory includes the repository's fake UDM tree or
depends on its `E2ETestProvider`.

The same-level `FillerRepairPlannerTest.cpp`, `PlannerTestDataSource.h`,
`PlannerTestOracle.*` and `SyntheticMasterCatalog.*` sources own all planner,
window, ranker, subset-search and OracleGate cases. They compile against the
destination's real wire types but construct no UDM
objects.

`FillerRepairCheckerE2ETest.cpp` has 34 cases:

- five final-checker overlay contract cases: intra/inter-row WIDTH/SPACING
  plus target-related detection when a changed neighbor is outside the guard;
- 17 end-to-end cases that pass checker snapshots to
  `internal::FillerRepairPlanner`, apply its returned overlay to the final
  checker, and cover clean/repair/failure, deterministic batching, candidate
  and budget boundaries, third-VT reachability, baseline consistency,
  same-size changes, new-violation rejection and multi-swap minimum width. The
  three-swap case also verifies opposite-side adaptive fallback when the best
  residual initially points at a blocked side;
- 12 UDM-free internal precheck cases for exact coverage, leading/middle/
  trailing gaps, overlaps, legal holes, clipping, multiple rows, empty spans
  and deterministic mixed diagnostics, plus empty placement, unordered input,
  triple overlap and touching legal spans.

The E2E source uses no fake checker, fake placement view or fake UDM data. The
small `PortablePlannerDataSource` in the source is only the planner projection
of the same `ImplantInput` owned by `ImplantLayerCheckerHelper`.

## Destination CMake

The standalone CMake works when given the destination UDM includes/targets:

```sh
cmake -S fillerRepair/test -B build-filler-repair-e2e \
  -DDPL2_UDM_INCLUDE_DIRS='<real UDM includes>' \
  -DDPL2_RUNTIME_LIBRARIES='<existing infra/checker targets>' \
  -DDPL2_UDM_LIBRARIES='<real UDM targets/libraries>'
cmake --build build-filler-repair-e2e
ctest --test-dir build-filler-repair-e2e --output-on-failure
```

The CMake project builds the 82-case planner executable and the 34-case E2E
executable. The latter always compiles `${DPL2_FILLER_REPAIR_SOURCES}`, including
the public runtime engine. If the destination already has owning dpl2/checker
targets, pass them through `DPL2_RUNTIME_LIBRARIES`; otherwise the standalone
fallback compiles the adjacent supplied sources. The test cannot pass merely
by compiling the planner while the engine/real-UDM boundary is broken.

Only the 91-case checker/engine suite and its fake UDM fixture/provider remain
outside the migration payload under `src/dpl2/test/local/`.
