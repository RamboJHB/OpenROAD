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
| Infrastructure | existing production Grid/Network are borrowed; engine owns only final checker/view, registers configured filler masters at init and target master lazily; empty `getFillerMasters()` errors out |
| Checker | final blocking contract, Node/Master IDs and FillerCellRecord wire |
| Production API | one `FillerRepairEngine` = precheck + private view/oracle + repair; fails closed before a successful `init()` |
| E2E | 52 cases in `fillerRepair/test/e2e_test.cpp`; every behavior has 3 cases and every fixture has at least 5 standard rows; fake UDM is the only data substitute |
| CMake | standalone GoogleTest/CTest harness passes 133/133 normal and ASan; source/test lists live in `src/fillerRepair/sources.cmake`; `DPL2_TEST_USE_FAKE_UDM=OFF` builds the real-UDM compile gate |

## Fixed decisions

1. The planner is deterministic, non-mutating and UDM-free.
2. This stage supports same-position/same-size filler swaps only.
3. `ImplantLayerChecker` is the only DRC oracle.
4. `FillerRepairEngine` is the only production planner boundary.
5. Opto calls public precheck before mutation; it checks gap/overlap only
   inside maximal supplied-Grid runs where pixels are valid and not reserved
   by halo/padding. Blockage cuts and legal empty regions are outside scope.
6. Candidates come only from `fillerSetting::getFillerMasters()`.
7. Instance/master IDs are `Node::getId()` / `Master::getId()`; physical wire
   handles are `LeafCellID` / `LibCellID` in `FillerCellRecord`.
8. Existing Network supplies placed masters. Configured filler masters are
   registered before init-time checker construction; an uninstantiated target
   master is registered by `repair()` followed by a private checker/view rebuild.
9. Checker batches use one target/guard plus ordered `FillerChanges`; checker
   computes the empty-overlay baseline and returns blocking violations.
10. Construct and init a new engine after a design commit.

## Production facade boundary

`FillerRepairEngine(grid, network)` borrows the already initialized production
objects. `init(desMgr, fillerSetting)` then:

- validates Grid/Network, `fillerSetting`, `PhysDesMgr` and UDM Session describe
  one active design;
- registers every configured filler master in the existing Network;
- constructs and validates the private final checker/view;
- lets `repair()` register an uninstantiated target replacement and rebuild the
  private checker/view only when that master is first encountered.

The engine owns only its final checker and planner view. The caller passes the
Grid/Network already owned by DePlace and never constructs another
repair-specific infrastructure object. No leaf-cell list, target master or
placement properties are accepted by init.

## Thread/lifetime model

One engine borrows infrastructure and privately owns checker/view for one
design snapshot. Precheck and repair do not mutate UDM; repair may idempotently
extend Network's in-memory master registry. Repair does not call precheck.
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
contains production Grid/Network/checker/engine/planner and fake UDM headers
only. Test-only fixture code wires Grid/Network from fake UDM data. Private
planner doubles remain confined to the unit executable;
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
