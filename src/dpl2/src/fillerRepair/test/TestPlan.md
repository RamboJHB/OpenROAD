# Test Plan — fillerRepair

Updated: 2026-07-18.

## Tiers

| Tier | Command | Boundary |
|---|---|---|
| Planner unit | `src/dpl2/src/fillerRepair/test/run_tests.sh` | 81 individually registered GoogleTests with private planner doubles |
| Production E2E | `src/dpl2/src/fillerRepair/test/run_e2e_tests.sh` | 52 GoogleTests; fake UDM data with supplied infra/checker types and production engine/planner |
| Full CTest | `src/dpl2/test/CMakeLists.txt` | 133 discovered GoogleTests |
| Real-UDM compile gate | `cmake -DDPL2_TEST_USE_FAKE_UDM=OFF ...` | `dpl2_filler_repair_compile_check`: all supplied + production sources against real UDM headers |

Normal and AddressSanitizer runs are required. Production builds use
`-Wall -Wextra -Werror`.

On macOS the scripts set `ASAN_OPTIONS=detect_container_overflow=0` because
Homebrew's GoogleTest static archive is not sanitizer-instrumented; this avoids
a libc++ annotation mismatch during test discovery. AddressSanitizer remains
enabled for every project source and test execution.

## Production E2E composition

Included: production Grid/Network, fillerSetting, final ImplantLayerChecker,
FillerRepairEngine (borrowing that infrastructure), internal
FillerRepairPlanner and all search stages (compile lists from
`src/fillerRepair/sources.cmake`). Fake UDM provides tech/library/row/cell data
only. A test-only fixture performs the Grid/Network wiring that DePlace already
performs in production; no importer exists in the production engine.

`e2e_test.cpp`, `run_e2e_tests.sh`, this plan and the only test-only fake UDM
include tree live below `fillerRepair/test`, so an entire-directory port carries
the tests and data provider with the implementation. `sources.cmake` exports
`DPL2_FILLER_REPAIR_E2E_TEST_SOURCE`; the destination CMake only adds that
source and selects the support include root for its production-chain GTest.

Excluded: `fillerRepair/fake/*` and every other test double.

The fake UDM include tree mirrors real UDM namespaces, names, signatures and
fixture-visible behavior. `dpl2_test_udm` is an interface target that switches
only include/link configuration. With `DPL2_TEST_USE_FAKE_UDM=OFF`, real UDM
is selected through `DPL2_TEST_UDM_INCLUDE_DIRS` / `DPL2_TEST_UDM_LIBRARIES`;
in that mode the E2E (whose DATA is fake UDM) is replaced by the
`dpl2_filler_repair_compile_check` static library over the same source graph.
There is no fake-related production `#ifdef`, and infrastructure/checker source
files remain unmodified.

## Required precheck cases

1. Clean placement: `isLegal=true`, no diagnostics.
2. Gap: `isLegal=false`, at least one `Gap` warning diagnostic.
3. Overlap: `isLegal=false`, at least one `Overlap` warning diagnostic.
4. A gap entirely inside a hard blockage is outside the legal Grid segments
   and remains legal.
5. A gap entirely inside an instance halo/padding reservation is outside the
   legal Grid segments and remains legal.
6. With the same blocked row tail, a gap inside the remaining valid,
   unreserved segment still returns `isLegal=false` and `Gap`.

Each case snapshots all fixture cell origins, masters, status and orientation
before/after precheck and requires equality. The tests do not pass target,
newMaster or candidate information to precheck, proving that scope separation.

All six behaviors are each instantiated for Canonical, ShiftedOrigin and
FarShiftedOrigin: three independently discovered testcases per behavior.

## Required repair cases

- `FillerRepairEngine(Grid*, Network*)` reuses the supplied initialized
  infrastructure and owns only the final checker/view; callers construct no
  repair-specific infrastructure object.
- `init(PhysDesMgr*, fillerSetting)` accepts no leaf-cell list or target master.
- Target new master is checked as an overlay with empty filler changes first.
- A clean target overlay returns success with an empty change list.
- Existing violating fixture returns the deterministic FH2 filler replacement.
- Output is an exact `FillerCellRecord` in `ipl::FillerChanges`.
- Repeated repair returns identical output.
- Physical UDM snapshots remain unchanged after each repair.
- Persistent checker init diagnostics (duplicated per result by the checker)
  never mark candidates illegal; repair still succeeds.
- Every configured filler master, including an uninstantiated one, is
  registered in the existing Network by engine initialization.
- An uninstantiated target new master is absent after init, registered lazily
  by the first repair, and visible to the rebuilt checker/view.
- An empty `getFillerMasters()` allow list fails engine initialization and its
  reason is preserved in standard result diagnostics.
- A PhysDesMgr different from the UDM Session current design is rejected before
  private checker construction.
- Before/after a failed `init()`, `precheck()` and `repair()` fail closed.
- Null borrowed Grid/Network fails init with `missing_infrastructure`.
- A second `init()` is rejected without damaging the first ready snapshot.
- Frame gates: a trailing pad row (any origin) is accepted; a LEADING pad row
  is refused by the frame-coherence gate (`RowFrameMismatch`) because the
  checker's Grid frame skips pad rows while its init/track frame does not; a
  standard row off the shared origin frame stays refused
  (`RowOriginMisaligned`/`ColFrameMismatch`), with the origin baseline on the
  first NON-pad row.
- Planner tests continue covering adaptive-L1, ranking, subset enumeration,
  budgets, cache, baseline-delta and malformed internal oracle protocol.
- `FR_VERBOSE=1` enables the deterministic `[fr][stage]` algorithm transcript;
  normal test runs remain silent apart from GoogleTest output.

Every repair/init behavior above is also instantiated for the three shared-row
origins. Row-frame validation has four dedicated cases (aligned shifted,
pad-before-standard refused, pad-after-standard accepted, misaligned-with-pad
refused). Thus every E2E behavior owns at least three testcases, and every
fixture constructs at least five standard rows.

## Commands

```sh
src/dpl2/src/fillerRepair/test/run_tests.sh
SANITIZE=address src/dpl2/src/fillerRepair/test/run_tests.sh
src/dpl2/src/fillerRepair/test/run_e2e_tests.sh
SANITIZE=address src/dpl2/src/fillerRepair/test/run_e2e_tests.sh

cmake -S src/dpl2/test -B src/dpl2/test/build-cmake
cmake --build src/dpl2/test/build-cmake -j2
ctest --test-dir src/dpl2/test/build-cmake --output-on-failure

cmake -S src/dpl2/test -B src/dpl2/test/build-cmake-asan \
  -DDPL2_ENABLE_ASAN=ON
# macOS + Homebrew GoogleTest only; the two scripts above set this themselves:
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_container_overflow=0"
cmake --build src/dpl2/test/build-cmake-asan -j2
ctest --test-dir src/dpl2/test/build-cmake-asan --output-on-failure
```

2026-07-18 results: planner 81/81 normal and ASan; production E2E 52/52 normal
and ASan; full CTest 133/133 normal and ASan; Werror clean. Compile-check mode
is configured against the UDM-compatible headers.

## Regression rules

- Repair must never invoke placement precheck.
- Precheck diagnostics may be warnings, but gap/overlap must set
  `isLegal=false` so opto can block.
- Neither public API mutates the DB.
- Lazy master registration may extend Network's in-memory master registry but
  must not change any UDM physical record.
- Production signatures must not expose planner requestId/status types.
- No planner fake may enter a production/E2E link target.
- Switching fake/real UDM must be an include/link-only CMake change.
- Production delivery contains fillerRepair only; supplied infra/checker must
  remain at zero diff.
