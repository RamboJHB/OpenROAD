# Multi-height Filler Insertion HandOff

## Status

- Branch: `codex/dpl-multi-height-filler-insertion-dpl`
- Baseline: `ce29b77554fb0adb84ca8e745667443eff52e7d8`
- Implementation root: `src/dpl`
- State: complete and ready for review

## Objective

Add deterministic multi-height filler insertion to classic OpenDB-backed DPL.
The insertion request consumes DPL-owned filler options, snapshots the legal
empty placement grid, plans non-overlapping one-row and multi-row fillers, and
commits the complete plan to OpenDB only after planning succeeds.

## Scope boundary

- All production code, tests, documentation, and build integration for this
  project live under `src/dpl`.
- `src/dpl2` is not modified and classic DPL does not depend on dpl2 or UDM.
- Existing `filler_placement [-prefix prefix] filler_masters` scripts remain
  supported.
- A DPL-local `set_filler_option` command supplies persistent settings for a
  subsequent no-argument `filler_placement` call.
- Placement DRC and dpl2 filler-repair transactions are outside this project's
  dependency boundary.

## Configuration contract

The configured insertion path reads one immutable DPL filler-options snapshot:

- configured filler masters are the only insertion candidates;
- `follow_order=true` preserves the configured candidate order;
- `follow_order=false` applies deterministic area/height/width ordering;
- `fit_space=true` requires exact coverage and commits nothing on failure;
- `fit_space=false` permits a deterministic partial packing;
- `prefix` controls generated instance names.

Calling `set_filler_option` replaces the previous snapshot. The legacy
`filler_placement ... filler_masters` form builds an equivalent request with
the historical width-first ordering and does not require prior configuration.

## Planned data flow

```text
set_filler_option
        |
        v
DPL-owned immutable filler options
        |
        v
filler_placement
        |
        +-- import placed OpenDB cells
        +-- snapshot valid, vacant DPL grid pixels
        +-- validate integral master footprints
        +-- deterministic bounded rectangular tiling
        +-- preflight generated names and coordinates
        v
atomic OpenDB create/place transaction
```

## Geometry and safety contract

- Filler width is a positive integral number of sites.
- Filler height is a positive integral number of rows.
- Filler masters use `CLASS CORE` or `CLASS CORE SPACER`; accepting both
  preserves classic DPL compatibility.
- Every covered pixel exists, is a valid row site, and is vacant in the
  request snapshot.
- A multi-height filler is anchored on the lowest covered row and uses that
  row's legal orientation.
- Planned filler footprints never overlap each other or an existing instance.
- Exact-mode planning, validation, budget, or name-collision failures create no
  instances.
- A commit-time create failure rolls back every instance created by the same
  request.

## Acceptance requirements

1. The corrected branch contains no changes under `src/dpl2` relative to the
   baseline.
2. A real Tcl/OpenDB regression configures a two-row filler through
   `set_filler_option`, runs no-argument `filler_placement`, and verifies the
   resulting masters, locations, orientations, and full grid coverage.
3. Unit tests cover candidate order, mixed heights, occupied/invalid sites,
   exact failure, partial packing, invalid input, budget failure, and repeated
   deterministic output.
4. Existing filler regressions remain compatible, including the historical
   unfillable-gap diagnostic.
5. Exact failures and invalid masters leave the database unchanged.
6. Generated instances are physical-only, placed, `DIST` source fillers with
   deterministic unique names using the configured prefix.
7. DPL unit tests and focused Tcl regressions pass; formatting and
   `git diff --check` pass.
8. This handoff is updated with final files, commands, test counts,
   limitations, commits, and the published branch.

## Validation record

All validation below was run from an independent Linux Codespace worktree at
the stated baseline. The Codespace required validation-only build workarounds
for unavailable OR-Tools/Boost components and unrelated baseline OpenSTA/fmt
compile issues; none of those workarounds are part of this branch.

| Check | Result |
| --- | --- |
| Build `dpl`, `dpl_test`, and the validation OpenROAD executable | Pass |
| Complete DPL C++ suite | 14/14 pass |
| Existing `fillers1` through `fillers8` Tcl regressions | 8/8 pass |
| New multi-height success and exact-failure Tcl regressions | 2/2 pass |
| `git diff --check` | Pass |
| Changes under `src/dpl2` relative to the baseline | None |

Commands used for the final test pass:

```sh
cd src/dpl/test
../../../build-dpl/src/dpl/test/dpl_test --gtest_color=no
./regression fillers1 fillers2 fillers3 fillers4 fillers5 fillers6 \
  fillers7 fillers8 fillers_multi_height \
  fillers_multi_height_exact_failure
```

The validation build disabled Python bindings, so the legacy regression count
above is explicitly the Tcl matrix. The planner and OpenDB transaction are
covered directly by the C++ tests.

## Final implementation map

- `src/FillerPlacementInternal.{h,cpp}`: deterministic rectangular planner,
  bounded exact backtracking, partial packing, origin masks, and forbidden
  width-abutment support.
- `src/FillerPlacement.cpp`: option consumption, OpenDB/grid validation,
  atomic preflight/commit/rollback, metadata, and legacy command compatibility.
- `include/dpl/Opendp.h`, `src/Opendp.{i,tcl}`: persistent DPL filler options
  and Tcl/SWIG command wiring.
- `test/FillerPlacementInternalTest.cpp`: 11 planner unit tests.
- `test/FillerPlacementOpenDbTest.cpp`: two real OpenDB transaction tests.
- `test/fillers_multi_height*` and `test/multi_height_fillers.lef`: success and
  fail-closed Tcl fixtures/goldens.
- `README.md` and DPL CMake/test registration: user contract and build wiring.

## Known boundaries

- Exact fallback search is intentionally bounded to 250,000 states and depth
  4,096; exceeding either budget fails closed before any OpenDB mutation.
- dpl2-specific implant-layer DRC, `check_drc`, and repair transactions are not
  dependencies of classic DPL and were not ported.
- Geometry follows classic DPL's uniform site-width and row-height grid model.

The implementation commit is the commit containing this handoff. The branch is
published as `codex/dpl-multi-height-filler-insertion-dpl` in the project
repository.
