# AGENTS.md — dpl2 filler repair project memory

Updated: 2026-07-20. Branch: `claude/wizardly-carson-secahu`.

Read `docs/filler_vt_overlay_repair_spec.md`, `src/dpl2/HandOff.md`,
`src/dpl2/src/drc/CHECKER_REPAIR_CONTRACT.md` and the fillerRepair/test READMEs
before changing this feature.

## Project status

The V2.1 swap-only planner and real-UDM runtime E2E package are complete.

| Area | State |
|---|---|
| Planner | internal `FillerRepairPlanner`; adaptive-L1, filler domains, per-band ranking/filtering, deterministic `FillerCellRecord` output and opt-in `[fr][stage]` transcript complete |
| Unit tests | 82/82 database-free GoogleTests under `fillerRepair/test/planner`; portable with production |
| Infrastructure | existing runtime Grid/Network are borrowed; `Network::updateNodes()` refreshes an unchanged instance set; engine registers configured filler masters at init and validates target replacements before lazy registration |
| Checker | final blocking contract, Node/Master IDs and FillerCellRecord wire |
| Runtime API | one `FillerRepairEngine` = precheck + update + private snapshot/oracle + repair; init/update failures are fail-closed |
| Portable tests | 116 GoogleTests: 82 planner cases plus 34 final-checker/planner/precheck E2E cases; no destination fixture provider |
| Local regression | 82 fake-UDM engine cases; repository CTest total 198 |
| CMake | `fillerRepair/sources.cmake` exports runtime/planner/precheck/test source sets; the standalone test CMake accepts destination UDM include/link inputs |

## Fixed decisions

1. The planner is deterministic and non-mutating. Its algorithm uses dense
   integer IDs; request/result changes deliberately reuse the checker
   `FillerCellRecord`, supplied by real UDM or the test-only fake UDM.
2. This stage supports same-position/same-size filler swaps only.
3. `ImplantLayerChecker` is the only DRC oracle.
4. `FillerRepairEngine` is the only runtime planner boundary.
5. Opto calls public precheck before mutation; it checks gap/overlap only
   inside maximal supplied-Grid runs where pixels are valid and not reserved
   by halo/padding. Blockage cuts and legal empty regions are outside scope.
6. Candidates come only from `fillerSetting::getFillerMasters()`.
7. Instance/master IDs are `Node::getId()` / `Master::getId()`; physical wire
   handles are `LeafCellID` / `LibCellID` in `FillerCellRecord`.
8. Existing Network supplies placed masters. Configured filler masters are
   registered before init-time checker construction; `repair()` validates the
   target, standard-cell type and dimensions before lazily registering an
   uninstantiated replacement and rebuilding its private snapshot.
9. Checker batches use one target/guard plus ordered `FillerChanges`; checker
   computes the empty-overlay baseline and returns blocking violations.
10. After a same-instance-set position/master commit, call `update()`; rebuild
    Grid/Network first when rows, blockages or the instance set changed.
11. Adaptive expansion follows the best residual's side first, but tries the
    opposite side if the primary side cannot add a filler. Unchanged blocking
    alone is not a valid cutoff for non-monotone multi-swap repair.

## Engine boundary

`FillerRepairEngine(grid, network)` borrows the already initialized runtime
objects. `init(desMgr, fillerSetting)` then:

- validates Grid/Network, `fillerSetting`, `PhysDesMgr` and UDM Session describe
  one active design;
- registers every configured filler master in the existing Network;
- constructs and validates the private final checker/snapshot;
- lets `repair()` validate and register an uninstantiated target replacement,
  then rebuild the private checker/snapshot only when needed.

The engine owns only its final checker and immutable planner snapshot. The caller passes the
Grid/Network already owned by DePlace and never constructs another
repair-specific infrastructure object. No leaf-cell list, target master or
placement properties are accepted by init.

## Thread/lifetime model

One engine borrows infrastructure and privately owns checker/snapshot for one
design snapshot. Precheck and repair do not mutate UDM; repair may idempotently
extend Network's in-memory master registry after request validation and repeats
precheck internally. Checker calls are serialized privately per engine.
`update()` refreshes existing Nodes and replaces the private snapshot; failure
leaves the engine fail-closed.

## Tests

```sh
src/dpl2/test/local/run_planner_tests.sh
SANITIZE=address src/dpl2/test/local/run_planner_tests.sh
src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address src/dpl2/test/local/run_fake_udm_e2e.sh
```

`fillerRepair/test` travels with runtime and contains 116 portable GoogleTests:
82 database-free planner cases/doubles plus 34 final-checker/planner/precheck
E2E cases. E2E data is built with `ImplantLayerCheckerHelper`; it needs no
DEF/LEF reader or destination fixture provider. The local UDM-compatible
include tree, provider and 82 engine cases remain under `src/dpl2/test/local`.

The destination copies `fillerRepair/` and preserves the infrastructure
`Network::updateNodes()` seam; checker sources remain unmodified. Every test
source copied with `fillerRepair/` targets real UDM wire types.

Local dependencies are GoogleTest, Boost, TBB, C++17/C++20 and CMake. On Apple ASan, use the
static Homebrew TBB archive as encoded in both build entry points.

## Change rules

- Keep planner algorithms unchanged unless a final-checker runtime-chain
  regression demonstrates a planner defect.
- Checker algorithm changes remain checker-RD-owned. Record any checker source
  compatibility edit in `CHECKER_REPAIR_CONTRACT.md`.
- Do not add another runtime abstraction beside `FillerRepairEngine`.
- Add/remove runtime or portable test sources only via
  `src/dpl2/src/fillerRepair/sources.cmake`.
- Run planner and E2E normal + ASan before commit.
- Keep changes in `src/dpl2/` and the authoritative spec unless scope expands.
