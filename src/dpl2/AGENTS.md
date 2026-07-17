# AGENTS.md — dpl2 filler repair project memory

Updated: 2026-07-18. Branch: `claude/wizardly-carson-secahu`.

Read `docs/filler_vt_overlay_repair_spec.md`, `src/dpl2/HandOff.md`,
`src/dpl2/src/drc/CHECKER_REPAIR_CONTRACT.md` and the adapter/test READMEs
before changing this feature.

## Project status

The V2.1 swap-only planner and fake-UDM-only production E2E are complete.

| Area | State |
|---|---|
| Planner | OracleGate fixes, adaptive-L1, filler domains, per-band ranking/filtering and deterministic output complete |
| Unit tests | 87/87 normal and ASan |
| Infrastructure | `RepairInfrastructure` builds production Network/Grid from PhysDesMgr data |
| Checker | final blocking contract, Node/Master IDs and FillerCellRecord wire |
| Adapter | one `adapter::PlacementView` = view + direct oracle + repair entry |
| E2E | fake UDM is the only data substitute; normal/ASan and `-Werror` pass |
| CMake | standalone CMake/CTest target passes normal and ASan |

The old list-only/raw contract, three-object adapter, Helper-injected Grid,
Grid link stubs, fake PlacementDRC and test DePlace/Network shims are retired.

## Fixed decisions

1. The planner is deterministic, non-mutating and UDM-free.
2. This stage supports same-position/same-size filler swaps only.
3. `ImplantLayerChecker` is the only DRC oracle.
4. `adapter::PlacementView` is the only production planner boundary.
5. Placement/precheck data comes from `PhysDesMgr`.
6. Candidates come only from `fillerSetting::getFillerMasters()`.
7. Instance/master IDs are `Node::getId()` / `Master::getId()`; physical wire
   handles are `LeafCellID` / `LibCellID` in `FillerCellRecord`.
8. All placed, target-new and configured filler masters are registered before
   checker construction, including uninstantiated masters.
9. Checker batches use one target/guard plus ordered `FillerChanges`; checker
   computes the empty-overlay baseline and returns blocking violations.
10. Rebuild infrastructure/checker/adapter after a design commit.

## Production infrastructure boundary

`RepairInfrastructure::build(desMgr, leafCellIds, fillerSetting,
targetNewMaster)`:

- validates `fillerSetting` and `PhysDesMgr` belong to the same design;
- derives core/rows and real Grid state from `PhysDesMgr`;
- creates deterministic Network master IDs in LibCellID order;
- imports Nodes by reading each supplied LeafCellID back through PhysDesMgr;
- includes uninstantiated target/candidate masters;
- paints production Grid occupancy.

The caller owns hierarchy traversal and supplies leaf IDs. No placement
properties are accepted from the caller.

## Thread/lifetime model

The infrastructure, checker and adapter form one immutable design snapshot.
Use one engine per thread. Adapter coverage is protected by `call_once` and
checker calls are serialized. `RepairInfrastructure` is intentionally
one-build; create another object for a new revision.

## Tests

```sh
src/dpl2/src/fillerRepair/test/run_tests.sh
SANITIZE=address src/dpl2/src/fillerRepair/test/run_tests.sh
src/dpl2/test/build_all.sh
SANITIZE=address src/dpl2/test/build_all.sh
```

The final E2E source list contains production Grid/Network/importer/checker/
adapter/planner and fake UDM headers only. Private test doubles remain confined
to the standalone 87-case planner unit executable for fault injection; never
link them into E2E or production targets.

Local dependencies are Boost, TBB, C++20 and CMake. On Apple ASan, use the
static Homebrew TBB archive as encoded in both build entry points.

## Change rules

- Keep planner algorithms unchanged unless a production-chain regression
  demonstrates a planner defect.
- Checker algorithm changes remain checker-RD-owned. Record any checker source
  compatibility edit in `CHECKER_REPAIR_CONTRACT.md`.
- Do not restore deleted fakes/stubs or add another production abstraction.
- Run planner and E2E normal + ASan before commit.
- Keep changes in `src/dpl2/` and the authoritative spec unless scope expands.
