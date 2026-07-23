# fillerRepair — filler VT overlay repair

Updated: 2026-07-23.

`ImplantLayerChecker` is the caller-facing entry. It owns one
`FillerRepairEngine`, which borrows the initialized Grid/Network already owned
by DePlace and privately owns its oracle/planner snapshot. Neither layer
mutates UDM or Network Nodes. Infrastructure owns UDM-to-Grid/Network
synchronization after placement/master commits.

```cpp
ImplantLayerChecker checker(deplace->getGrid(), deplace->getNetwork());
checker.setFillerRepairDebugLogging(true);  // optional [fr][stage] transcript
checker.initFillerRepair(deplace->getDesMgr(), fillerSetting);

ipl::CheckResult placement = checker.precheckFillerRepair();
bool legal = checker.check(node, x, y, orient);
if (legal) {
  commitTargetAndFillers(checker.getFillerChanges());
}
// Infrastructure synchronizes Grid/Network with committed UDM first.
checker.updateFillerRepair(deplace->getDesMgr(), fillerSetting);
```

`precheckFillerRepair()` reports only `Gap` and `Overlap` inside coverage-required legal Grid
segments. A segment is a maximal run of valid pixels not reserved by
halo/padding, so blockage cuts, fragmented-row holes and legal reserved
whitespace are ignored. Warning diagnostics explain the location;
`isLegal=false` is the hard opto-blocking value. It does not inspect
target/master/candidates/IDs/implant DRC. Before registering a replacement
master, repair repeats the coverage check over the initial target influence.
If adaptive search later proposes a filler change outside those rows, that
request's expanded row range is checked before it enters the checker batch;
an illegal request is rejected without rejecting legal peers in the same
batch. A defect in checked rows returns `PrecheckFailed`, no solution for that
request and no changes. The whole-design `precheckFillerRepair()` remains the
caller's global gate. After any placement/master commit, infrastructure must
synchronize Network with UDM before callers run `updateFillerRepair()` to
rebuild checker/engine snapshots. Live reads are not a replacement for
refreshing snapshot row membership.

`check()` forwards its exact `ipl::CheckRequest` to the engine. Repair overlays
the requested target master and first asks the private oracle with empty filler
changes. A clean snapshot succeeds with empty changes; otherwise the internal
planner searches same-position/same-size filler swaps. Commit remains with
opto/infrastructure. Configured filler masters are registered in the existing
Network during init. On the checker path, the request master already belongs
to Network; repair validates target/type/dimensions and rebuilds its private
oracle if DePlace registered that master after init. The direct UDM-handle
overload retains lazy registration for focused engine tests.
This changes only the in-memory master registry, not UDM placement.

Filler classification normally follows the physical cell type. When a
destination exposes stale type metadata, fillerRepair also recognizes an
available cell/master name beginning with ASCII-case-insensitive `Fill`.
This exact-prefix fallback does not admit similarly named substrings, does not
expand the fillerSetting candidate universe, and does not change shared
infrastructure or checker code.

Implant metadata is read from the checker's accessor-based `Layer` model.
The engine matches each physical implant shape using
`Layer::getTechLayerId()`, then reads `Layer::Vt` and `Layer::Polar`; it does
not retain an `ImplantLayer` mirror or join metadata by layer name. Persistent
init diagnostics remain textual, so their unused-layer filter still compares
the checker-provided layer name embedded in the diagnostic message.

## Main files

| Path | Purpose |
|---|---|
| `FillerRepairEngine.h/.cpp` | checker-owned implementation; its `Impl` owns the oracle, planner snapshot, precheck, update and repair |
| `FillerClassification.h` | fillerRepair-local `Fill...` name fallback for stale destination cell-type metadata |
| `PlacementPrecheck.h/.cpp` | UDM-free gap/overlap coverage sweep used by the public precheck API and portable boundary tests |
| `FillerRepairPlanner.h/.cpp` | internal deterministic search pipeline and debug transcript |
| `OracleGate.h/.cpp` | owns `PlannerOracle` plus `OracleRequest/Result/Status`, batching, cache and baseline-delta gate |
| `PlannerDataSource.h/.cpp` | small planner-only data contract and candidate filter; implemented privately by `FillerRepairEngine::Impl` |
| `Types.h` | deliberately standalone leaf: planner IDs, geometry/model types and exact final-checker wire helpers |
| `Swap`, `Signature`, `Window`, `Ranker`, `SubsetSearch` | search stages |
| `sources.cmake` | source-of-truth lists for planner, runtime and portable tests |
| `test/FillerRepairPlannerTest.cpp` and same-level doubles | 83 portable database-free planner unit tests |
| `test/FillerRepairCheckerE2ETest.cpp` | 71 portable real-checker, planner-to-checker and internal precheck cases |
| `test/CMakeLists.txt` | standalone planner and E2E targets plus complete runtime-engine compile/link gate |

## Debug transcript

Debug output is disabled by default. Callers may call
`checker.setFillerRepairDebugLogging(true)` before or after initialization;
planner tests use
`FR_VERBOSE=1` on the unit-test executable. The deterministic transcript is printed as
`[fr][stage]` lines and records the request/configuration, normalized
violations, L0/adaptive-L1 windows, emitted swaps, ranked filler domains,
subset counts, checker batches/cache/budget, best non-clean candidate and the
final decision. Logging never changes search order or acceptance.

Planner `OracleRequest`, `OracleStatus` and requestId stay internal to
`OracleGate`; their change payload and the public result are both the exact
final-checker `ipl::FillerChanges`/`FillerCellRecord` wire. Destination builds compile
`DPL2_FILLER_REPAIR_SOURCES` from `sources.cmake` -- never a
hand-copied file list. Migration steps live in `src/dpl2/HandOff.md`.
The engine consumes only borrowed Grid/Network pointers and the idempotent
`Network::addMaster(PhysLibCell, Grid)` registration API. It never updates
Network Nodes. `update()` validates the infrastructure revision and replaces
only private checker/planner/precheck state. A stale or incomplete Network
makes update fail closed until infrastructure synchronizes/rebuilds it and a
later update succeeds; there is no repair-specific importer.

`Types.h` is intentionally not merged into Engine, Planner or OracleGate.
Geometry, diagnostics, violations and planner entry records are used by
multiple sibling stages, so merging them upward would reverse the dependency
direction. Only the oracle protocol is owned by `OracleGate.h`; only the public
runtime result is owned by `FillerRepairEngine.h`. The sole infrastructure-
sensitive `Network::addMaster()` call is centralized in the engine's private
master-registration seam.

## Portable verification

The 83 database-free planner tests and their same-level synthetic doubles
exercise every pure search stage without constructing UDM objects.

The 71 E2E tests construct checker input directly with the final checker's
`ImplantLayerCheckerHelper`. They do not parse DEF/LEF and do not need a
destination-specific UDM provider. The 71 cases comprise 26 final-checker
fixture/overlay cases, 33 planner-to-final-checker repairs/failures and 12
boundary cases for the exact coverage sweep behind `precheck()`. One direct
case pins detection when a changed neighbor lies outside the guard. The checker
fixtures contain eight dense rows and exercise intra/inter-row width/spacing,
candidate and budget boundaries, baseline-delta protection, deterministic
batching, multi-swap minimum width, adaptive-direction fallback for a
checker-legal three-swap repair and atomic no-partial failure semantics.
Every width/spacing checker and repair path runs with exact target-local
filler:std-cell ratios of 50:50, 30:70, 20:80, 10:90 and 5:95. Required
editable/bridge fillers remain present in each DRC core. The local
window definition, dense-placement risks and future fast-failure/span-rewrite
proposal are documented in `docs/filler_repair_dense_placement_analysis.md`.

```sh
cmake -S test -B test/build/e2e \
  -DDPL2_UDM_INCLUDE_DIRS='<real include dirs>' \
  -DDPL2_RUNTIME_LIBRARIES='<existing infra/checker targets>' \
  -DDPL2_UDM_LIBRARIES='<real libraries or CMake targets>'
cmake --build test/build/e2e
ctest --test-dir test/build/e2e --output-on-failure
```

The E2E executable consumes the complete `DPL2_FILLER_REPAIR_SOURCES`, so a
destination build cannot pass while `FillerRepairEngine.cpp` is incompatible
with its real UDM/infrastructure/checker headers. Supplying
`DPL2_RUNTIME_LIBRARIES` reuses the destination's owning targets; when omitted,
the standalone fallback compiles the sibling infrastructure/checker sources.

The 97-case fake-UDM checker/engine suite stays outside this directory
under `src/dpl2/test/local/`; see that directory's README for local commands
and dependency details.
