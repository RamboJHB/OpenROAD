# fillerRepair handoff

This document describes the current integration and migration work. The
behavioral contract is in [the specification](../../docs/filler_vt_overlay_repair_spec.md).
Use the current branch sources together; historical port tags and previous
verification counts are available in Git history.

## Current implementation

- The checker accepts one temporary standard-cell `Node` through
  `CheckRequest`. Request `x/y/orientation` override the temporary node's
  stored placement.
- `isLegal` replaces one committed standard cell at its current origin with
  the same footprint. Buffer insertion replaces one or more intersecting
  fillers; every target site must be covered by those selected fillers.
- Exact cover uses the filler-swap planner. Non-exact cover tiles exactly
  `deleted filler sites - target sites`, then checks the added fillers and
  surrounding swaps through the real ImplantLayerChecker.
- The engine/checker returns filler `Add` and/or `Replace` records. Input
  filler `Delete` overlays remain caller-owned. Failure returns no partial
  repair.
- DePlace assembles complete transactions: `isLegal` appends the target
  `Replace`; `findLeg` appends the target `Add`, selected filler `Delete`
  records and filler repairs. Pass that result to `commit` once without
  appending the target or Deletes again.
- Probes do not mutate DB, Grid or Network. An existing output-vector prefix
  is preserved but is not a placement overlay for later probes. Independently
  accepted probes are not jointly DRC-validated.
- `DePlace::commit` handles Add/Replace/Delete, preserves std-cell identity
  for Replace, and synchronizes DB placement, Network lookup maps and Grid
  occupancy/padding. It checks final occupancy and feasibility, not DRC.

The engine API remains:

```cpp
explicit FillerRepairEngine(const ipl::ImplantLayerChecker& checker);
bool isReady() const;
RepairOutcome repair(const ipl::CheckRequest& request) const;
```

## Ownership and call sequence

DePlace owns Design references, Grid, Network and fillerSetting. The checker
borrows those objects and lazily owns one engine, initialized with
`std::call_once`. The engine obtains the same objects from the checker.

Before publishing checker workers:

1. Configure the final fillerSetting and import the complete master catalog,
   including masters without placed instances.
2. Complete Network creation, Grid initialization and occupancy painting.
3. Call `initPlacementDRC()`. Its first step,
   `Network::updateFillerClassification(*filler_setting_)`, sets every
   Master's filler flag from configured LibCellIDs and every Node's type from
   its Master.
4. Register the available checker set. The current repository registers one
   ImplantLayerChecker; other destination checker registrations remain
   commented because their implementations are not included here.

`importDb()` imports placement data; the default DePlace constructor creates
the checker after Grid initialization. An owner using the explicit
`DePlace(PhysDesMgr*)` setup path must complete the same sequence.

```text
DePlace::isLegal / findLeg
  -> PlacementDRC::checkDRC(temp, x, y, orientation, changes, overlayChanges)
  -> every registered checker
  -> ImplantLayerChecker::check
  -> lazy FillerRepairEngine::repair(CheckRequest), when needed
  -> ImplantLayerChecker::checkPlaceWithOverlays
  -> DePlace assembles the complete accepted transaction
  -> caller serializes DePlace::commit against placement readers
```

The ordinary checkDRC overload is direct-only. The two-vector overload stages
changes locally and publishes them only when all registered checkers pass.
Non-exact coverage invokes repair even if direct implant DRC is already legal,
because released sites still need fillers.

## Lifetime and configuration

Master geometry, VT/compatibility metadata, checker rules, Grid geometry and
filler settings are setup data. Rebuild checker/engine when those inputs
change. Ordinary placement commits do not require rebuilding them.

Every repair reads current nodes, masters, coordinates, orientation, DB IDs
and names through request-local placement caches. Concurrent read-only repairs
are supported; DB/Grid/Network mutation must not overlap them. Commit must
finish synchronizing all three before subsequent repair calls.

Configured filler order controls Add choices when follow-order is enabled.
Avoid patterns are absolute positive site-width pairs: `1:2` forbids
horizontal abutment of a one-site and two-site filler, in either order, in
any shared row. This policy constrains new Adds and their filler neighbors;
it does not retile pre-existing forbidden pairs. Site/orientation checks
precede Add ranking.

Generated filler names append
`FILLER_REPAIR_<row>_<column>_W<width-DBU>_H<height-rows>_<add-index>`
to the configured prefix, inserting one separator underscore only when the
prefix does not already end in one. An empty or absent prefix uses
`FILLER_REPAIR_` directly.
A deterministic suffix avoids live-name and same-candidate Add collisions.
DePlace findLeg uses `DPL_STD_<row>_<column>_M<LibCellID-index>`, also with
collision suffixes. CellChangeRecord coordinates are core-relative DBU;
commit converts them to absolute DB coordinates.

## Files to migrate

Copy `src/dpl2/src/fillerRepair2/` to the destination's
`src/dpl2/src/fillerRepair/`. It contains the engine, planner, retiler,
internal headers, CMake and two GoogleTest sources. The local harness checks
the shared implementation files against the exercised fillerRepair sources
and compiles the copy under its destination directory name.

| Destination touch point | Required integration |
| --- | --- |
| `drc/ImplantLayerChecker.h/.cpp` | CheckRequest, overlay validation/Add support, lazy engine dispatch and batch oracle |
| `drc/DRCChecker.h`, `PlacementDRC.h/.cpp` | Two-vector interface and publication only after every checker passes |
| `DePlace.h/.cpp`, `Place.cpp` | Temporary-node probes, complete transaction assembly and commit |
| `dbToOpendp.cpp` | Full master import and checker initialization after Grid setup |
| `infrastructure/network.h/.cpp`, `Objects.h` | Stable IDs/maps, filler classification and CellChangeRecord wire |
| `infrastructure/fillerSetting.h/.cpp` | Configured master IDs/order/prefix and site-width avoid patterns |
| `dpl2ui/testFillerRepairCmd.cc/.hh` | Optional direct checker/commit test command |
| `drc/ImplantLayerCheckerHelper.h/.cpp` | Required by the helper-based integration GoogleTest and dump replay |

Use only missing destination changes at these touch points; preserve the
destination's existing checker implementations and registration. Do not copy
`src/dpl2/local/`, `src/dpl2/test/local/`, fake UDM headers or build
directories.

The minimal parent CMake connection is:

```cmake
add_subdirectory(src/dpl2/src/fillerRepair)
target_link_libraries(dpl2Lib PRIVATE dpl2::fillerRepair)
```

The payload compiles `RepairPlanner.cpp`, `FillerRetiler.cpp` and
`FillerRepairEngine.cpp` at C++20. If needed, define the parent-owned
`dpl2_filler_repair_deps` interface target first for UDM, infra, checker and
TBB include/link dependencies. With `UV_BUILD_TESTS`, the payload contributes
both test files as an object library to the destination's UNIT_TESTS
collection; the parent provides GoogleTest and its executable.

## Verification

Run the complete CMake build, which includes the commit/command executable
and compile-only destination wiring as well as the planner/checker tests.
These are the commands used for this validation; the explicit ASan compiler
and SDK paths apply to the current macOS host:

```bash
cmake -S src/dpl2/test -B src/dpl2/test/build/doc-sync-normal \
  -DCMAKE_BUILD_TYPE=Debug -DDPL2_TEST_USE_FAKE_UDM=ON -DDPL2_ENABLE_ASAN=OFF
cmake --build src/dpl2/test/build/doc-sync-normal --parallel 6
ctest --test-dir src/dpl2/test/build/doc-sync-normal --output-on-failure --parallel 6

cmake -S src/dpl2/test -B src/dpl2/test/build/doc-sync-asan-clang21 \
  -DCMAKE_BUILD_TYPE=Debug -DDPL2_TEST_USE_FAKE_UDM=ON -DDPL2_ENABLE_ASAN=ON \
  -DCMAKE_CXX_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0
cmake --build src/dpl2/test/build/doc-sync-asan-clang21 --parallel 6
ctest --test-dir src/dpl2/test/build/doc-sync-asan-clang21 --output-on-failure --parallel 6
```

The gate covers planner/internal cases, unchanged checker golden expectations,
one/two-row targets, 50/75/90% integration utilization, exact/non-exact
multi-filler coverage, order/avoid patterns, bounded failures, concurrency,
zero probe mutation, complete probe/commit transactions and engine reuse
after commit. FillerRepair and migration payload targets use
`-Wall -Wextra -Werror`; delivered infra/checker sources retain visible
warnings without treating all of them as errors. The pure planner also
compiles at C++17.

On macOS the harness disables ASan container-overflow annotations because
Homebrew GoogleTest is an uninstrumented static library; other configured
AddressSanitizer checks remain enabled.

The current macOS 27 host's Xcode clang 17 ASan runtime deadlocks during
initialization, before main, even for a minimal program. The installed Command
Line Tools clang 21 with its matching SDK runs that program successfully;
the ASan validation therefore uses that toolchain without changing xcode-select.

Verification on 2026-10-19, from fresh build directories:

- Normal: 419/419 passed with AppleClang 17 (CTest time 3.37 seconds).
- ASan: 419/419 passed with Command Line Tools AppleClang 21 (23.43 seconds).
- Both builds compiled every default target, including commit/command tests,
  destination wiring and the migration-copy gate. `git diff --check` passed.

These results use the test-only fake UDM provider; they do not establish
real UDM ABI compatibility or source-ODB writeback.

## Remaining destination work and limits

- Map `DePlace::commit` Add/place operations to real UDM APIs. The local
  `UnlChange_addCell/placeCell` signatures and error behavior are test
  contracts, not verified real UDM declarations. Confirm CellChangeRecord
  field/ID variants and preserve target connectivity on Replace.
- Run isLegal, findLeg, mixed commits and subsequent repairs on a real UDM
  design with the complete destination checker set. Local registration does
  not validate the missing checkers' interaction with filler changes.
- Provide the commit/read barrier and keep DB, Grid and Network synchronized.
  The preflight rejects ordinary invalid batches before mutation; commit has
  no rollback journal for allocation failures or unexpected UDM exceptions.
- The local OpenROAD importer projects ODB into fake UDM. Its commits modify
  that projection, not the source ODB or a later write_def. Source-ODB writeback
  remains separate work.
- Existing std-cell movement/footprint changes and buffer placement over empty
  sites are outside the probe contract. The new buffer is geometrically added;
  real opto net connections still need destination integration.
- Search is bounded: defaults include 16 tilings, 100000 geometric states,
  512 checker candidates per planner window and 2048 per repair. Any returned
  solution must pass the checker. If none is found within the caps, the result
  has no solution and no partial changes; truncation does not prove infeasibility.
