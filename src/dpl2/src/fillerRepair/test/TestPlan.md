# Test Plan — portable fillerRepair E2E

Updated: 2026-07-19.

## Migration gate

Copy `fillerRepair/` next to the destination's existing `infrastructure/` and
`drc/` directories. The destination must already provide the final
`ImplantLayerChecker`, `ImplantLayerCheckerHelper`, Grid/Network sources and
real UDM include/link configuration. No DEF/LEF fixture or provider source is
needed.

The 33 portable GoogleTests cover:

1. final-checker intra-row minimum-width overlay acceptance/rejection;
2. final-checker inter-row minimum-width overlay acceptance/rejection;
3. final-checker intra-row minimum-spacing overlay acceptance/rejection;
4. final-checker inter-row minimum-spacing overlay acceptance/rejection;
5. planner repair of each of those four violation classes using the real
   checker as the oracle, plus an already-clean empty repair;
6. deterministic repeated repair and invariant results across checker batch
   sizes, including batch size one;
7. empty candidate universes, third-VT-only reachability and exhausted checker
   budgets, all with no partial changes returned;
8. baseline-delta rejection for fabricated and duplicate original violations,
   and rejection of candidates that create a known new guard violation;
9. returned filler records preserve instance geometry/master size;
10. a minimum-width repair that requires two atomic swaps;
11. a checker-legal three-swap overlay beyond the current adaptive window,
    locking the current safe failure/no-partial-result behavior;
12. the extracted exact-coverage sweep behind `precheck()`: clean coverage,
    leading/middle/trailing gaps, coalesced overlaps, excluded legal holes,
    clipping, multi-row order, empty spans, empty placement, unordered input,
    triple coverage, touching legal spans and mixed deterministic findings;
13. no placement mutation by checker overlay queries or planner repair.

Each dense fixture has eight rows and 200 sites per row. Each direct overlay
test evaluates at least three candidates: clean repair, unresolved violation,
and repair that creates a new violation.

## Required commands

```sh
cmake -S fillerRepair/test -B build-e2e \
  -DDPL2_UDM_INCLUDE_DIRS='<real UDM includes>' \
  -DDPL2_UDM_LIBRARIES='<real UDM targets/libraries>'
cmake --build build-e2e
ctest --test-dir build-e2e --output-on-failure

cmake -S fillerRepair/test -B build-e2e-asan \
  -DDPL2_ENABLE_ASAN=ON \
  -DDPL2_UDM_INCLUDE_DIRS='<real UDM includes>' \
  -DDPL2_UDM_LIBRARIES='<real UDM targets/libraries>'
cmake --build build-e2e-asan
ctest --test-dir build-e2e-asan --output-on-failure
```

Both configurations compile with `-Wall -Wextra -Werror` and C++20.

## Separate local regression

The 81 pure-planner tests, their fake checker/view, fake UDM headers and the
64 production-facade cases are retained under
`src/dpl2/test/local/`. They do not move with `fillerRepair/` and are not linked
by the portable E2E target. Twelve of those 64 instances exercise the public
`FillerRepairEngine::precheck()` boundary in opto-style external call order:
stable repeated calls, hard blocking without mutation, simultaneous gap plus
overlap diagnostics, and proof that `repair()` does not invoke precheck
implicitly. The same four behaviors run across all three local layouts.
