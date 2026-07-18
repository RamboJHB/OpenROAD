# AGENTS.md — dpl2 filler repair project memory

Updated: 2026-07-18. Branch: `claude/wizardly-carson-secahu`.

Read `docs/filler_vt_overlay_repair_spec.md`, `src/dpl2/HandOff.md`,
`src/dpl2/src/drc/CHECKER_REPAIR_CONTRACT.md` and the fillerRepair/test READMEs
before changing this feature.

## Project status

The V2.1 swap-only planner and provider-neutral production E2E are complete.

| Area | State |
|---|---|
| Planner | internal `FillerRepairPlanner`; adaptive-L1, filler domains, per-band ranking/filtering, deterministic output and opt-in `[fr][stage]` transcript complete |
| Unit tests | 81/81 GoogleTests normal and ASan |
| Infrastructure | existing production Grid/Network are borrowed; engine owns only final checker/view, registers configured filler masters at init and target master lazily; empty `getFillerMasters()` errors out |
| Checker | final blocking contract, Node/Master IDs and FillerCellRecord wire |
| Production API | one `FillerRepairEngine` = precheck + private view/oracle + repair; fails closed before a successful `init()` |
| E2E | 52 cases in `fillerRepair/test/e2e_cases.cpp`; one assertion source links with either a real-UDM or local fake-UDM data provider |
| CMake | portable `fillerRepair/test/CMakeLists.txt` carries all unit/real-UDM cases; repository-local fake harness passes 133/133 normal and ASan |

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
src/dpl2/test/local/run_planner_tests.sh
SANITIZE=address src/dpl2/test/local/run_planner_tests.sh
src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address src/dpl2/test/local/run_fake_udm_e2e.sh
```

`fillerRepair/test` travels with production and contains the 81 unit cases,
the 52 provider-neutral E2E assertions, the provider contract and portable
CMake. The fake UDM include tree/provider/runners live only under
`src/dpl2/test/local`; they are not part of the migration payload. Planner
doubles are test support below `fillerRepair/test/support/planner` and never
link into E2E or production.

The destination copies only `fillerRepair/`; existing infrastructure/checker
remain unmodified. Real and local E2E runners compile the same case source and
differ only in the linked `E2ETestProvider` implementation.

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
