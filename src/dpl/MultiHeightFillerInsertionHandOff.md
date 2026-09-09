# Multi-height Filler Insertion HandOff

## Status

- Branch: `codex/dpl-multi-height-filler-insertion-dpl`
- Baseline: `ce29b77554fb0adb84ca8e745667443eff52e7d8`
- Implementation root: `src/dpl`
- State: complete and validated

## Objective

Add deterministic multi-height filler insertion to classic OpenDB-backed DPL.
The no-argument insertion path consumes the existing dpl2 `fillerSetting`
directly, snapshots the legal empty placement grid, plans non-overlapping
one-row and multi-row fillers, and commits the complete plan to OpenDB only
after planning succeeds.

## Scope boundary

- All production code, tests, documentation, and build integration for this
  project live under `src/dpl`.
- `src/dpl2` is not modified. DPL reads dpl2's existing `fillerSetting`; it
  does not implement another setting object or `set_filler_option` command.
- Existing `filler_placement [-prefix prefix] filler_masters` scripts remain
  supported.
- A no-argument `filler_placement` reads the setting owned by the dpl2
  `DePlace` singleton.
- Placement DRC and dpl2 filler-repair transactions are outside this project's
  dependency boundary.

## Configuration contract

The configured insertion path reads one immutable snapshot from dpl2
`fillerSetting`:

- configured filler masters are the only insertion candidates;
- `follow_order=true` preserves the configured candidate order;
- `follow_order=false` applies deterministic area/height/width ordering;
- `fit_space=true` requires exact coverage and commits nothing on failure;
- `fit_space=false` permits a deterministic partial packing;
- `prefix` controls generated instance names.

The existing dpl2 `set_filler_option` remains the sole configuration writer.
The legacy `filler_placement ... filler_masters` form builds a request directly
from its arguments with historical geometric ordering and does not require
dpl2 configuration.

## Data flow

```text
dpl2 set_filler_option
        |
        v
existing dpl2 fillerSetting
        |
        v
DPL filler_placement
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
2. A real OpenDB test configures dpl2 `fillerSetting`, runs the no-argument
   insertion API, and verifies that the dpl2 master catalog and prefix control
   the committed instances.
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
| Normal OpenROAD/DPL build (`BUILD_DPL2_LOCAL_TEST=OFF`) | Pass |
| Normal DPL C++ tests | 14/14 pass |
| dpl2-setting integration build (`BUILD_DPL2_LOCAL_TEST=ON`) | Pass |
| dpl2-setting integration C++ tests | 15/15 pass |
| dpl2-setting no-argument Tcl insertion smoke | Pass; 4 multi-height fillers |
| Focused legacy and multi-height Tcl regressions | 10/10 pass |
| `clang-format-18 --dry-run --Werror` | Pass |
| `git diff --check` | Pass |
| Changes under `src/dpl2` relative to the baseline | None |

Commands used for the final test pass:

```sh
cd src/dpl/test
../../../build-dpl/src/dpl/test/dpl_test --gtest_color=no
./regression fillers1 fillers2 fillers3 fillers4 fillers5 fillers6 \
  fillers7 fillers8 fillers_multi_height \
  fillers_multi_height_exact_failure

cd ../../..
cmake -S . -B build-dpl2-setting -DBUILD_DPL2_LOCAL_TEST=ON \
  -DBUILD_PYTHON=OFF
cmake --build build-dpl2-setting --target dpl_test -j2
cd src/dpl/test
../../../build-dpl2-setting/src/dpl/test/dpl_test --gtest_color=no
```

The normal build omits dpl2 integration intentionally; its no-argument form
fails with an explicit diagnostic while the historical positional command
continues to work. The dpl2-enabled build exercises the direct shared-setting
path.

## Final implementation map

- `src/FillerPlacementInternal.{h,cpp}`: deterministic rectangular planner,
  bounded exact backtracking, partial packing, origin masks, and forbidden
  master-abutment support.
- `src/FillerPlacement.cpp`: direct dpl2 `fillerSetting` consumption,
  including dpl2 master-ID avoid-pattern mapping; OpenDB/grid validation,
  atomic preflight/commit/rollback, metadata, and legacy command compatibility.
- `include/dpl/Opendp.h`, `src/Opendp.{i,tcl}`: direct setting adapter and
  no-argument insertion wiring; no DPL `set_filler_option` implementation.
- `test/FillerPlacementInternalTest.cpp`: 11 planner unit tests.
- `test/FillerPlacementOpenDbTest.cpp`: three real OpenDB transaction tests,
  including direct dpl2 `fillerSetting` consumption.
- `test/fillers_multi_height*` and `test/multi_height_fillers.lef`: success and
  fail-closed Tcl fixtures/goldens.
- `README.md` and DPL CMake/test registration: user contract and build wiring.

## Known boundaries

- Exact fallback search is intentionally bounded to 250,000 states and depth
  4,096; exceeding either budget fails closed before any OpenDB mutation.
- dpl2 `CheckDRC` remains owned by the dpl2 checker/repair path; classic DPL's
  OpenDB insertion consumes the remaining catalog, ordering, exact-fit, prefix,
  and avoid-pattern fields.
- Geometry follows classic DPL's uniform site-width and row-height grid model.
- Direct setting integration is compiled when the destination exposes the
  dpl2 runtime through `BUILD_DPL2_LOCAL_TEST`; builds without that runtime
  retain the legacy positional `filler_placement` interface.

The implementation commit is the commit containing this handoff. The branch is
published as `codex/dpl-multi-height-filler-insertion-dpl` in the project
repository.
