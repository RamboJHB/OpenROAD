# AGENTS.md — dpl2 filler repair project memory

Updated: 2026-07-18. Branch: `claude/wizardly-carson-secahu`.

Read `docs/filler_vt_overlay_repair_spec.md`, `src/dpl2/HandOff.md`,
`src/dpl2/src/drc/CHECKER_REPAIR_CONTRACT.md` and the fillerRepair/test READMEs
before changing this feature.

## Project status

The V2.1 swap-only planner and fake-UDM-only production E2E are complete.

| Area | State |
|---|---|
| Planner | internal `FillerRepairPlanner`; adaptive-L1, filler domains, per-band ranking/filtering, deterministic output and opt-in `[fr][stage]` transcript complete |
| Unit tests | 81/81 GoogleTests normal and ASan |
| Infrastructure | private inside `FillerRepairEngine`; one init builds/owns Network, Grid and final checker from PhysDesMgr data; empty `getFillerMasters()` errors out |
| Checker | final blocking contract, Node/Master IDs and FillerCellRecord wire |
| Production API | one `FillerRepairEngine` = precheck + private view/oracle + repair; fails closed before a successful `init()` |
| E2E | 39 cases in `fillerRepair/test/e2e_test.cpp`; every behavior has 3 cases and every fixture has at least 5 standard rows; fake UDM is the only data substitute |
| CMake | standalone GoogleTest/CTest harness passes 120/120 normal and ASan; source/test lists live in `src/fillerRepair/sources.cmake`; `DPL2_TEST_USE_FAKE_UDM=OFF` builds the real-UDM compile gate |

## Fixed decisions

1. The planner is deterministic, non-mutating and UDM-free.
2. This stage supports same-position/same-size filler swaps only.
3. `ImplantLayerChecker` is the only DRC oracle.
4. `FillerRepairEngine` is the only production planner boundary.
5. Opto calls public precheck before mutation; it checks gap/overlap only.
6. Candidates come only from `fillerSetting::getFillerMasters()`.
7. Instance/master IDs are `Node::getId()` / `Master::getId()`; physical wire
   handles are `LeafCellID` / `LibCellID` in `FillerCellRecord`.
8. All placed, target-new and configured filler masters are registered before
   checker construction, including uninstantiated masters.
9. Checker batches use one target/guard plus ordered `FillerChanges`; checker
   computes the empty-overlay baseline and returns blocking violations.
10. Construct and init a new engine after a design commit.

## Production facade boundary

`FillerRepairEngine::init(desMgr, leafCellIds, fillerSetting,
targetNewMaster)` performs one private snapshot build:

- validates `fillerSetting` and `PhysDesMgr` belong to the same design;
- derives core/rows and real Grid state from `PhysDesMgr`;
- creates deterministic Network master IDs in LibCellID order;
- imports Nodes by reading each supplied LeafCellID back through PhysDesMgr;
- includes uninstantiated target/candidate masters;
- paints production Grid occupancy.

The engine then constructs and owns its final checker and planner view. The
caller owns hierarchy traversal and supplies leaf IDs, but never constructs
Grid, Network, a repair checker or another repair-specific object. No placement
properties are accepted from the caller.

## Thread/lifetime model

One engine privately owns the infrastructure/checker/view for one design
snapshot. Precheck and repair are non-mutating; repair does not call precheck.
Checker calls are serialized privately per engine. `init()` is one-shot;
create another engine for a new revision.

## Tests

```sh
src/dpl2/src/fillerRepair/test/run_tests.sh
SANITIZE=address src/dpl2/src/fillerRepair/test/run_tests.sh
src/dpl2/src/fillerRepair/test/run_e2e_tests.sh
SANITIZE=address src/dpl2/src/fillerRepair/test/run_e2e_tests.sh
```

The E2E source, runner and only fake-UDM include tree live under
`fillerRepair/test`, so they travel with the directory being ported. Its target
contains production Grid/Network/importer/checker/engine/planner and fake UDM
headers only. Private planner doubles remain confined to the unit executable;
never link them into E2E or production targets.

The destination ports only `fillerRepair/`; existing infrastructure/checker
must remain unmodified. Fake versus real UDM is selected only through the test
CMake interface target's include/link settings.

Local dependencies are GoogleTest, Boost, TBB, C++20 and CMake. On Apple ASan, use the
static Homebrew TBB archive as encoded in both build entry points.

## Change rules

- Keep planner algorithms unchanged unless a production-chain regression
  demonstrates a planner defect.
- Checker algorithm changes remain checker-RD-owned. Record any checker source
  compatibility edit in `CHECKER_REPAIR_CONTRACT.md`.
- Do not add another production abstraction beside `FillerRepairEngine`.
- Add/remove production sources only via `src/fillerRepair/sources.cmake`.
- Run planner and E2E normal + ASan before commit.
- Keep changes in `src/dpl2/` and the authoritative spec unless scope expands.
