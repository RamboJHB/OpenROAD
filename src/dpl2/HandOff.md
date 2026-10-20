# fillerRepair handoff

This document describes the current integration and migration work. The
behavioral contract is in [the specification](../../docs/filler_vt_overlay_repair_spec.md).
Use the current branch sources together; historical port tags and previous
verification counts are available in Git history.

## Current implementation

- The checker accepts one temporary standard-cell `Node` through
  `CheckRequest`. Request `x/y/orientation` override the temporary node's
  stored placement.
- `isLegal` supports master size up/down and movement, retaining the original
  standard-cell ID and connections. It prepares removal/refill virtually,
  then reuses findLeg's diamond search. Buffer insertion replaces one or more intersecting
  fillers; every target site must be covered by those selected fillers.
- Same-footprint original-position probes retain the direct/swap fast path;
  removal preparation is deferred until that path fails or the footprint changes.
- Exact cover uses the filler-swap planner. Non-exact cover tiles exactly
  `(old std sites + deleted filler sites) - target sites`, then checks the added fillers and
  surrounding swaps through the real ImplantLayerChecker.
- The engine/checker returns complete filler `Delete`/`Add`/`Replace` records.
  FR owns cancellation/retiling of tentative preparation Adds and deduplication
  of real Deletes/swaps. No Delete is emitted for an uncommitted Add. The old
  std Delete is overlay-only. Failure returns no partial repair.
- DePlace assembles complete transactions: `isLegal` appends the target
  `Replace`; `findLeg` appends the target `Add` to FR's filler transaction.
  Pass that result to `commit` once without
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

`isLegal(inst, master, changes)` now permits resizing at the original position.
Use `isLegal(inst, master, va, diameter, changes)` to allow position search;
`diameter` is a nonnegative DBU extension, zero forbids movement, and `va` uses
absolute design coordinates. Records remain core-relative DBU. At most 256
positions are tried per relocation search rectangle.

Removal preparation is `CheckRequest{nullptr, ..., {oldStdDelete}}`; only
ImplantLayerChecker sees this intermediate request. A failed preparation does
not block the final search. Every final target candidate runs the full
registered checker set. FR rebuilds the released site union minus that target,
including disjoint old/new regions, against a committed-placement DRC baseline.
DePlace rejects final target/filler padding collisions before reporting success.

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
  -> ImplantLayerChecker::check
  -> lazy FillerRepairEngine::repair(CheckRequest), when needed
  -> ImplantLayerChecker::checkPlaceWithOverlays
  -> remaining checkers validate the final filler transaction read-only
  -> DePlace assembles the complete accepted transaction
  -> caller serializes DePlace::commit against placement readers
```

The ordinary checkDRC overload is direct-only. The two-vector overload runs
ImplantLayer first, irrespective of registration order, then validates its
finalized filler transaction with every other checker. Those checkers receive
only real Delete overlays, not staged preparation Add/Replace records. Editing
either validation vector is rejected. Results are published only after all
validators pass; the caller's preserved output prefix is not a new overlay.
Real filler Deletes invoke FR even for exact direct-legal coverage, so their
output is not lost. Non-exact coverage also invokes repair because released
sites still need fillers.

## Lifetime and configuration

Master geometry, VT/compatibility metadata, checker rules, Grid geometry and
filler settings are setup data. Rebuild checker/engine when those inputs
change. Ordinary placement commits do not require rebuilding them.

Every repair reads current nodes, masters, coordinates, orientation, DB IDs
and names through request-local placement caches. Concurrent read-only repairs
are supported; DB/Grid/Network mutation must not overlap them. Commit must
finish synchronizing all three before subsequent repair calls.
PlacementDRC's shared TBB arena is published once during setup and lives until
process shutdown, rather than being deleted by an arbitrary PlacementDRC owner.

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

Verification on 2026-10-20, complete default-target builds:

- Normal: 443/443 passed with AppleClang 17.
- ASan: 443/443 passed with Command Line Tools AppleClang 21.
- TSan: 8/8 lifecycle/concurrent-probe tests passed with AppleClang 21 and
  a locally TSan-instrumented oneTBB 2023.0.0; no race suppressions were used.
  The four-worker concurrent probe also passed 20 repeated runs.
  The uninstrumented Homebrew TBB reports scheduler races even for the atomic
  counter control test, so it is not used for the TSan verdict.
- Coverage includes checker old/new-region validation, DePlace resize/move,
  height changes, pending transaction normalization, identity/Pin references,
  padding and concurrent probes. Migration tests exercise 50/75/90% utilization.
- Both builds compiled every default target, including commit/command tests,
  destination wiring and the migration-copy gate. `git diff --check` passed.

These results use the test-only fake UDM provider; they do not establish
real UDM ABI compatibility or source-ODB writeback.

Review corrections on 2026-10-20:

- Generate the final filler transaction before other DRC validators; reject
  validator edits and publish nothing on failure, regardless of registry order.
- Keep the shared TBB arena alive across PlacementDRC owner destruction.
- Use candidate-local committed baselines/guards: a candidate checked alone,
  batched, or reordered must have the same result.
- Probe coherent VT tilings before a poor seed spends the shared search budget;
  cache those snapshots. A width-12 removal with unit fillers now reaches the
  independently verified all-LVT solution without exhausting 2048 candidates.
- Retain the same-footprint fast path and emit probe logs as complete stdio
  records rather than formatting a shared ostream from multiple callers.

Small Debug microbenchmark (21 nodes, five rows, FR_VERBOSE=0; 500 warmups and
500 timed calls per path, excluding setup): same-footprint isLegal decreased
from about 482 to 81 microseconds/call. This is not a large-design benchmark.

The test command also supports std removal/refill:
`test_filler_repair -inst U1`, equivalently `-inst U1 -master {}`.
An empty/whitespace master requests exactly one movable, ungrouped std cell;
nonempty master keeps placement/swap semantics. The command submits a null
target to ImplantLayerChecker and commits the old std Delete plus FR's refill
once. It tests Implant/FR, not every registered DRC rule. Regression coverage
includes repeated option use, invalid/fixed/grouped requests, failed calls
without mutation, and complete one/two-row refill after removal.

## Remaining destination work and limits

- Map `DePlace::commit` Add/place operations to real UDM APIs. The local
  `UnlChange_addCell/placeCell` signatures and error behavior are test
  contracts, not verified real UDM declarations. Confirm CellChangeRecord
  field/ID variants and preserve target connectivity on Replace. Local tests
  prove stable IDs/Node/Pin/Edge references; fake UDM has no logical net model,
  so real size-cell port mapping and connectivity need destination coverage.
- Run isLegal, findLeg, mixed commits and subsequent repairs on a real UDM
  design with the complete destination checker set. Local registration does
  not validate the missing checkers' interaction with filler changes. Those
  checkers must validate final fillers and real mixed Delete overlays read-only;
  removal-only preparation is routed exclusively to ImplantLayerChecker.
- Provide the commit/read barrier and keep DB, Grid and Network synchronized.
  The preflight rejects ordinary invalid batches before mutation; commit has
  no rollback journal for allocation failures or unexpected UDM exceptions.
- The local OpenROAD importer projects ODB into fake UDM. Its commits modify
  that projection, not the source ODB or a later write_def. Source-ODB writeback
  remains separate work.
- Placement over empty sites is outside the probe contract. Selected old std
  and real filler footprints must cover every target site. Padding-constrained
  tilings may fail safely instead of searching alternative padding-aware tilings.
  The new buffer is geometrically added;
  real opto net connections still need destination integration.
- Search is bounded: defaults include 16 tilings, 100000 geometric states,
  512 checker candidates per planner window and 2048 per repair. Any returned
  solution must pass the checker. If none is found within the caps, the result
  has no solution and no partial changes; truncation does not prove infeasibility.
- Large-design latency still needs profiling: relocation repeats name/placement
  reads and guard checks for each candidate, and the shared arena currently uses
  32 workers. Tune total opto/TBB concurrency against destination workloads.
