# fillerRepair — filler VT overlay repair

Updated: 2026-07-27.

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
| `test/FillerRepairCheckerE2ETest.cpp` | 59 portable real-checker and planner-to-checker cases (band-polarity model: N bottom / P top band per master, MX on odd rows) |

## Debug transcript

`FR_VERBOSE=1` enables the deterministic `[fr][stage]` transcript on the
checker's lazily-created engine; planner tests use the same variable. Logging
never changes search order or acceptance.

## Verification

- portable planner: 82 cases; portable checker E2E: 59 cases (both compile,
  link and run in fake-UDM AND real-UDM harness modes — the migration gate).
- repository-local fake-UDM engine regression: 62 cases under
  `src/dpl2/test/local/`.
- 2026-07-27 full local suite: 203/203 normal and ASan; migration gate
  141/141.
