# fillerRepair — filler VT overlay repair

Updated: 2026-07-28.

`ImplantLayerChecker::check()` is the caller-facing entry. Opto owns the
`FillerCellRecord` vector; the checker appends checker-verified repair swaps
into that reference and keeps **no** filler-change member state. The
`FillerRepairEngine` is created **lazily** on the first DRC-illegal check —
a run whose checks all pass never pays engine initialization.

```cpp
// production (set_filler_option ran; DePlace registered the setting provider)
std::vector<FillerCellRecord> fcRecord;
bool legal = deplace->isLegal(cellId, lcId, fcRecord);   // -> checker.check(...)
if (legal && !fcRecord.empty()) {
  commitFillerSwaps(fcRecord);   // commit stays with opto/infrastructure
}

// harnesses without a DePlace owner preset the lazy context instead:
checker.setFillerRepairContext(desMgr, &fillerSetting);
```

The engine's only placement gate is **regional**: repair refuses to run on a
gap/overlap inside the rows it can edit (legal spans derived from Grid pixels
lazily, per row). Whole-design placement legality is infrastructure's own
gate — the engine has no global precheck.

Infrastructure data is **trusted as-is**: RowId is the Grid row, x is
core-left-relative (the same frame the checker's `CheckRequest` uses), and the
engine performs no Network↔UDM cross-validation — with lazy init it typically
runs mid-check, while the candidate Node already carries its proposed master
ahead of the pending UDM commit. UDM master type is the filler authority
(Network and checker agree by construction); `fillerSetting` remains the
replacement-candidate allow list.

## Main files

| Path | Purpose |
|---|---|
| `FillerRepairEngine.h/.cpp` | runtime engine: snapshot over Grid/Network, regional coverage gate, oracle/planner ownership, repair entries |
| `FillerRepairPlanner.h/.cpp` | the complete deterministic search pipeline: swap model, violation signatures, L0/adaptive window, ranking, subset enumeration, oracle gate, driver |
| `PlannerDataSource.h` | header-only read-only view contract + shared binary-search helpers |
| `Types.h` | leaf types: geometry, diagnostics, violations, planner entry records, debug log |
| `sources.cmake` | source-of-truth lists for the runtime payload and portable tests |
| `test/FillerRepairPlannerTest.cpp` + doubles | 82 portable database-free planner unit tests |
| `test/FillerRepairCheckerE2ETest.cpp` | 59 portable real-checker and planner-to-checker cases (see the fixture model below) |

## Debug transcript

`FR_VERBOSE=1` enables the deterministic `[fr][stage]` transcript on the
checker's lazily-created engine; planner tests use the same variable. Logging
never changes search order or acceptance.

## Portable fixture model

The portable checker fixtures encode four invariants of the current checker;
breaking any of them silently changes what the cases test:

- **Rule and layer ids ARE indices** into `ImplantInput::rules` / `layers`.
  The checker resolves them as `rules_[id]` / `layers_[id]` (its own builders
  assign the container size), so semantic numbering indexes out of bounds.
- **Band polarity**: each VT family has an N layer (bottom band in R0) and a
  P partner (top band), `basePolar = N`, and odd rows are placed MX. Inter-row
  expectations pick the N or P rule by boundary parity (`interRule`).
- **Rule reach sizes the snapshot**: `getSnapshot` spans
  `colId +/- maxRuleValue_` sites, where
  `maxRuleValue_ = ceil(max rule minValue / siteWidth)`. A neighbour further
  out is never in `shapes`, so the spacing scenarios keep their neighbour run
  starting within that window, with a single editable bridge filler forming
  the sub-minimum gap.
- **Min width still applies** to every run, so a scenario's runs must stay at
  or above `MIN_RULE` while the gap between them stays below it.

## Verification

- portable planner: 82 cases; portable checker E2E: 59 cases (both compile,
  link and run in fake-UDM AND real-UDM harness modes — the migration gate).
- repository-local fake-UDM engine regression: 62 cases under
  `src/dpl2/test/local/`.
- 2026-07-27 full local suite: 203/203 normal and ASan; migration gate
  141/141.
