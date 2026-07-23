# Test Plan — portable fillerRepair tests

Updated: 2026-07-22.

## Migration gate

Copy `fillerRepair/` next to the destination's existing `infrastructure/` and
`drc/` directories. The destination must already provide the final
`ImplantLayerChecker`, `ImplantLayerCheckerHelper`, Grid/Network sources and
real UDM include/link configuration. No DEF/LEF fixture or provider source is
needed.

The portable package contains 153 GoogleTests: 82 database-free planner unit
tests and 71 final-checker/precheck E2E tests. The planner matrix covers Swap,
candidate filtering, synthetic master metadata, window construction, ranking,
subset enumeration, OracleGate protocol/error handling, budgets, determinism,
adaptive expansion and end-to-end planner decisions. The 71 E2E tests cover:

1. final-checker intra-row minimum-width overlay acceptance/rejection;
2. final-checker inter-row minimum-width overlay acceptance/rejection;
3. final-checker intra-row minimum-spacing overlay acceptance/rejection;
4. final-checker inter-row minimum-spacing overlay acceptance/rejection;
5. target-related rule detection when the changed neighbor is outside the
   guard, proving the checker uses its complete snapshot/rule reach;
6. planner repair of each of those four violation classes using the real
   checker as the oracle, plus an already-clean empty repair;
7. deterministic repeated repair and invariant results across checker batch
   sizes, including batch size one;
8. empty candidate universes, third-VT-only reachability and exhausted checker
   budgets, all with no partial changes returned;
9. baseline-delta rejection for fabricated and duplicate original violations,
   and rejection of candidates that create a known new guard violation;
10. returned filler records preserve instance geometry/master size;
11. a minimum-width repair that requires two atomic swaps;
12. a checker-legal three-swap repair whose best residual initially selects a
    blocked adaptive side; the planner must try the opposite side, return all
    three atomic changes and pass final-checker verification;
13. the extracted exact-coverage sweep behind `precheck()`: clean coverage,
    leading/middle/trailing gaps, coalesced overlaps, excluded legal holes,
    clipping, multi-row order, empty spans, empty placement, unordered input,
    triple coverage, touching legal spans and mixed deterministic findings;
14. no placement mutation by checker overlay queries or planner repair;
15. exact target-local-window filler:standard-cell ratios of 50:50, 30:70,
    20:80, 10:90 and 5:95; all four direct checker overlay classes and all
    four planner-to-checker repair classes run independently at every ratio.

The checker fixtures use the accessor-based `Layer`/`Rule` model. The local
checker/engine matrix additionally exercises engine metadata extraction through
`Layer::TechLayerId`, so compilation cannot fall back to the removed
`ImplantLayer` fields or metadata joins by layer name.

The portable planner suite also pins `RepairConfig::maxAdaptiveLevels`:
reaching the cap ends a no-solution search with the TRUNCATED verdict (never
"definitive") and empty changes
(`planner.FillerRepairPlanner.planner_adaptive_level_cap_truncates`).

Each dense fixture has eight rows and 200 sites per row. A density window spans
20 columns and every valid target row +/-1: 40 sites for the boundary-row
intra-width case and 60 sites for the other three cases. Each direct overlay
test evaluates at least three candidates: clean repair, unresolved violation,
and repair that creates a new violation. The minimum required editable/bridge
fillers and remaining DRC-core context identities are locked; all other identities inside
each local window are redistributed uniformly while preserving implant
geometry, and every local ratio is asserted. The motivation, observed timing
and proposed algorithm evolution are recorded in
`docs/filler_repair_dense_placement_analysis.md`.

## Required commands

```sh
cmake -S fillerRepair/test -B build-e2e \
  -DDPL2_UDM_INCLUDE_DIRS='<real UDM includes>' \
  -DDPL2_RUNTIME_LIBRARIES='<existing infra/checker targets>' \
  -DDPL2_UDM_LIBRARIES='<real UDM targets/libraries>'
cmake --build build-e2e
ctest --test-dir build-e2e --output-on-failure

cmake -S fillerRepair/test -B build-e2e-asan \
  -DDPL2_ENABLE_ASAN=ON \
  -DDPL2_UDM_INCLUDE_DIRS='<real UDM includes>' \
  -DDPL2_RUNTIME_LIBRARIES='<existing infra/checker targets>' \
  -DDPL2_UDM_LIBRARIES='<real UDM targets/libraries>'
cmake --build build-e2e-asan
ctest --test-dir build-e2e-asan --output-on-failure
```

Both configurations compile the planner tests with C++17 and the complete
`${DPL2_FILLER_REPAIR_SOURCES}` (including `FillerRepairEngine.cpp`) E2E with
C++20, all under `-Wall -Wextra -Werror`. `DPL2_RUNTIME_LIBRARIES`
reuses destination infra/checker targets; omitting it selects the adjacent
source fallback.

## Separate local regression

The 82 planner tests and their database-free doubles are same-level sources under
`fillerRepair/test/` and move with the feature. The planner suite directly validates the internal
`PlannerOracle` protocol, including missing, duplicate, unknown and extra
`OracleResult` records; every batch-cardinality mismatch fails closed.

Only the 99 fake-UDM checker/engine cases, compatibility headers and runtime provider
remain under `src/dpl2/test/local/`. The suite exercises repeated public precheck calls and opto-style external
gating. Additional three-layout cases verify that `repair()` repeats
precheck and returns `PrecheckFailed`, that two-row hard macros supply coverage
on both their origin and upper rows while
hard blockages remove coverage requirements and soft blockages do not, that
invalid target/size/precheck requests do not register replacement masters, and
that `update()` never changes a stale Node mapping, rejects an unsynchronized
infrastructure revision, accepts it after the test's infrastructure fixture
synchronizes the Node, and leaves failed private snapshots fail-closed.
Three additional instances verify that a missing rule on a layer used by Network
masters makes `init()` fail closed; the existing three persistent-diagnostic
repair instances prove that missing rules on unused layers remain non-blocking.
Nine checker-entry instances (three layouts × three behaviors) prove that
`ImplantLayerChecker::check()` calls the engine with the exact request, exposes
a complete repair, returns empty changes for a clean candidate, and returns
false with no partial/stale changes when precheck blocks. All three preserve
the UDM physical snapshot.

Two local initialization-diagnostic cases make destination failures
actionable: one injects a non-uniform site width and requires both row/site
records plus Grid/checker/engine widths; the other injects conflicting
Node/physical filler classifications and requires every classification source
and configured-list membership in the returned diagnostics.

The whole test set is not UDM-independent: these 99 cases intentionally cover
Session/PhysDesMgr extraction, physical handles, filler-master lookup, Network
registration and refresh. Therefore the single test-only UDM-compatible
provider and its CMake include switch remain necessary. No fake header,
compile definition or conditional exists in runtime sources or the migration
payload.
