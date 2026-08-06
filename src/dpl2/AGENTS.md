# AGENTS.md — dpl2 filler repair project memory

Updated: 2026-08-06. Branch: `claude/wizardly-carson-secahu`.

Current integration: DePlace binds its owned `fillerSetting` to Network. An
outer lifecycle owner constructs one checker and one engine, initializes the
engine with that checker, then binds the engine back to the checker before
starting worker threads. Neither object owns the other, and the engine never
constructs Network masters.

Read before changing this feature:

| Document | Answers |
|---|---|
| `docs/filler_vt_overlay_repair_spec.md` | what the feature must do |
| `src/dpl2/src/fillerRepair/README.md` | how the module is built and why |
| `src/dpl2/src/fillerRepair2/README.md` | minimal destination-only copy/link package |
| `src/dpl2/HandOff.md` | how to migrate it |
| `src/dpl2/src/drc/CHECKER_REPAIR_CONTRACT.md` | what we changed outside fillerRepair |
| `src/dpl2/src/fillerRepair/test/README.md` | what the portable tests cover |

## Status

Swap-only repair, complete and migration-ready.

| Area | State |
|---|---|
| Planner | `internal::RepairPlanner`: adaptive window, filler domains, per-band ranking, subset enumeration, baseline-delta oracle gate. Deterministic, non-mutating |
| Engine | `FillerRepairEngine` coordinates internal placement-snapshot, filler-catalog and checker-overlay components; it implements the two stable seams and borrows Grid/Network, `fillerSetting`'s Design, and the caller-owned checker |
| Checker | `ImplantLayerChecker(Grid*, Network*)` uses Grid's retained `PhysDesMgr`; repair is enabled by default, writes into caller's `fcRecord`, and borrows a pre-initialized engine. The test helper disables repair |
| Build | `fillerRepair/CMakeLists.txt` owns the full verification package; `fillerRepair2/CMakeLists.txt` owns only the C++20 runtime target. Both use `dpl2_filler_repair_deps` when supplied |
| Tests | 91 planner + 80 real-checker + 111 local fake-UDM engine cases; 282/282 local and 171/171 migration gate, normal and ASan |
| Mirror rule | `fillerRepair/` is the source of truth. Mirror every runtime/API change into `fillerRepair2/`; test CMake stages that copy under the destination name and strictly compiles both runtime sources |
| Retained infrastructure helper | `DePlace::registerFillerRepairMasters()` uses the real edge table and is called by the current `test_filler_repair` before checker construction |

## Fixed decisions

1. **Checker-as-oracle.** `ImplantLayerChecker` is the only DRC authority.
   Nothing in fillerRepair re-derives a rule, a reach, or a legality verdict.
2. **Swap only.** Same instance, position, orientation, width and height;
   different master. No move, no resize, no merge/split, no rewrite.
3. **Opto owns the records.** `check()` appends into the caller's
   `std::vector<CellChangeRecord>`; the checker keeps no member state, and
   commit belongs to opto/infrastructure. Repair never mutates UDM.
   Ordinary checker instances enable repair by default;
   `ImplantLayerCheckerHelper` explicitly disables it for checker-only tests.
4. **`fillerSetting::core_` is the only filler authority.**
   `fillerSetting::isFillerCell(LibCellID)` classifies registered Masters;
   Nodes inherit that stored Master type. Candidates come from the same list.
   No runtime path re-derives filler identity from UDM macro flags.
5. **Explicit design, no Session.** The caller supplies Design to the checker;
   the engine gets the same Design from `fillerSetting`. Design, passed
   `PhysDesMgr` and Grid manager must agree or initialization fails closed.
6. **Trust infrastructure.** RowId is the Grid row, x is core-left-relative.
   No Network↔UDM cross-validation. The owner completes registration and
   binding before checker/engine initialization; the request carries the
   proposed target master as a non-mutating overlay.
7. **Regional gate only.** Repair refuses to run on a gap/overlap in the rows
   it can edit. Whole-design placement legality is infrastructure's gate.
8. **Bounded, never wrong.** Budgets and level caps end a search as
   *truncated*, which is a bounded give-up — never a wrong acceptance.
9. **Two seams, no third abstraction.** `PlacementView` in, `RepairOracle`
   out. The lifecycle owner directly wires the checker and engine; do not add
   another runtime adapter.
10. **One immutable revision per parallel phase.** Initialize and bind before
    starting workers. Concurrent checks/repairs may share the pair, but Grid,
    Network, fillerSetting and UDM must not mutate. Stop workers and construct
    a new pair after a revision change.

## Change rules

- Keep changes inside `src/dpl2/`. Outside `fillerRepair/`, change only what
  is necessary, and tag it `[fillerRepair-fix]` with the reason.
- Record any checker-side edit in `CHECKER_REPAIR_CONTRACT.md`.
- Add or remove sources only via `fillerRepair/CMakeLists.txt` and its
  `test/` subdirectory — never by listing files in a consumer's build.
- Includes use angle brackets, resolved from the `src/` root.
- Changing planner algorithms requires a real-checker regression showing the
  defect, not a reading of the code.
- Run the full suite **and** the migration gate, normal and ASan, before
  committing. Performance claims need a before/after measurement, not an
  argument.
