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
  then uses the shared `findLegAtOrigin` search. Buffer insertion replaces one or more intersecting
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
- DePlace assembles complete transactions: `isLegal` and `findLeg(LeafCellID, ..., rtOrient)`
  append a target `Replace`; `findLeg(PinID)` appends a named target `Add`.
  Pass that result to `commit` once without appending the target or Deletes
  again. For a std Add, first create its UDM instance using the returned name
  and master; `commit` resolves that real ID and imports it into Network/Grid.
- Probes do not mutate DB, Grid or Network. An existing output-vector prefix
  is preserved but is not a placement overlay for later probes. Independently
  accepted probes are not jointly DRC-validated.
- `DePlace::commit` handles Add/Replace/Delete. Standard-cell changes update only
  Network lookup maps and Grid occupancy/padding, preserving the std Node and
  DB ID on Replace and leaving std UDM untouched. Filler changes still update
  UDM, Network and Grid. The whole batch is preflighted before mutation;
  this checks final occupancy and feasibility, not DRC. A missing, ambiguous,
  wrong-master or already imported std Add instance rejects the batch.
  Replace preserves the std/filler classification; use Delete + Add to cross
  that boundary.
- Placement/search helpers use the destination's reference outputs
  `ccRecords` and `rtOrient`, with no pointer overloads or `replacedCell`
  argument. Only accepted candidates publish outputs. Replacement probes
  carry the existing Node ID and DB ID; insertion probes have no Node ID.
- Runtime placement reads use Network Node coordinates/master/orientation and
  Grid occupancy. Initial-location and overlap pixel/boundary visitors no longer
  read retained std UDM placement. New placement orientation comes from the
  destination site/site pattern before Grid painting. Overlap library shapes
  are translated by the Node's core-relative origin exactly once.
- Std commit applies the already preflighted proposal directly; the redundant
  second commit/UDM-validation path was removed. `addFillerNode` delegates to
  the common import path instead of duplicating it.

The engine API remains:

```cpp
explicit FillerRepairEngine(const ipl::ImplantLayerChecker& checker);
bool isReady() const;
RepairOutcome repair(const ipl::CheckRequest& request) const;
```

`isLegal(inst, master, changes)` now permits resizing at the original position.
Use `isLegal(inst, master, va, diameter, changes)` to allow position search;
use `findLeg(instId, va, diameter, master, changes, rtOrient)` to always run
virtual removal preparation and get the final location along with the
transaction. `diameter` is a nonnegative DBU extension, zero forbids movement
for an existing instance, and `va` uses absolute design coordinates. All
`findLeg` entry points return core-relative DBU (matching the change records) or
`(-1, -1)`. At most 256 positions are tried per relocation search rectangle.
The PinID overload deliberately uses an external UDM physical-pin anchor;
callers must keep it current. Missing terms fail without publishing output.
No verified pin-to-live-Node/current-master mapping API is present in this tree.

The destination-named `findLegAtOrigin(origin, Node*, va, diameter, changes,
rtOrient)` is the only shared search. It uses the supplied core-relative origin, searches a local Node
copy and returns core-relative DBU, with a single Grid-to-DBU conversion.
The Node-pointer resize overload is only a compatibility delegate to the
LeafCellID entry. Successful resize publishes final filler operations plus
one identity-preserving std Replace and the site orientation. Failure leaves
both the caller's change prefix and orientation unchanged. Preparation stays
request-local, calls ImplantLayerChecker only and may fail without blocking
the final target. The probe never registers or modifies library masters.
The public LeafCellID `findLeg` contains the resize transaction directly;
there is no additional resize helper to migrate. `isLegal` keeps its original
same-footprint fast path, then delegates to that public API on failure.
`checkPixels` keeps editable-filler and site-orientation selection inside its
body, with no extra helper declaration to port. Concrete `[FRPORT]` comments
mark these changes in the three destination functions.
The destination's `checkPixels` must match
this branch's temporary-Node ID/LeafCellID replacement recognition and internal
overlay assembly; it must not require old std Deletes in the caller's output prefix.
It still requires the existing Grid site-stack/padding APIs and symmetry
checks. Preserve any destination-specific pixel checks not present in this branch.

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
finish before subsequent repair calls. Network/Grid are authoritative for std
placement after a commit; the caller owns std UDM updates. Filler placement
remains synchronized across UDM, Network and Grid.
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
filler commit converts them to absolute DB coordinates. The caller uses the
same core offset when creating a std Add's UDM instance.

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
| `infrastructure/Grid.h/.cpp` | Full site-stack orientation checks and Node-based runtime overlap geometry |
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

Validated on 2026-11-25 with complete default-target builds:

- Normal (AppleClang 17): 511/511 tests passed.
- ASan (Command Line Tools AppleClang 21): 511/511 tests passed.
- Six focused regressions cover the destination resize entry, shared search
  and fast-path rejection without a valid radius/Implant checker. Node-pointer
  and LeafCellID entry results are compared on successful and rejected probes.
  Existing regressions cover stale/invalid std UDM geometry, translated overlap
  shapes, live placement seeds/site orientation and missing physical-pin terms.
- The command regression verifies that only ImplantLayerChecker sees virtual
  removal, while final validation still runs other checkers and rejects without mutation.
- Both builds include commit/command tests, destination wiring and migration
  parity/compile gates. The legacy ECO command is now compile-checked too.
- `git diff --check` passed; no checker goldens or repair algorithms changed.

Run the complete CMake build, which includes the commit/command executable
and compile-only destination wiring as well as the planner/checker tests.
These are the commands used for this validation; the explicit ASan compiler
and SDK paths apply to the current macOS host:

```bash
cmake -S src/dpl2/test -B src/dpl2/test/build/doc-sync-normal \
  -DCMAKE_BUILD_TYPE=Debug -DDPL2_TEST_USE_FAKE_UDM=ON -DDPL2_ENABLE_ASAN=OFF
cmake --build src/dpl2/test/build/doc-sync-normal --parallel 4
ctest --test-dir src/dpl2/test/build/doc-sync-normal --output-on-failure --parallel 4

cmake -S src/dpl2/test -B src/dpl2/test/build/doc-sync-asan-clang21 \
  -DCMAKE_BUILD_TYPE=Debug -DDPL2_TEST_USE_FAKE_UDM=ON -DDPL2_ENABLE_ASAN=ON \
  -DCMAKE_CXX_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0
cmake --build src/dpl2/test/build/doc-sync-asan-clang21 --parallel 4
ctest --test-dir src/dpl2/test/build/doc-sync-asan-clang21 --output-on-failure --parallel 4
```

The gate covers planner/internal cases, unchanged checker golden expectations,
one/two-row targets, 50/75/90% integration utilization, exact/non-exact
multi-filler coverage, order/avoid patterns, bounded failures, concurrency,
zero probe mutation, complete probe/commit transactions and engine reuse
after commit. FillerRepair and migration payload targets use
`-Wall -Wextra -Werror`; delivered infra/checker sources retain visible
warnings without treating all of them as errors. The pure planner also
compiles at C++17.

Removal coverage is split at the existing test boundary: internal tests call
the real retiler on the released standard-cell footprint (one/two rows,
multi-height preference, single-height fallback, no exact cover and budget
exhaustion). Integration tests call the actual null-target engine API and
check filler transactions, row orientation and unchanged Network/Grid state.
The fake-UDM runtime additionally removes a two-row standard cell with only
single-row-high filler masters. Both released rows must be filled using real
Grid site orientations; the database-free helper lacks that row/site context.
Both use five-row fixtures with at least six cells per row at 50/75/90%
utilization; no runtime dependencies were added to the internal test target.
The 15 internal removal variants are registered in `internalCases()` under
`FillerRepairInternalCases/FillerRepairInternalTest`, not a separate suite.

On macOS the harness disables ASan container-overflow annotations because
Homebrew GoogleTest is an uninstrumented static library; other configured
AddressSanitizer checks remain enabled.

The current macOS 27 host's Xcode clang 17 ASan runtime deadlocks during
initialization, before main, even for a minimal program. The installed Command
Line Tools clang 21 with its matching SDK runs that program successfully;
the ASan validation therefore uses that toolchain without changing xcode-select.

The runtime regression gate also covers std UDM immutability, repeated
resize/removal, caller-created std Add identity/preflight, stale or invalid
std UDM geometry, live overlap-shape translation, shifted-core/padding
coordinates, destination-site orientation and absent physical-pin anchors.
Checker/engine algorithms and destination checker golden expectations are
unchanged by this audit. The two fillerRepair implementation copies remain
identical and are both compiled.
Destination-entry tests additionally cover size up/down, 1h/2h transitions,
partial/disjoint moves, caller-prefix isolation, failure orientation preservation,
removal-preparation failure followed by final success, and concurrent calls
through both isLegal and the LeafCellID entry. A five-row, six-cells-per-row
114-DBU-site fixture checks shifted-core coordinates, INT_MAX radius clipping,
PinID result units and explicit/unaligned/negative search origins without mutation.

The local OpenROAD fixtures cover direct repair, 19 mixed-height checks with
implant rules, and a Nangate45 1h -> 2h -> 1h placement round trip. The latest
recorded fixture run is 2026-11-19; it is not a native UDM run. The local wire
updates only its UDM projection for fillers, not source ODB/write_def.
Historical performance and TSan measurements are retained in Git history,
not treated as validation of this revision.

`test_filler_repair -inst U1 ?-master {}?` checks std removal/refill.
With `-master M2H ?-diameter DBU?`, resize uses `probeResize`: virtual removal
calls ImplantLayerChecker directly; final placement uses `legalCellInRect`
and the full registered checker set, not the public `findLeg`. Diameter defaults to zero
(original position only); an explicit diameter also enables same-size moves.
The command creates caller-owned std Add instances before commit and removes
them if the batch rejects. Std Replace/Delete leaves UDM untouched.

## Remaining destination work and limits

- Map filler `DePlace::commit` Add/place operations and caller-owned std Add
  creation to real UDM APIs. The local
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
- Provide the commit/read barrier and coordinate caller-owned std UDM edits
  with the authoritative Network/Grid placement and filler UDM changes.
  The preflight rejects ordinary invalid batches before mutation; commit has
  no rollback journal for allocation failures or unexpected UDM exceptions.
- Name generation scans live Network nodes. If the caller retains a removed
  std instance only in UDM, a later generated-name collision can be rejected
  by commit; callers must manage the corresponding UDM lifecycle.
- The local OpenROAD importer projects ODB into fake UDM. Filler commits and
  caller-owned std Add creation modify that projection, not the source ODB
  or a later write_def. Std Replace/Delete commits change only Network/Grid.
  Source-ODB writeback remains separate work.
- The local site-stack adapter handles ordered TechSite/orientation pairs.
  The actual proprietary UDM pattern element type was not available;
  map any different representation in `normalizeSitePattern` in Grid.cpp.
  Unknown explicit pattern representations fail closed. This OpenDB version
  projects individual master sites but does not retain explicit LEF patterns.
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
