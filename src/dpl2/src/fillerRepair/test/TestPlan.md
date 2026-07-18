# Test Plan — fillerRepair

Updated: 2026-07-19.

## Tiers

| Tier | Command | Boundary |
|---|---|---|
| Real-UDM compile gate | `fillerRepair/test/CMakeLists.txt` | complete production chain plus all 52 E2E case objects against real UDM |
| Real-UDM E2E | `DPL2_REAL_UDM_PROVIDER_SOURCE=<provider.cpp>` | the same 52 cases linked with the destination's real-UDM fixture |

Normal and AddressSanitizer runs are required. Production builds use
`-Wall -Wextra -Werror`.

On macOS the scripts set `ASAN_OPTIONS=detect_container_overflow=0` because
Homebrew's GoogleTest static archive is not sanitizer-instrumented; this avoids
a libc++ annotation mismatch during test discovery. AddressSanitizer remains
enabled for every project source and test execution.

## Production E2E composition

Included: production Grid/Network, fillerSetting, final ImplantLayerChecker,
FillerRepairEngine (borrowing that infrastructure), internal
FillerRepairPlanner and all search stages (compile lists from
`src/fillerRepair/sources.cmake`). The destination's real-UDM fixture provides
tech/library/row/cell data. A test-only provider exposes the Grid/Network wiring
that DePlace already performs in production; no importer exists in the engine.

`e2e_cases.cpp`, `E2ETestProvider.h`, real-UDM CMake and this plan live below
`fillerRepair/test`, so an entire-directory port carries every production E2E
assertion. `sources.cmake` exports the E2E source list.

The provider boundary is data-only: create/activate a canonical design, expose
the Grid/Network already wired as production does, resolve semantic cell/master
roles, move a cell for gap/overlap setup and snapshot physical records. Engine
construction, calls and assertions remain exclusively in the case source.

All 81 planner unit cases and their test doubles are maintained in the separate
repository-local regression copy at `src/dpl2/test/local/planner/`; none is
part of this real-UDM migration test tree.

## Required precheck cases

1. Clean placement: `isLegal=true`, no diagnostics.
2. Gap: `isLegal=false`, at least one `Gap` warning diagnostic.
3. Overlap: `isLegal=false`, at least one `Overlap` warning diagnostic.
4. A gap entirely inside a hard blockage is outside the legal Grid segments
   and remains legal.
5. A gap entirely inside an instance halo/padding reservation is outside the
   legal Grid segments and remains legal.
6. With the same blocked row tail, a gap inside the remaining valid,
   unreserved segment still returns `isLegal=false` and `Gap`.

Each case snapshots all fixture cell origins, masters, status and orientation
before/after precheck and requires equality. The tests do not pass target,
newMaster or candidate information to precheck, proving that scope separation.

All six behaviors are each instantiated for Canonical, ShiftedOrigin and
FarShiftedOrigin: three independently discovered testcases per behavior.

## Required repair cases

- `FillerRepairEngine(Grid*, Network*)` reuses the supplied initialized
  infrastructure and owns only the final checker/view; callers construct no
  repair-specific infrastructure object.
- `init(PhysDesMgr*, fillerSetting)` accepts no leaf-cell list or target master.
- Target new master is checked as an overlay with empty filler changes first.
- A clean target overlay returns success with an empty change list.
- Existing violating fixture returns the deterministic FH2 filler replacement.
- Output is an exact `FillerCellRecord` in `ipl::FillerChanges`.
- Repeated repair returns identical output.
- Physical UDM snapshots remain unchanged after each repair.
- Persistent checker init diagnostics (duplicated per result by the checker)
  never mark candidates illegal; repair still succeeds.
- Every configured filler master, including an uninstantiated one, is
  registered in the existing Network by engine initialization.
- An uninstantiated target new master is absent after init, registered lazily
  by the first repair, and visible to the rebuilt checker/view.
- An empty `getFillerMasters()` allow list fails engine initialization and its
  reason is preserved in standard result diagnostics.
- A PhysDesMgr different from the UDM Session current design is rejected before
  private checker construction.
- Before/after a failed `init()`, `precheck()` and `repair()` fail closed.
- Null borrowed Grid/Network fails init with `missing_infrastructure`.
- A second `init()` is rejected without damaging the first ready snapshot.
- Frame gates: a trailing pad row (any origin) is accepted; a LEADING pad row
  is refused by the frame-coherence gate (`RowFrameMismatch`) because the
  checker's Grid frame skips pad rows while its init/track frame does not; a
  standard row off the shared origin frame stays refused
  (`RowOriginMisaligned`/`ColFrameMismatch`), with the origin baseline on the
  first NON-pad row.
- The separate local planner suite continues covering adaptive-L1, ranking,
  subset enumeration, budgets, cache, baseline-delta and malformed internal
  oracle protocol.
- `FR_VERBOSE=1` enables the deterministic `[fr][stage]` algorithm transcript;
  normal test runs remain silent apart from GoogleTest output.

Every repair/init behavior above is also instantiated for the three shared-row
origins. Row-frame validation has four dedicated cases (aligned shifted,
pad-before-standard refused, pad-after-standard accepted, misaligned-with-pad
refused). Thus every E2E behavior owns at least three testcases, and every
fixture constructs at least five standard rows.

## Commands

```sh
cmake -S fillerRepair/test -B build-real \
  -DDPL2_REAL_UDM_INCLUDE_DIRS='<real UDM includes>' \
  -DDPL2_REAL_UDM_LIBRARIES='<real UDM targets/libraries>' \
  -DDPL2_REAL_UDM_PROVIDER_SOURCE='<RealUdmE2ETestProvider.cpp>'
cmake --build build-real
ctest --test-dir build-real -R '^real-udm\.'
```

Repository-local fake regression commands are documented outside this directory
in `src/dpl2/test/local/README.md`.

2026-07-19 local results: planner 81/81 and production E2E 52/52 in both
normal and ASan builds; full CTest 133/133; `-Wall -Wextra -Werror` clean.

## Regression rules

- Repair must never invoke placement precheck.
- Precheck diagnostics may be warnings, but gap/overlap must set
  `isLegal=false` so opto can block.
- Neither public API mutates the DB.
- Lazy master registration may extend Network's in-memory master registry but
  must not change any UDM physical record.
- Production signatures must not expose planner requestId/status types.
- No planner double may enter a production/E2E link target.
- Every test source below `fillerRepair/test` must compile against real UDM.
- Planner doubles and UDM-compatible local test data must remain outside
  `fillerRepair/`.
- Production delivery contains fillerRepair only; supplied infra/checker must
  remain at zero diff.
