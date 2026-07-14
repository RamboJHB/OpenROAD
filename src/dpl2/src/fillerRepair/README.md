# fillerRepair — Filler VT Overlay Repair Planner

Implements the pure repair planner from `docs/filler_vt_overlay_repair_spec.md`.
Development is confined to `src/dpl2`; the checker integrates into the
infrastructure side. The real checker overlay API has landed, but the real UDM
extraction, adapter, and build integration have not, so the planner still runs
end-to-end against the fakes in `fake/`.

> **V2.1 rollout (spec §0), three batches:** **batch 1 (OracleGate correctness,
> #1/#2/#3/#4/#5/#10) DONE.** **Batch 2: #6/#7/#8/#11 DONE**
> (unfixable→warning, dropped L2, adaptive-L1, dropped final check).
> **Batch 3: #9 filler-domain enumeration DONE** (ranker
> returns `FillerDomain`s; caps count fillers); **#12 precheck upstreaming
> pending** (goes with the adapter). 79 planner tests green. Plan and pointers in
> `src/dpl2/HandOff.md`. Dependency topology (checker calls engine, engine
> calls checker through its own abstract oracle — no cycle) is pinned in spec
> §3.3.
>
> **Checker status (contract FINAL, 2026-07-13):** the user pinned the overlay
> contract to **list-only, no filtering, no dedup** — `checkPlaceWithOverlays`
> returns every guard-clipped violation per candidate (the blocking filter and
> its helpers are deleted; the raw/blocking split is gone — the one API IS
> raw). Duplicates (per band/direction) are allowed but deterministic; the
> engine's one-to-one multiset matching tolerates them as long as the original
> snapshot and the baseline come from this same API. The 2026-07-13 RD drop
> carries the inline UDM extraction; the scan-correctness fixes and the
> `DPL2_FAKE_UDM` boundary are re-applied on top (see
> `drc/CHECKER_REPAIR_CONTRACT.md`). Standalone harness: 9 cases green under
> `-Werror` and ASan. Pending the adapter the engine stays on the fakes.
> **Upcoming:** infrastructure is being updated; the planner/engine will move
> to an infra-backed version with no UDM dependency (spec §5.2.1, AGENTS D21).

## Layout

| Path | Content |
|---|---|
| `BaseTypes.h` | Planner-owned UDM-free ids and half-open `XInterval`; deliberately separate from checker `ipl::` types |
| `Types.h` | Wire types (`TargetPlace`, `FillerChange`, `Violation`, `OverlayCheckRequest`, `CheckResult`) and planner entry/coverage types |
| `PlacementView.h` | Read-only DB view interface (real adapter wraps the UDM design) |
| `CheckerApi.h` | Abstract `ImplantOverlayChecker` (spec §5.2 protocol) |
| `CandidateApi.h` | Abstract `FillerMasterCandidateProvider` (spec §5.3) |
| `Swap.h/.cpp` | `Swap` (the stage's atomic operation: FillerChange + geometry metadata), overlay cache key (spec §4) |
| `PreCheck.h/.cpp` | 100% utility pre-check (spec §6.1) |
| `Signature.h/.cpp` | Violation normalization, pinned signature matching, change-relatedness (spec §6.2) |
| `Window.h/.cpp` | L0 builder + adaptive-L1: grow K contiguous fillers per blocking side/relevant row, coupled rows ±1, fixed-boundary and unchanged-blocking cutoffs; guardRegion two-cell ring (spec §6.2/6.3) |
| `SwapGenerator.h/.cpp` | Swap generator: atomic swaps only, no group/seed machinery (spec §6.5) |
| `Ranker.h/.cpp` | Ranks fillers (direct/bridge/width/position) and returns `FillerDomain`s — each filler's full master domain, VT-preference ordered with the third VT demoted within the domain; realizes anchor-follow (spec §6.6, V2.1 #9) |
| `SubsetSearch.h/.cpp` | Enumerates filler combinations × per-domain assignments in pinned order; member caps count fillers; complete-space rule (spec §6.7, V2.1 #9) |
| `OracleGate.h/.cpp` | Baseline + batched checker calls, protocol validation, result cache, baseline-delta gate (spec §6.8). V2.1 batch-1 target: two-way self-consistency, BaselineMismatch gate, multiset delta, per-violation ruleDistance (#1–#5) |
| `FillerRepairEngine.h/.cpp` | Planner entry + pipeline skeleton (spec §3.2) |
| `fake/FakeDesign.h` | In-memory `PlacementView` with fluent builders |
| `fake/FakeCandidateProvider.h` | Same-size replacement lookup over the fake library |
| `fake/FakeUdmCandidateProvider.h/.cpp` | Adapter rehearsal: UDM-style master catalog (layers named `FAMILY_POLARITY`, band shapes) with the checker's derivation rules — VT = implant-layer family, never the master name; `describeMasters(ids)` -> width/VT per id; implements the spec §5.3 provider; ships the appendix-A 12-master library |
| `fake/FakeImplantChecker.h/.cpp` | Rule-parameterized oracle locking the request/result protocol |
| `test/` | Unit tests + runner |

## Conventions

- Base ids (`DbCoord`, `InstanceId`, `MasterId`, `RowId`, `LayerId`) and
  `XInterval` are owned by `fillerRepair/BaseTypes.h`. The checker owns its
  distinct `ipl::` types; the adapter must convert explicitly. The checker
  harness includes both headers in one translation unit to prevent the old ODR
  collision from returning. The now-unreferenced `drc/ImplantBaseTypes.h` is
  retained until checker RD/build integration confirms it can be removed.
  `PlacedInstance` / `MasterInfo` remain adapter-side projections of the
  infrastructure `Node` / `Master` classes.
- The operation vocabulary is **swap** (this stage) and **rewrite** (future).
  There is no generic "Move" abstraction.

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
SANITIZE=address test/run_tests.sh

cd ../drc/test
./run_tests.sh                 # real checker core, fake UDM boundary, 8 cases
SANITIZE=address ./run_tests.sh
```

## Status vs spec §11 TODO

TODO 1–11 and all V2.1 engine revisions are implemented for the current
single-VT planner projection (79 deterministic tests green).
TODO 12 (real checker/infra adapter + CMake) is pending. The spec is at **V2.1**;
folding the 12 revisions into this code is tracked as three batches in
`src/dpl2/HandOff.md`.

| # | Item | Status |
|---|---|---|
| 1 | Planner API, `Swap` struct, overlay cache key | done |
| 2 | Fake checker + fake candidate provider, protocol locked by tests | done |
| 3 | 100% utility pre-check with fatal short-circuit | done (V2.1 #12: move authority to infra cache) |
| 4 | Violation normalization + signature matching | done |
| 5 | Window builder + guardRegion + unfixable check | done (V2.1 #6/#7/#8: warning-only, drop L2, adaptive-L1) |
| 6 | Swap generator (atomic swaps only) | done |
| 7 | Ranker (filler domains, third-VT demotion in-domain) | done for current single-VT projection; true P/N per-band majority awaits adapter metadata |
| 8 | Subset searcher (filler combos × domain assignments) | done; V2.1 #9 applied (caps count fillers); exact `space == budget` completeness fixed |
| 9 | Oracle gate (batch, cache, baseline-delta, protocol validation) | done; V2.1 #1–#5 correctness fixes applied (batch 1) |
| 10 | Window escalation + diagnostics | done; V2.1 #10 applied; #11 final check dropped (batch 2) |
| 11 | Full spec test set | done for current fake-checker scope (79 planner + 8 real-checker-core tests) |
| 12 | Real checker adapter + CMake integration | pending |
