# fillerRepair — filler VT overlay repair

Updated: 2026-07-19.

`FillerRepairEngine` is the only production entry. It borrows the initialized
Grid/Network already owned by DePlace and privately owns only its final
checker/view, then exposes a placement-only precheck plus pre-commit filler
repair. It never mutates UDM. The destination supplies the existing
infrastructure/checker source types; only this directory is migrated and none
of those supplied sources is patched.

```cpp
FillerRepairEngine engine(deplace->getGrid(), deplace->getNetwork());
engine.setDebugLogging(true);  // optional [fr][stage] transcript
engine.init(deplace->getDesMgr(), fillerSetting);

ipl::CheckResult placement = engine.precheck();  // opto calls before mutation
RepairOutcome outcome = engine.repair(targetCell, newMaster);
```

Precheck reports only `Gap` and `Overlap` inside coverage-required legal Grid
segments. A segment is a maximal run of valid pixels not reserved by
halo/padding, so blockage cuts, fragmented-row holes and legal reserved
whitespace are ignored. Warning diagnostics explain the location;
`isLegal=false` is the hard opto-blocking value. It does not inspect
target/master/candidates/IDs/implant DRC. Repair never calls precheck.

Repair overlays `newMaster` and first asks the final checker with empty filler
changes. A clean snapshot succeeds with empty changes; otherwise the internal
planner searches same-position/same-size filler swaps. Commit remains with
opto/infrastructure. Configured filler masters are registered in the existing
Network during init. An uninstantiated target `newMaster` is registered lazily
by repair, followed by a private checker/view rebuild; this changes only the
in-memory master registry, not UDM placement.

## Main files

| Path | Purpose |
|---|---|
| `FillerRepairEngine.h/.cpp` | production API; borrows Grid/Network and owns final checker, view/oracle, precheck and repair |
| `PlacementPrecheck.h/.cpp` | UDM-free gap/overlap coverage sweep used by the public precheck facade and portable boundary tests |
| `FillerRepairPlanner.h/.cpp` | internal deterministic search pipeline and debug transcript |
| `OracleGate.h/.cpp` | internal checker abstraction, batching, cache and baseline-delta gate |
| `PlacementView.h/.cpp` | planner-only read view and candidate filter |
| `Types.h` | planner-internal IDs, geometry and request/result types |
| `Swap`, `Signature`, `Window`, `Ranker`, `SubsetSearch` | search stages |
| `sources.cmake` | source-of-truth lists for planner, production and portable E2E |
| `test/FillerRepairCheckerE2ETest.cpp` | 33 portable real-checker, planner-to-checker and internal precheck cases |
| `test/CMakeLists.txt` | standalone destination E2E target |

## Debug transcript

Debug output is disabled by default. Production callers may call
`engine.setDebugLogging(true)` before or after `init()`; planner tests use
`FR_VERBOSE=1` on the unit-test executable. The deterministic transcript is printed as
`[fr][stage]` lines and records the request/configuration, normalized
violations, L0/adaptive-L1 windows, emitted swaps, ranked filler domains,
subset counts, checker batches/cache/budget, best non-clean candidate and the
final decision. Logging never changes search order or acceptance.

Planner `OverlayCheckRequest`, `CheckStatus` and requestId stay internal to
`OracleGate`; the public API uses final checker `CheckResult`, `Diagnostic`,
`FillerChanges` and `FillerCellRecord`. Destination builds compile
`DPL2_FILLER_REPAIR_PRODUCTION_SOURCES` from `sources.cmake` -- never a
hand-copied file list. Migration steps live in `src/dpl2/HandOff.md`.
The facade consumes only borrowed Grid/Network pointers and the idempotent
`Network::addMaster(PhysLibCell, Grid)` registration API; there is no
repair-specific importer.

## Portable final-checker verification

The migrated tests construct checker input directly with the final checker's
`ImplantLayerCheckerHelper`. They do not parse DEF/LEF and do not need a
destination-specific UDM provider. The 33 cases comprise four direct checker
overlay contracts, 17 planner-to-final-checker repairs/failures and 12
boundary cases for the exact coverage sweep behind `precheck()`. The checker
fixtures contain eight dense rows and exercise intra/inter-row width/spacing,
candidate and budget boundaries, baseline-delta protection, deterministic
batching, multi-swap minimum width and no-partial-result failure semantics.

```sh
cmake -S test -B test/build/e2e \
  -DDPL2_UDM_INCLUDE_DIRS='<real include dirs>' \
  -DDPL2_UDM_LIBRARIES='<real libraries or CMake targets>'
cmake --build test/build/e2e
ctest --test-dir test/build/e2e --output-on-failure
```

The complete repository-local regression copy—including all 81 planner unit
cases, their doubles, the UDM-compatible test data provider and its runners—is
outside this directory at `src/dpl2/test/local/`. It is not part of the copied
payload.
