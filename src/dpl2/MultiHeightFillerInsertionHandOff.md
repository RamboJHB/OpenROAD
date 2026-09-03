# Multi-height Filler Insertion HandOff

## Status

- Branch: `codex/dpl-multi-height-filler-insertion`
- Baseline: `ce29b77554fb0adb84ca8e745667443eff52e7d8`
- State: implementation and acceptance complete; ready for review
- Implementation commit: `a2e863c368fef410e220f96471d6647e8b43152f`
- Source of truth for final validation: existing OpenROAD Codespace

## Objective

Add request-scoped, deterministic multi-height filler insertion to the dpl2
placement infrastructure.  The inserter consumes the existing
`set_filler_option`/`fillerSetting` configuration, finds empty legal Grid sites,
tiles them with configured filler masters, and returns `CellChangeRecord::Add`
records.  It does not create UDM cells or mutate Grid/Network itself; the caller
owns atomic commit, consistent with the existing filler-repair transaction
boundary.

## Non-goals and compatibility

- Do not change `CellChangeRecord` or `PlacementDRC` wire formats. An additive
  `ImplantLayerChecker::checkFillerInsertion()` batch entry point is allowed.
- Do not make classic OpenDB `src/dpl` depend on UDM-backed dpl2 types.
- Do not emit Delete or Replace records from initial insertion.
- Do not move standard cells, existing fillers, macros, or blockages.
- Do not mutate `fillerSetting` while planning.
- Preserve configured master order when `FollowOrder` is true.
- Keep filler-repair behavior and its `fillerRepair`/`fillerRepair2` mirror
  unchanged.

## Configuration authority

`fillerSetting` is the only insertion policy and catalog authority:

- `getFillerPhysCells()` supplies the configured masters.
- `getFollowOrder()` selects configured-order versus deterministic geometric
  ordering.
- `getFitSpace()` requires exact coverage of a solved component.
- `getCheckDRC()` enables final checker validation when a checker is available.
- `getPrefix()` prefixes generated instance names.
- `getAvoidPattern()` supplies symmetric forbidden width-abutment pairs.

The insertion engine reads these values for each immutable planning call.
`fillerSetting` owns a monotonic revision. `DePlace::planFillerInsertion()`
publishes a changed revision to `Network` and rebuilds the checker before
planning, so a late `set_filler_option` update cannot silently use a stale
catalog. Direct engine users must provide an already-published Network/checker
snapshot; stale input fails closed.

## Planned data flow

```text
set_filler_option
        |
        v
final fillerSetting + complete Network master catalog
        |
        v
DePlace::planFillerInsertion()
        |
        +-- publish changed fillerSetting revision
        v
FillerInsertionEngine::plan()
        |
        +-- snapshot legal empty sites from Grid
        +-- build configured master footprints
        +-- deterministic bounded exact-cover search
        +-- validate names/coordinates/orientations/avoid patterns
        +-- optional PlacementDRC overlay validation
        v
vector<CellChangeRecord> containing Add only
        |
        v
caller-owned atomic commit
```

## Geometry contract

- Coordinates in returned records are core-relative, matching the current
  `CellChangeRecord`/Node protocol.
- A filler width must be positive and an exact multiple of Grid site width.
- A filler height must cover one or more complete Grid row intervals.
- Every covered pixel must exist, be valid, be unreserved, and have no committed
  occupant.
- A placement uses the legal orientation of its anchor row/site.
- All pixels of an Add footprint are reserved in the request-local occupancy
  before another tile is considered.
- Output footprints never overlap and no generated Add covers an existing node.

## Search contract

- All legal empty sites in the Grid form one request-scoped transaction.
- Search anchors on the lexicographically first uncovered site.
- Candidate ordering is deterministic.  With `FollowOrder=true`, configured
  order is primary; otherwise use area, height, width, and master ID.
- The search is bounded by explicit state and solution budgets.
- `FitSpace=true` accepts only exact coverage of the transaction. Untileable or
  budget-exhausted input emits no Add records. `FitSpace=false` may return the
  deterministic maximal partial packing found after exhaustive bounded search.
- Avoid-pattern checks apply at horizontal filler boundaries.

## Implemented design

1. A C++17 database-free rectangular exact-cover planner owns request-local
   occupancy and deterministic bounded search.
2. A dpl2 runtime adapter converts Grid/Network/configured physical masters into
   planner input and materializes Add-only records.
3. The implant checker validates the complete synthetic Add batch against the
   committed snapshot without mutating placement state.
4. `DePlace` provides lazy revision publication and the public planning entry
   point; the local import path also finalizes configuration explicitly.

## Test matrix

### Pure planner: 10 tests

- multi-height master used as one rectangle;
- authoritative candidate order;
- occupied/invalid holes remain untouched;
- exact-fill rejection and maximal partial packing;
- forbidden abutments and per-master origin masks;
- zero budget and invalid input fail closed;
- repeated calls are byte-order deterministic.

### Runtime/checker/DePlace: 11 tests

- `set_filler_option` selects a double-height master and produces two Add
  records for a four-row fixture;
- configured order and deterministic geometric order are distinct;
- avoid patterns cover new/new and committed/new boundaries;
- prefix and `FitSpace` control the returned transaction;
- stale classification and missing requested checker fail closed;
- malformed Add records (orientation or original master) are rejected;
- concurrent calls are deterministic and read-only;
- revisions change only for real, fully parsed settings changes;
- `DePlace` publishes a late setting revision before planning.

## Acceptance requirements

1. `fillerSetting` is the only source of candidate masters and naming policy.
2. At least one real multi-height case places a two-row filler as one Add record.
3. Every successful result is deterministic, Add-only, aligned, non-overlapping,
   and covers only legal empty pixels.
4. Exact-fill requests that are untileable, invalid, stale, budget-exhausted,
   or checker-rejected return no changes. Partial results require
   `FitSpace=false` explicitly.
5. Planning does not mutate UDM, Grid, Network, Node, or `fillerSetting`.
6. `CellChangeRecord` and `PlacementDRC` wires remain unchanged; the checker API
   change is additive and Add-only.
7. New tests and all existing filler-repair regressions pass normally, and the
   21 new tests pass under targeted ASan.
8. C++ warning gates, formatting, and `git diff --check` pass.
9. This document records final files, commands, counts, limitations, commit, and
   remote branch before handoff.

## Initialization publication

The late-configuration risk is resolved by `fillerSetting::getRevision()` and
`DePlace::finalizeFillerConfiguration()`. Every real setting change advances the
revision; planning refreshes Network filler classification and the checker when
the published revision differs. Invalid multi-token avoid settings are parsed
transactionally and leave both configuration and revision unchanged.

## Validation evidence

- Codespace full fake-UDM suite: `371/371` passed.
- Codespace targeted insertion suite: `21/21` passed.
- Codespace targeted ASan insertion suite: `21/21` passed.
- Destination/migration configuration: `343/343` passed with parallelism 2.
- New planner/runtime targets compile with the strict warning profile.
- `git diff --check` passes after line-ending normalization in Codespace.
- A top-level OpenROAD configure was attempted, but this Codespace does not
  contain `ortoolsConfig.cmake`; configuration stops in `src/mpl2` before dpl2
  compilation. This is an environment dependency, not a dpl2 test failure.

Commands used:

```bash
ALL=1 bash src/dpl2/test/local/run_fake_udm_e2e.sh
cmake --build src/dpl2/test/build/insertion-asan \
  --target dpl2_filler_insertion_runtime_test --parallel 2
ctest --test-dir src/dpl2/test/build/insertion-asan \
  --output-on-failure -R filler-insertion -j 2
cmake --build src/dpl2/test/build/insertion-migration --parallel 2
ctest --test-dir src/dpl2/test/build/insertion-migration \
  --output-on-failure -j 2
```

## Changed files

- `src/dpl2/src/fillerInsertion/FillerInsertionPlanner.{h,cpp}`
- `src/dpl2/src/fillerInsertion/FillerInsertionEngine.{h,cpp}`
- `src/dpl2/src/fillerInsertion/test/FillerInsertionPlannerTest.cpp`
- `src/dpl2/test/local/filler_insertion_runtime_test.cpp`
- `src/dpl2/include/dpl2/DePlace.h`, `src/dpl2/src/DePlace.cpp`
- `src/dpl2/src/infrastructure/fillerSetting.{h,cpp}`
- `src/dpl2/src/drc/ImplantLayerChecker.{h,cpp}`
- destination/local initialization and CMake/test-runner integration files

## Explicit limitations

- Planning returns a transaction; actual UDM Add commit and rollback remain the
  caller's responsibility.
- Exact fill is atomic over the whole Grid snapshot; the implementation does not
  independently commit disconnected components.
- Search is deliberately bounded (default 250,000 states and 64 complete
  solutions). Exhaustion fails closed.
- Final insertion DRC currently uses the available implant-layer checker. Other
  checker types can be added behind the unchanged `PlacementDRC` ownership.
- Generated names are unique within the proposed batch. The eventual commit
  owner must also enforce uniqueness against the live UDM namespace.
