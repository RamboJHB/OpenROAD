# fillerRepair handoff

## Current state

This handoff describes the second destination-port version of fillerRepair.
Its checker-side marker is the two repair failure logs added in
`ImplantLayerChecker::repairOverlay()`.

The module accepts a temporary standard-cell `Node` plus either one committed
standard-cell Delete overlay, or one/multiple filler Delete overlays that
intersect a new buffer footprint. It first classifies the filler geometry as
exact cover. Exact cover keeps the existing swap-only path. For non-exact
cover, the engine exactly tiles the released set
`(deleted filler sites - target sites)` with configured filler masters, then
runs the existing checker-guided planner so the new fillers and surrounding
fillers may receive master swaps. An unfillable or DRC-illegal released set
returns no solution and no partial changes.

The result is an atomic list of request-local filler `Add` and surrounding
filler `Replace` records. It never contains `Delete`; those records remain
caller-owned request overlays. `CheckRequest::x/y/orientation` are
authoritative, and the public request/result wire is unchanged. There is no
target movement, engine `update`, external engine setter, or numeric request
API.

Every successful repair emits a `REPAIR SUCCESS` transcript. It first lists
each caller-owned `Delete` overlay, then each returned `Add` or `Swap
(Replace)` change. The records include the old/new network and DB cell IDs,
cell names, master IDs and names, site widths, orientations, and core-relative
origins. A newly added cell reports its configured generated name and marks
IDs that are assigned only when the caller commits the transaction.

The destination-facing call chain is:

```text
DePlace
  -> PlacementDRC::checkDRC(temp, x, y, orient,
                            fillerChanges, overlayChanges)
  -> every registered DRC checker
  -> ImplantLayerChecker::check(...)
  -> lazy FillerRepairEngine::repair(CheckRequest)
  -> ImplantLayerChecker::checkPlaceWithOverlays(...)
```

`isLegal` overlays one old standard cell. DePlace `findLegal` collects every
filler intersecting the proposed target and passes those caller-owned Delete
records directly through `PlacementDRC`; empty target sites are allowed. The
`test_filler_repair` command similarly resolves every `-inst` selection into a
Delete overlay and calls the checker without first mutating Grid or Network.
Both flows remain pre-commit and non-mutating.

## Files to migrate

Copy `src/dpl2/src/fillerRepair2/` as `src/dpl2/src/fillerRepair/`. Its runtime
sources are synchronized with the exercised implementation, its CMake is the
small integration form, and its tests are split by responsibility:

- `test/FillerRepairIntegrationTest.cpp` exercises the public checker/engine
  boundary with `ImplantLayerCheckerHelper`. Its five-row fixtures assert at
  least six cells per row and cover 50%, 75%, and 90% occupied-site
  utilization;
- `test/FillerRepairInternalTest.cpp` exercises planner behavior through
  in-memory `PlacementView` and `RepairOracle` doubles and also covers
  deterministic exact tiling, no-overlap coverage, and unfillable released
  sets.

The copy-only payload intentionally exposes just this runtime API:

```cpp
explicit FillerRepairEngine(const ImplantLayerChecker& checker);
bool isReady() const;
RepairOutcome repair(const CheckRequest& request) const;
```

The August 21 cleanup removed the remaining one-shot lifecycle wrappers and
unused migration helpers. In particular, the payload has no separate
`init`/`bindInfrastructure`/`update`/`precheck`, context setter, adapter, or
request API predating `CheckRequest`. `PlacementView`, `RepairOracle`,
`RepairPlanner`, and their request IDs are implementation-only planner
boundaries, not opto integration APIs.

The concrete destination touch points are:

- `src/dpl2/src/drc/ImplantLayerChecker.h/.cpp`: dual-record entry,
  direct `CheckRequest` flow with no resolved-request layer, lazy engine
  dispatch, and overlay batch oracle. `MasterItem` does not carry a site name;
- `src/dpl2/src/drc/DRCChecker.h`: the two-vector virtual check interface;
- `src/dpl2/src/PlacementDRC.h/.cpp`: direct versus overlay/repair-capable
  request dispatch and atomic trial publication across every checker;
- `src/dpl2/src/DePlace.cpp` and `include/dpl2/DePlace.h`: the isLegal
  temporary-node request;
- `src/dpl2/src/dbToOpendp.cpp`: installs one Implant checker from
  `initPlacementDRC()` after importing the full master catalog and final
  fillerSetting;
- `src/dpl2/src/Place.cpp`: collects all target-intersecting fillers for
  findLegal and passes their Delete overlays through the existing API;
- `src/dpl2/dpl2ui/testFillerRepairCmd.cc`: direct checker probe for selected
  filler Deletes, with Add/Replace validation and reporting;
- `src/dpl2/src/infrastructure/Objects.h`: shared `CellChangeRecord` wire, if
  the destination does not already have the same definition.

The clean destination-source baseline used for diffing was pushed separately
as commit `270e634a2f`. Compare the final implementation against that commit to
isolate all migration edits.

Do not copy `src/dpl2/local/`, `src/dpl2/test/local/`, build directories, or
fake UDM headers. They are repository-only validation wiring.

## CMake

The minimal destination connection is:

```cmake
add_subdirectory(src/dpl2/src/fillerRepair)
target_link_libraries(dpl2Lib PRIVATE dpl2::fillerRepair)
```

The payload target compiles `RepairPlanner.cpp`, `FillerRetiler.cpp`, and
`FillerRepairEngine.cpp`; its test target explicitly names the integration and
internal test sources instead of globbing destination files.

The destination supplies its existing UDM, infrastructure, and checker include
and link closure through `dpl2_filler_repair_deps` if needed. No fake target is
linked into the destination library.

## Preconditions

Before the first parallel repair call:

1. DePlace has finished Grid, Network, Design, and fillerSetting setup before
   `initPlacementDRC()`.
2. `initPlacementDRC()` calls `Network::updateFillerClassification()` once,
   then registers the complete checker set exactly once. This setup barrier
   makes every Master and Node follow the final fillerSetting before checker
   metadata is frozen.
3. Usable configured filler masters have a Network Master and `isFiller=true`;
   unusable entries are logged and skipped.
4. Filler masters used for Add have a legal site/orientation at the released
   site. Database-free fixtures derive this from the deleted filler occupying
   that site; real designs use the master's TechSite.
5. Every standard-cell master opto may propose was imported by createNetwork.
6. Grid and Network describe the same committed placement revision.
7. No database mutation runs concurrently with checker/engine calls.

Master geometry, VT metadata, master compatibility, rules, and grid geometry
are immutable setup data. Rules remain checker-owned; the lazy engine caches
master/geometry metadata and compatibility tables. Rebuild checker/engine if
those setup inputs change, not after ordinary placement commits.

Each repair reads placed instances, current masters, coordinates, orientation,
and DB ids from the current Grid/Network. `PlacementView` remains the planner's
query interface, backed by request-local lazy node/row caches for stable
references; no placed-instance snapshot survives a repair call. Caller commits
must synchronize DB, Grid occupancy, and Network (including Add/Delete lookup
maps) before subsequent calls. Concurrent read-only repairs remain supported,
but commit and repair must not overlap.

The engine does not require Design or PhysDesMgr. It builds shared metadata
from checker `MasterItem` records and queries current Network nodes as needed.
Only missing Grid/Network or invalid row/site geometry blocks initialization;
other bad records and requests fail with empty changes and structured
`[fr]` logs.

## Verification

The 2026-09-09 maintenance pass keeps the public engine/checker wire unchanged:

- One private request-bound oracle replaces the BoundOracle/LayoutOracle chain.
  It merges fixed Adds with candidates and converts participants once. Caller
  Deletes stay in the original request; same-name Add changes replace in place.
- The default 2048 checker-candidate limit now covers the whole repair, including
  layout snapshots, all tilings/seeds, and planner baselines. Cache hits do not
  consume it. Geometry-search caps remain separate; incomplete search and checker
  failures are logged distinctly from an exhausted candidate domain.
- Adaptive enumeration skips only candidates actually answered in the same
  oracle context. Spatial instance order is not used as an ID-sorted set, and a
  truncated level cannot hide an unasked old combination in a later level.
- Guards include complete editable footprints and the checker's maximum rule
  reach. Multi-row ranking/frontiers use the same footprint convention.
- Invalid search bounds are rejected, and canonical overlay keys retain their
  entries when duplicate removal crosses the small-buffer boundary.
- Static master-size groups replace repeated full master-list scans and copied
  master-ID maps during retiling. Current placement remains request-local.
- The CMake gate compares the shared implementation files with fillerRepair2,
  refreshes its staged sources on edits, and explicitly enables test transcripts
  even when the invoking shell sets FR_VERBOSE=0.

Validation on 2026-09-09: normal and ASan builds both passed all 381 CTest
cases with external FR_VERBOSE=0; normal took 41.15 seconds and ASan took
121.63 seconds. These are harness results, not a real-design performance claim.

Run from `src/dpl2/test`:

```bash
cmake -S . -B build-cmake
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure

cmake -S . -B build-cmake-asan -DDPL2_ENABLE_ASAN=ON
cmake --build build-cmake-asan -j
ctest --test-dir build-cmake-asan --output-on-failure
```

The gate compiles:

- the pure planner at C++17;
- the runtime payload at C++20 with `-Wall -Wextra -Werror`;
- `fillerRepair2` as a copy-only migration compile check;
- the `fillerRepair2` checker/engine integration GoogleTest;
- the `fillerRepair2` internal-only planner GoogleTest;
- destination DePlace/Place/PlacementDRC sources;
- checker replacement and atomic failed-dispatch lifecycle tests;
- reused-engine placement lifecycle tests (committed Swap/Add/Delete, sparse
  DB/Network ids, moved/oriented instances, and fillers appearing after setup);
- unchanged checker golden expectations;
- checker/planner E2E;
- fake-UDM runtime E2E, including one- and two-row targets and concurrent
  checker calls;
- single- and multi-filler non-exact cover, exact released-site Add coverage,
  deterministic Add output, and unfillable no-solution behavior;
- Add-plus-Swap repairs and same-name Add master changes, each with an illegal
  seed precondition and a final real-checker verification;
- the whole-repair 2048 checker-candidate limit, with no partial result or
  placement mutation on exhaustion;
- unordered/synthetic IDs, truncated-level candidate coverage, large rule reach,
  multi-row external neighbors, invalid configuration, and key shrink boundaries.

## Remaining destination checks

- Confirm the destination's real `CellChangeRecord` field names and ID variant
  exactly match this branch.
- Confirm destination `createNetwork()` imports every library master, including
  masters without a placed instance.
- Confirm fillerSetting is complete before `initPlacementDRC()`.
- Confirm opto treats all Delete records as caller-owned target overlay input
  and commits those deletions plus returned filler Add/Replace records in one
  transaction. The generic `DePlace::commit` path still ignores `OpType::Add`,
  so it is not yet the transaction owner for this buffer-insertion flow.
- Establish a commit/read barrier that keeps DB/Grid/Network synchronized;
  ordinary commits do not require checker replacement, but static setup changes do.
- Run a real-UDM design with both isLegal and findLegal; fake UDM is only a
  deterministic data provider, not a substitute for that final ABI/link test.
