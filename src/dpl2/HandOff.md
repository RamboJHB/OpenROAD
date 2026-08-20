# fillerRepair handoff

## Current state

This handoff describes the second destination-port version of fillerRepair.
Its checker-side marker is the two repair failure logs added in
`ImplantLayerChecker::repairOverlay()`.

The module now has one target contract: a temporary standard-cell `Node` plus
one `CellChangeRecord` Delete overlay naming the committed node it replaces.
The result is an atomic list of surrounding filler `Replace` records.

There is no filler Add path, filler Delete output, footprint retiler, target
movement, layout rewrite, engine `update`, external engine setter, or numeric
request API.

The destination-facing call chain is:

```text
DePlace
  -> PlacementDRC::checkDRC(temp, x, y, orient,
                            fillerChanges, overlayChanges)
  -> every registered DRC checker
  -> ImplantLayerChecker::check(...)
  -> lazy FillerRepairEngine::repair(CheckRequestOverlay)
  -> ImplantLayerChecker::checkPlaceWithOverlays(...)
```

`isLegal` overlays one old standard cell. `findLegal` overlays one exact-cover
filler. Both remain pre-commit and non-mutating.

## Files to migrate

Copy `src/dpl2/src/fillerRepair2/` as `src/dpl2/src/fillerRepair/`. Its runtime
sources are synchronized with the exercised implementation, its CMake is the
small integration form, and `test/FillerRepairPortableTest.cpp` follows the
checker GoogleTest/helper pattern.

The concrete destination touch points are:

- `src/dpl2/src/drc/ImplantLayerChecker.h/.cpp`: dual-record entry,
  `CheckRequestOverlay`, lazy engine dispatch, and overlay batch oracle;
- `src/dpl2/src/drc/DRCChecker.h`: the two-vector virtual check interface;
- `src/dpl2/src/PlacementDRC.h/.cpp`: direct versus overlay/repair-capable
  request dispatch and atomic trial publication across every checker;
- `src/dpl2/src/DePlace.cpp` and `include/dpl2/DePlace.h`: the isLegal
  temporary-node request;
- `src/dpl2/src/dbToOpendp.cpp`: installs one Implant checker from
  `initPlacementDRC()` after importing the full master catalog and final
  fillerSetting;
- `src/dpl2/src/Place.cpp`: exact one-filler candidate rule for findLegal;
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

The destination supplies its existing UDM, infrastructure, and checker include
and link closure through `dpl2_filler_repair_deps` if needed. No fake target is
linked into the destination library.

## Preconditions

Before the first parallel repair call:

1. DePlace has finished Grid, Network, Design, and fillerSetting setup before
   `initPlacementDRC()`.
2. `initPlacementDRC()` has registered the complete checker set exactly once.
3. Usable configured filler masters have a Network Master and `isFiller=true`;
   unusable entries are logged and skipped.
4. Every standard-cell master opto may propose was imported by createNetwork.
5. Grid and Network describe the same committed placement revision.
6. No database mutation runs concurrently with checker/engine calls.

The checker and lazy engine are immutable for that revision. Publish a new
checker revision after a committed placement mutation before beginning a new
repair phase.

The engine does not require Design or PhysDesMgr. It builds planner metadata
from checker `MasterItem` records and Network nodes. Only missing Grid/Network
or invalid row/site geometry blocks initialization; other bad records and
requests fail with empty changes and structured `[fr]` logs.

## Verification

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
- the `fillerRepair2` helper-based checker/engine GoogleTest;
- destination DePlace/Place/PlacementDRC sources;
- checker replacement and atomic failed-dispatch lifecycle tests;
- unchanged checker golden expectations;
- portable checker/planner E2E;
- fake-UDM runtime E2E, including one- and two-row targets and concurrent
  checker calls.

## Remaining destination checks

- Confirm the destination's real `CellChangeRecord` field names and ID variant
  exactly match this branch.
- Confirm destination `createNetwork()` imports every library master, including
  masters without a placed instance.
- Confirm fillerSetting is complete before `initPlacementDRC()`.
- Confirm opto treats the single Delete record as target overlay input and
  commits only the returned filler Replace records in the same transaction.
- Establish the revision barrier used to replace the checker after commit.
- Run a real-UDM design with both isLegal and findLegal; fake UDM is only a
  deterministic data provider, not a substitute for that final ABI/link test.
