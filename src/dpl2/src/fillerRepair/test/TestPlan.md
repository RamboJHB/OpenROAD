# Test Plan — fillerRepair

Updated: 2026-07-18.

## Tiers

| Tier | Command | Boundary |
|---|---|---|
| Planner unit | `src/dpl2/src/fillerRepair/test/run_tests.sh` | 81 individually registered GoogleTests with private planner doubles |
| Production E2E | `src/dpl2/test/build_all.sh` | 6 GoogleTests; fake UDM data with supplied infra/checker and production engine/planner |
| Full CTest | `src/dpl2/test/CMakeLists.txt` | 87 discovered GoogleTests |
| Real-UDM compile gate | `cmake -DDPL2_TEST_USE_FAKE_UDM=OFF ...` | `dpl2_filler_repair_compile_check`: all supplied + production sources against real UDM headers |

Normal and AddressSanitizer runs are required. Production builds use
`-Wall -Wextra -Werror`.

On macOS the scripts set `ASAN_OPTIONS=detect_container_overflow=0` because
Homebrew's GoogleTest static archive is not sanitizer-instrumented; this avoids
a libc++ annotation mismatch during test discovery. AddressSanitizer remains
enabled for every project source and test execution.

## Production E2E composition

Included: production RepairInfrastructure, Grid/Network, fillerSetting, final
ImplantLayerChecker, FillerRepairEngine, internal FillerRepairPlanner and all search
stages (compile lists from `src/fillerRepair/sources.cmake`). Fake UDM
provides tech/library/row/cell data only.

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

Each case snapshots all fixture cell origins, masters, status and orientation
before/after precheck and requires equality. The tests do not pass target,
newMaster or candidate information to precheck, proving that scope separation.

## Required repair cases

- `init()` succeeds through the single production facade.
- Target new master is checked as an overlay with empty filler changes first.
- A clean target overlay returns success with an empty change list.
- Existing violating fixture returns the deterministic FH2 filler replacement.
- Output is an exact `FillerCellRecord` in `ipl::FillerChanges`.
- Repeated repair returns identical output.
- Physical UDM snapshots remain unchanged after each repair.
- Persistent checker init diagnostics (duplicated per result by the checker)
  never mark candidates illegal; repair still succeeds.
- A configured filler master absent from the Network fails `init()`.
- An empty `getFillerMasters()` allow list errors out in both
  `RepairInfrastructure::build()` and `FillerRepairEngine::init()`.
- Before/after a failed `init()`, `precheck()` and `repair()` fail closed.
- The row-origin frame check baselines on the first NON-pad row: pad rows may
  sit anywhere; a misaligned standard row is refused even behind a pad row.
- Planner tests continue covering adaptive-L1, ranking, subset enumeration,
  budgets, cache, baseline-delta and malformed internal oracle protocol.
- `FR_VERBOSE=1` enables the deterministic `[fr][stage]` algorithm transcript;
  normal test runs remain silent apart from GoogleTest output.

## Commands

```sh
src/dpl2/src/fillerRepair/test/run_tests.sh
SANITIZE=address src/dpl2/src/fillerRepair/test/run_tests.sh
src/dpl2/test/build_all.sh
SANITIZE=address src/dpl2/test/build_all.sh

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

2026-07-18 results: planner 81/81 normal and ASan; production E2E 6/6 normal
and ASan; full CTest 87/87 normal and ASan; Werror clean; compile-check mode
configured and built against the UDM-compatible headers.

## Regression rules

- Repair must never invoke placement precheck.
- Precheck diagnostics may be warnings, but gap/overlap must set
  `isLegal=false` so opto can block.
- Neither public API mutates the DB.
- Production signatures must not expose planner requestId/status types.
- No planner fake may enter a production/E2E link target.
- Switching fake/real UDM must be an include/link-only CMake change.
- Production delivery contains fillerRepair only; supplied infra/checker must
  remain at zero diff.
