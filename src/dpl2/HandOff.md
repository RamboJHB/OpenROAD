# HandOff — filler VT overlay repair

Updated: 2026-07-18. Branch: `claude/wizardly-carson-secahu`.

## Result

The destination already supplies complete infrastructure and checker sources.
Production migration therefore copies only `src/dpl2/src/fillerRepair/`
(which now contains `RepairInfrastructure.{h,cpp}` as well) and optionally
`src/dpl2/test/` for the harness, then adds the `sources.cmake` list to the
destination's existing target. No infrastructure/checker source or API change
is required, and this change set keeps both directories at zero diff.

The production boundary is now one checker-style class:

```cpp
FillerRepairEngine(Grid* grid, Network* network);
void setDebugLogging(bool enabled);  // optional, disabled by default
bool init(PhysDesMgr* desMgr,
          const ImplantLayerChecker* checker,
          const fillerSetting* fillerSetting);
ipl::CheckResult precheck() const;
RepairOutcome repair(LeafCellID targetCell, const PhysLibCell& newMaster);
```

The production view/oracle/wire conversion lives in `FillerRepairEngine.cpp`;
the pure search pipeline is `internal::FillerRepairPlanner`. `init()` must succeed
before use: until it does, `precheck()` and `repair()` fail closed
(`precheck_not_initialized` / `engine_not_initialized`).

## Required opto sequence

1. Construct engine with the current Grid/Network and call `init()`.
2. Before any cell mutation, call `precheck()`.
3. If `precheck().isLegal == false`, stop. `Gap`/`Overlap` diagnostics are
   warnings for logging, but the bool is a hard blocking contract.
4. Call `repair(targetCell, newMaster)` while the DB still contains the old
   target master. `newMaster` is an overlay.
5. If repair succeeds, opto/infrastructure commits the returned
   `ipl::FillerChanges` together with its own target mutation.

fillerRepair provides the gate; it does not modify opto and does not commit.
`precheck()` and `repair()` are both non-mutating.

## Precheck scope

Precheck checks only placement gaps and overlaps over non-pad rows using
`PhysDesMgr` physical cells and the Network cell universe. It does not check:

- target or proposed master;
- master-size compatibility;
- candidate availability;
- Node/Master ID mapping;
- site alignment or a separate out-of-bounds category;
- implant DRC.

`isLegal=true` means no gap/overlap. `isLegal=false` means at least one
`Gap`/`Overlap` diagnostic and opto must block. `repair()` deliberately does
not call precheck again.

## Repair semantics

The first implant query overlays the target new master with an empty filler
change list. No violation returns success with empty changes. Violations enter
the unchanged adaptive-L1/ranker/subset/cache/budget/baseline-delta planner.
A clean solution returns `ipl::FillerChanges`; no solution returns failure and
empty changes. The final checker remains the only DRC oracle.

Optional debug logging is enabled with `engine.setDebugLogging(true)`. It emits
a deterministic `[fr][stage]` transcript for planner configuration,
normalization, window growth, swap generation, ranking, enumeration,
checker/cache/budget activity and the final decision. It is disabled by
default and does not affect search behavior.

## Data authority

| Data | Authority |
|---|---|
| rows and physical placement | `PhysDesMgr` |
| cell/master topology and checker IDs | production Network |
| filler allow-list | `fillerSetting::getFillerMasters()` |
| VT family/band polarity | `PhysLibCell` implant shapes + checker layers |
| implant legality | final `ImplantLayerChecker` |
| commit | opto/infrastructure |

`RepairInfrastructure` registers placed, target-new and configured candidate
masters before checker/engine construction, including uninstantiated masters.

## Migration to the destination environment

What to copy (two directories, nothing else):

1. `src/dpl2/src/fillerRepair/` -> next to the destination's existing
   `infrastructure/` and `drc/` directories (the production sources include
   `infrastructure/...` and `drc/ImplantLayerChecker.h` relative to that
   common source root). This directory now contains the complete production
   delivery including `RepairInfrastructure.{h,cpp}`, plus `fake/` and
   `test/` subdirectories that are used ONLY by the standalone harness.
2. `src/dpl2/test/` -> anywhere (standalone CMake project); only needed to
   run the harness in the destination environment.

Production wiring (their CMake, 2 lines):

```cmake
include(<srcroot>/fillerRepair/sources.cmake)
target_sources(<owning-target> PRIVATE ${DPL2_FILLER_REPAIR_PRODUCTION_SOURCES})
```

`<owning-target>` is the target that already compiles Grid/Network/checker,
so its UDM include paths and libraries apply to our sources unchanged. The
only modifications that may be needed on their side:

- the common source root must be on the include path (theirs already is if
  `infrastructure/...`-style includes work today);
- C++17 or newer for the production sources (the harness builds them at 17
  for the planner and 20 for the chain).

Do not hand-copy file names -- the test harness includes the same
`sources.cmake`, so the two lists cannot drift. Do not add
`fillerRepair/fake/*`, `fillerRepair/test/*`, or a fake UDM include path to a
production target. No production source uses a fake/real UDM conditional; it
includes the real UDM names already used by infra/checker.

Harness wiring in the destination environment:

```sh
# 1) Compile gate against REAL UDM (no fake anywhere):
cmake -S <copied test dir> -B build-real \
  -DDPL2_TEST_USE_FAKE_UDM=OFF \
  -DDPL2_TEST_UDM_INCLUDE_DIRS='<real UDM include dirs>' \
  -DDPL2_TEST_UDM_LIBRARIES='<real UDM libs/targets>'
cmake --build build-real   # builds dpl2_filler_repair_compile_check

# 2) Full test run (fake UDM is the data provider; default mode):
cmake -S <copied test dir> -B build-test
cmake --build build-test && ctest --test-dir build-test
```

In real-UDM mode there is no runnable E2E (its DATA comes from fake UDM), so
the harness builds `dpl2_filler_repair_compile_check` instead: every supplied
and production source compiling and linking against the real UDM headers is
the migration gate. The harness expects the copied `test/` directory to sit
next to `src/` as in this repo (`../src` relative layout); if the destination
places it elsewhere, adjust `DPL2_SRC` at the top of `test/CMakeLists.txt` --
that is the only expected edit.

## Build and verification

The standalone test CMake is not a proposed production CMake file. It provides
one interface-only selection point:

- default: `DPL2_TEST_USE_FAKE_UDM=ON`, using the test-only UDM-compatible
  include root selected by `DPL2_TEST_FAKE_UDM_INCLUDE_DIR`;
- real UDM rehearsal: set it `OFF` and provide
  `DPL2_TEST_UDM_INCLUDE_DIRS` and/or `DPL2_TEST_UDM_LIBRARIES`.

Both modes compile the same infra/checker/fillerRepair sources. The fake uses
the real UDM namespaces, type names, signatures and placement behavior; only
include/link configuration changes.

Test dependencies: GoogleTest, Boost, TBB, C++20 and CMake 3.20+. Commands:

```sh
src/dpl2/src/fillerRepair/test/run_tests.sh
SANITIZE=address src/dpl2/src/fillerRepair/test/run_tests.sh
src/dpl2/test/build_all.sh
SANITIZE=address src/dpl2/test/build_all.sh

cmake -S src/dpl2/test -B src/dpl2/test/build-cmake
cmake --build src/dpl2/test/build-cmake -j2
ctest --test-dir src/dpl2/test/build-cmake --output-on-failure
```

All 81 planner cases and the 6 production E2E cases are GoogleTests. The E2E
uses fake UDM only as test data and links supplied Grid/Network,
RepairInfrastructure, final checker, FillerRepairEngine and internal planner.
It covers clean/gap/overlap precheck, opto-blocking return values,
deterministic repair, byte-equivalent physical snapshots before/after both
APIs, persistent-checker-diagnostic stripping, configured-master/Network
consistency, fail-closed behavior after failed init, and the first-non-pad-row
origin baseline.

2026-07-18 result: planner 81/81 normal and ASan; E2E 6/6 normal and ASan;
full CTest 87/87 normal and ASan; all targets passed Werror.

## Integration risks

- Opto must honor the explicit precheck ordering; repair has no fallback gate.
- Engine/Grid/Network/checker/PhysDesMgr must describe one design revision;
  rebuild after commit.
- Destination build must consume `sources.cmake`; nothing else is part of the
  production delivery.
- Real-UDM verification still depends on the destination providing its UDM
  include directories and link libraries/targets; no code port remains.
- Checker calls are serialized inside ONE engine because the checker const
  overlay path updates counters. Sharing one checker across several engine
  instances is NOT serialized -- use one checker per engine (or one engine per
  thread over its own checker).
- The public facade owns production translation; planner fake/checker types
  must remain outside production targets.
