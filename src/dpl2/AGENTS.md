# AGENTS.md — dpl2 filler repair project memory

Updated: 2026-07-28. Branch: `claude/wizardly-carson-secahu`.

Read before changing this feature:

| Document | Answers |
|---|---|
| `docs/filler_vt_overlay_repair_spec.md` | what the feature must do |
| `src/dpl2/src/fillerRepair/README.md` | how the module is built and why |
| `src/dpl2/HandOff.md` | how to migrate it |
| `src/dpl2/src/drc/CHECKER_REPAIR_CONTRACT.md` | what we changed outside fillerRepair |
| `src/dpl2/src/fillerRepair/test/README.md` | what the portable tests cover |

## Status

Swap-only repair, complete and migration-ready.

| Area | State |
|---|---|
| Planner | `internal::RepairPlanner`: adaptive window, filler domains, per-band ranking, subset enumeration, baseline-delta oracle gate. Deterministic, non-mutating |
| Engine | `FillerRepairEngine` implements both seams (`PlacementView`, `RepairOracle`), owns a private oracle checker, borrows Grid/Network |
| Checker | repair wiring in `check()` writes into the caller's `fcRecord`; engine built lazily on first failing check |
| Build | `fillerRepair/CMakeLists.txt` owns `dpl2::fillerRepair` (C++20) and `dpl2::fillerRepairPlanner` (C++17); externals arrive through the single `dpl2_filler_repair_deps` interface target |
| Tests | 85 portable planner + 76 portable real-checker + 95 local fake-UDM engine cases; 256/256 local and 161/161 migration gate, normal and ASan |

## Fixed decisions

1. **Checker-as-oracle.** `ImplantLayerChecker` is the only DRC authority.
   Nothing in fillerRepair re-derives a rule, a reach, or a legality verdict.
2. **Swap only.** Same instance, position, orientation, width and height;
   different master. No move, no resize, no merge/split, no rewrite.
3. **Opto owns the records.** `check()` appends into the caller's
   `std::vector<CellChangeRecord>`; the checker keeps no member state, and
   commit belongs to opto/infrastructure. Repair never mutates UDM.
4. **`fillerSetting::core_` is the only filler authority.**
   `fillerSetting::isFiller(LibCellID)` classifies registered Masters;
   Nodes inherit that stored Master type. Candidates come from the same list.
   No production path re-derives filler identity from UDM macro flags.
5. **Trust infrastructure.** RowId is the Grid row, x is core-left-relative.
   No Network↔UDM cross-validation: with lazy init the engine typically runs
   mid-check, while the candidate Node already carries its proposed master.
6. **Regional gate only.** Repair refuses to run on a gap/overlap in the rows
   it can edit. Whole-design placement legality is infrastructure's gate.
7. **Bounded, never wrong.** Budgets and level caps end a search as
   *truncated*, which is a bounded give-up — never a wrong acceptance.
8. **Two seams, no third abstraction.** `PlacementView` in, `RepairOracle`
   out. Do not add another runtime layer beside the checker-owned engine.

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
