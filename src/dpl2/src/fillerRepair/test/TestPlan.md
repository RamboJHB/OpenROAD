# Test Plan — portable fillerRepair E2E

Updated: 2026-07-19.

## Migration gate

Copy `fillerRepair/` next to the destination's existing `infrastructure/` and
`drc/` directories. The destination must already provide the final
`ImplantLayerChecker`, `ImplantLayerCheckerHelper`, Grid/Network sources and
real UDM include/link configuration. No DEF/LEF fixture or provider source is
needed.

The eight portable GoogleTests cover:

1. final-checker intra-row minimum-width overlay acceptance/rejection;
2. final-checker inter-row minimum-width overlay acceptance/rejection;
3. final-checker intra-row minimum-spacing overlay acceptance/rejection;
4. final-checker inter-row minimum-spacing overlay acceptance/rejection;
5. planner repair of each of those four violation classes using the real
   checker as the oracle;
6. baseline-delta rejection of a candidate that fixes the target but creates
   a new violation elsewhere in the guard;
7. filtering of an unrelated pre-existing guard violation;
8. no placement mutation by either checker overlay queries or planner repair.

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
historical 52 production-facade cases are retained under
`src/dpl2/test/local/`. They do not move with `fillerRepair/` and are not linked
by the portable E2E target.
