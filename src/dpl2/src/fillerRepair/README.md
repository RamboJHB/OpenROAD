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
ahead of the pending UDM commit.

## Neighbourhood expansion (`expandByCellRing`)

A second public entry, independent of repair: DePlace hands in a `Rect` and
gets back the bounding rectangle of that region grown by N **cells** on the
left and right and N **rows** up and down (default 3).

```cpp
::Rect grown = engine.expandByCellRing(region);        // 3 rings
::Rect tight = engine.expandByCellRing(region, 1);     // 1 ring
```

Rings are counted in cells, not sites or DBU. Selection is by index into the
row's x-sorted instance list, so **a std cell is a ring member like any other
and never stops the walk** — the same `instancesInRing` primitive the repair
guard uses. Rows and columns clamp at the core edges, so the result never
leaves the placeable area, and the output always snaps outward to whole cells
and whole rows.

Coordinates are **core-relative DBU** — the frame `Grid::gridX(DbuX)` and
`Node::getLeft()` use. `init()` must have succeeded; otherwise the input is
returned unchanged (with a transcript line saying so).

**One filler authority.** `dpl2::isFillerMaster()` (infrastructure
`Objects.h`) is the single predicate: `Network::addNode` classifies nodes with
it, `Node::isFiller()` / `Master::isFiller()` report it, and fillerRepair asks
those two rather than re-deriving anything from UDM macro flags.
`fillerSetting` is a separate concept -- which filler masters may be *offered*
as replacements -- and stays in the engine's `filler_master_ids_` allow list.

**Includes** use angle brackets throughout, resolved from the `src/` root
(`<fillerRepair/RepairPlanner.h>`, `<infrastructure/Grid.h>`), matching the
delivered infrastructure/checker sources.

## Main files

The module is one runtime layer over one pure search pipeline, joined by two
explicit seams. The engine implements both seams; the planner depends on
nothing else, which is what keeps it database-free and portable.

```
                 FillerRepairEngine          (runtime: UDM/Grid/Network + checker)
                   implements v   ^ implements
        PlacementView (data in)   |   RepairOracle (legality out)
                              \   |   /
                            RepairPlanner     (pure deterministic search)
                                   |
                    RepairTypes + Debug       (leaf data model, logging)
```

| Path | Purpose |
|---|---|
| `FillerRepairEngine.h/.cpp` | runtime entry: snapshot over Grid/Network, regional coverage gate, owns the private checker and planner, implements both seams |
| `PlacementView.h` | **seam 1** — read-only placement view the planner queries (`MasterInfo`, `PlacedInstance`, candidate query, span/binary-search helpers) |
| `RepairOracle.h` | **seam 2** — legality oracle protocol (`OracleRequest/Result/Status`); not a second DRC checker |
| `RepairPlanner.h/.cpp` | the search pipeline in flow order: swap model → violation signatures → L0/adaptive window → ranking → subset enumeration → oracle gate → driver, plus `RepairConfig` |
| `RepairTypes.h` | leaf data model: ids, geometry, violations, diagnostics, entry/exit records. Depends on nothing in the module |
| `Debug.h` | `[fr][stage]` transcript (`cat`, `show`, `DebugLog`) |
| `sources.cmake` | source-of-truth lists for the runtime payload and portable tests |
| `test/RepairPlannerTest.cpp` + `TestPlacementView.h`, `TestRepairOracle.*`, `SyntheticMasterCatalog.*` | 82 portable database-free planner unit tests (the doubles implement the two seams) |
| `test/FillerRepairCheckerE2ETest.cpp` | 59 portable real-checker and planner-to-checker cases (see the fixture model below) |

## Debug transcript

The deterministic `[fr][stage]` transcript is **on by default**, so a
production run leaves a diagnosable trail without a rebuild or a rerun. Set
`FR_VERBOSE=0` to silence it (any other value, or unset, keeps it on); the
same variable governs the planner tests. Logging never changes search order
or acceptance.

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
- 2026-07-28 full local suite: 203/203 normal and ASan; migration gate
  141/141 normal and ASan.
