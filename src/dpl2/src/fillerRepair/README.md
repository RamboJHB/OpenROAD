# fillerRepair — Filler VT Overlay Repair Planner

Implements the pure repair planner from `docs/filler_vt_overlay_repair_spec.md`
(V2). Development is confined to `src/dpl2`; the checker integrates into the
infrastructure side, so until the real checker / infrastructure APIs land the
planner is built and tested against the fakes in `fake/`.

## Layout

| Path | Content |
|---|---|
| `Types.h` | Base ids, intervals, wire types (`TargetPlace`, `FillerChange`, `Violation`, `OverlayCheckRequest`, `CheckResult`), planner entry types, coverage types |
| `PlacementView.h` | Read-only DB view interface (real adapter wraps the UDM design) |
| `CheckerApi.h` | Abstract `ImplantOverlayChecker` (spec §5.2 protocol) |
| `CandidateApi.h` | Abstract `FillerMasterCandidateProvider` (spec §5.3) |
| `Move.h/.cpp` | Internal span-rewrite Move, `SwapMove`, canonical key, conflict, wire adapter, coverage invariants (spec §4) |
| `Preflight.h/.cpp` | 100% utility preflight (spec §6.1) |
| `FillerRepairEngine.h/.cpp` | Planner entry + pipeline skeleton (spec §3.2) |
| `fake/FakeDesign.h` | In-memory `PlacementView` with fluent builders |
| `fake/FakeCandidateProvider.h` | Same-size replacement lookup over the fake library |
| `fake/FakeImplantChecker.h/.cpp` | Rule-parameterized oracle locking the request/result protocol |
| `test/` | Unit tests + runner |

## Conventions

- All x coordinates are DBU; intervals are half-open `[xl, xh)`; `Region` row
  ranges are inclusive. The wire-level `Rect` conversion is an adapter concern.
- The planner is deterministic and never mutates the design; commit belongs to
  the infrastructure.
- The fake checker's MW/MS model is a deliberate simplification. Planner code
  must not depend on its details — that is the checker-as-oracle boundary.
- Debug prints (`RepairConfig::verbose`, `[fr][stage] ...`) state cause →
  effect with the concrete data involved, so a transcript reads as the
  engine's decision chain.

## Build & test

Standalone (STL only) until dpl2 joins the CMake build (spec §11 TODO 12):

```sh
test/run_tests.sh              # quiet
FR_VERBOSE=1 test/run_tests.sh # with the [fr] debug transcript
```

## Status vs spec §11 TODO

| # | Item | Status |
|---|---|---|
| 1 | Planner API, internal Move, canonical key/conflict, wire adapter | done |
| 2 | Fake checker + fake candidate provider, protocol locked by tests | done |
| 3 | 100% utility preflight with fatal short-circuit | done |
| 4 | Violation normalization + signature matching | next |
| 5 | L0/L1/L2 window builder + guardRegion | pending |
| 6 | Swap move generator + bridge fillers + group hints | pending |
| 7 | Ranker | pending |
| 8 | Subset searcher | pending |
| 9 | Oracle gate (batch, cache, baseline-delta) | pending |
| 10 | Final full-overlay check + diagnostics | pending |
| 11 | Full spec test set | partial (TODO 1-3 cases) |
| 12 | Real checker adapter + CMake integration | pending |
