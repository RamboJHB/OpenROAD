# fillerRepair handoff

## Current state

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
  -> ImplantLayerChecker::check(...)
  -> lazy FillerRepairEngine::repair(CheckRequestOverlay)
  -> ImplantLayerChecker::checkPlaceWithOverlays(...)
```

`isLegal` overlays one old standard cell. `findLegal` overlays one exact-cover
filler. Both remain pre-commit and non-mutating.

## Files to migrate

Copy `src/dpl2/src/fillerRepair2/` to the destination as
`src/dpl2/src/fillerRepair/`. Its sources are synchronized with the exercised
`fillerRepair/` implementation; its CMake is intentionally the smaller
destination form.

The concrete destination touch points are:

- `src/dpl2/src/drc/ImplantLayerChecker.h/.cpp`: dual-record entry,
  `CheckRequestOverlay`, lazy engine dispatch, and overlay batch oracle;
- `src/dpl2/src/drc/DRCChecker.h`: the two-vector virtual check interface;
- `src/dpl2/src/PlacementDRC.h/.cpp`: atomic trial/publish dispatch;
- `src/dpl2/src/DePlace.cpp` and `include/dpl2/DePlace.h`: setup-time master
  registration and the isLegal temporary-node request;
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

Before the first parallel checker call:

1. DePlace has finished Grid, Network, Design, and fillerSetting setup.
2. Every configured filler master has a Network Master and `isFiller=true`.
3. Every standard-cell master opto may propose is registered.
4. Grid and Network describe the same committed placement revision.
5. No database mutation runs concurrently with checker/engine calls.

The checker and lazy engine are immutable for that revision. Publish a new
checker revision after a committed placement mutation before beginning a new
repair phase.

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
- destination DePlace/Place/PlacementDRC sources;
- unchanged checker golden expectations;
- portable checker/planner E2E;
- fake-UDM runtime E2E, including one- and two-row targets and concurrent
  checker calls.

## Remaining destination checks

- Confirm the destination's real `CellChangeRecord` field names and ID variant
  exactly match this branch.
- Confirm its target masters are all registered before first use.
- Confirm opto treats the single Delete record as target overlay input and
  commits only the returned filler Replace records in the same transaction.
- Establish the revision barrier used to replace the checker after commit.
- Run a real-UDM design with both isLegal and findLegal; fake UDM is only a
  deterministic data provider, not a substitute for that final ABI/link test.
