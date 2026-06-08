# PG DRC (`check_drc -check_pg`) — Progress Log

Tracks the requirements in `docs/requirements_drc_pg.txt`. The section below is
the **baseline status as of the start of this log**; only *new* progress is
recorded under "Progress log" from here on.

## Baseline status (vs the 6 requirements)

| # | Requirement | Status | Evidence / gap |
|---|---|---|---|
| 1 | PG–PG spacing (VDD vs VSS, etc.) | ✅ done + tested | `checkMetalSpacing_prl`/`_short`; drc_test_pg & drc_test_pg_full row 1 (VDD vs VSS → Short + Metal Spacing) |
| 2 | Spacing/encroachment into blockage & keepout | ✅ done + tested | blockages loaded fixed → PG-vs-OBS `Short` + `Metal Spacing` (drc_test_pg); metal keepout via `Lef58EolKeepOut` (drc_test_pg_full) |
| 3 | Width / grid / track-pitch alignment | ◐ mostly | ✅ `Min Width` + ✅ `Off Grid` (= manufacturing grid), both tested. ❌ routing **track/pitch** alignment is not a separate standalone check |
| 4 | Via: cut size / enclosure / overlap | ◐ partial | ✅ `Cut Spacing` + ✅ `Minimum Cut` (tested). ❌ cut size min/max, **via enclosure**, via-metal overlap have **no** violation type in the GC engine (`MetalWidthViaMap` = via-table *selection* only, untested) |
| 5 | Boundary / ring & mesh perimeter | ❌ not done | GC engine has **no** boundary/perimeter DRC; an earlier prototype was reverted as out-of-scope |
| 6 | Layer-dependent / NDR / multi-pattern spacing | ◐ partial | ✅ layer/width-dependent `SPACINGTABLE`/PRL (tested as Metal Spacing). ⚠️ NDR handled in code but PG are special nets → not exercised. ❌ multi-patterning/SAMEMASK unsupported by the engine |

**Bottom line:** ~3.5 of 6 categories complete. The PG-vs-(PG/signal/blockage)
spacing + short + width + grid + area + cut + min-cut + layer-dependent surface
is done, with deterministic output, a per-layer/per-type summary table, and the
3-row (PG-PG / PG-signal / signal-signal-suppressed) tests. Extras beyond the
req: Min Area, Min Hole, and the reachable LEF58 EOL family (Lef58Area,
Lef58SpacingEndOfLine, Lef58EolExtension, Lef58EolKeepOut).

**Open gaps:**
1. #5 boundary / ring & mesh perimeter — entirely missing (needs a new subsystem). Biggest gap.
2. #4 via enclosure + cut-size min/max + via overlap — not violation types in the engine.
3. #3 routing track/pitch alignment — only manufacturing-grid off-grid is checked.
4. #6 NDR (for PG) and multi-patterning/SAMEMASK — NDR not exercised for PG; SAMEMASK unsupported.

## Progress log

_(new entries only, newest last)_

- _no entries yet_
