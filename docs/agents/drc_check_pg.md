# `check_drc -check_pg` — PG-only DRC logic chain

This document describes the `-check_pg` option added to the `check_drc`
command in TritonRoute (the `drt` module), how it works, and the exact
logic chain you can follow in the debug log.

## 1. What the option does

```tcl
check_drc -output_file <file> [-box {x1 y1 x2 y2}] [-check_pg]
```

Without `-check_pg`, `check_drc` behaves exactly as before: every object in
the design (signal, clock, power, ground, blockages, …) is loaded into the
geometry checker (GC) and every DRC violation is reported.

With `-check_pg`, the engine reports **only the DRC violations that involve a
power/ground (PG) object** — PG-to-PG, PG-to-signal, and PG-to-blockage —
while suppressing violations that involve no PG geometry (signal-to-signal,
signal-to-blockage, …).

This is achieved **not by deleting objects, but by how they are loaded**: PG
(supply) geometry is loaded **non-fixed** (the geometry "under test"), and
every other object — signal/clock pins and routing, blockages, obstructions —
is loaded **fixed** ("background"). The GC engine skips spacing/short checks
when *both* shapes of a pair are fixed, so:

* PG-vs-PG, PG-vs-signal, PG-vs-blockage pairs → one side is non-fixed → **checked**;
* signal-vs-signal, signal-vs-blockage pairs → both fixed → **suppressed**.

This matches how Cadence Innovus scopes a PG DRC: `verify_drc -check_only
special` reports special-net (PG) violations against the surrounding
geometry, e.g. *"Special Wire of Net X & Blockage of Cell Y"* and special-vs-
regular shorts — it does not make signal/blockages invisible, it checks PG
against them.

> Note: this means a PG strap that shorts or crowds a *signal* wire **is**
> reported (with both net names as the violation source). That is intentional
> — it is exactly the kind of PG violation a real PG DRC must catch.

The switch is carried by the global `DRC_CHECK_PG` (declared in
`src/drt/src/global.h`, defined in `src/drt/src/global.cpp`, default
`false`). It is set at the start of `checkDRC()` and reset to `false` at the
end so it never leaks into a subsequent normal run.

## 2. What "PG" means here

An object is considered PG iff it belongs to a **supply net**, i.e. a net (or
floating terminal) whose `odb::dbSigType` satisfies `isSupply()`
(`POWER` or `GROUND`). This is decided by
`FlexGCWorker::Impl::isPGObj()` in `src/drt/src/gc/FlexGC_init.cpp`, which
mirrors the owner-resolution switch used by `getNet()`:

| Object type                         | PG test                                            |
| ----------------------------------- | -------------------------------------------------- |
| `frcBTerm`                          | net's `getType().isSupply()`, else terminal's type |
| `frcInstTerm`                       | net's `getType().isSupply()`, else terminal's type |
| `frcPathSeg`/`frcVia`/`frcPatchWire`| owning `frNet`'s `getType().isSupply()`            |
| `drcPathSeg`/`drcVia`/`drcPatchWire`| backing `frNet`'s `getType().isSupply()`           |
| `frcBlockage`/`frcInstBlockage`     | `false` (background constraint, loaded fixed)      |
| anything else                       | `false` (background, loaded fixed)                 |

This is the **hard part**: `isPGObj()` has to recognise PG geometry across the
several different object types the region query returns, and treat "no net"
shapes (blockages) as non-PG. Note that **no object is dropped** — the PG vs
non-PG decision only controls the *fixed* flag (see §2.1).

### 2.1 Fixed vs non-fixed is the whole mechanism

The GC engine skips spacing/short checks when **both** shapes of a pair are
fixed (`FlexGC_main.cpp:422/603/752/...`). In PG-only mode we exploit that:

* **PG shapes → non-fixed** — the geometry under test
  (`initDesign()`: `isFixed = DRC_CHECK_PG ? !isPGObj(obj) : true`).
* **everything else → fixed** — signal/clock pins, blockages, obstructions are
  loaded as fixed background. Signal/clock *routing* is loaded through
  `initNetsFromDesign()` → `initRouteObj(obj, net, /*isFixed=*/true)`.

Result: every pair that involves a PG shape has a non-fixed side and is
checked; every pair with no PG shape is "both fixed" and skipped. Normal mode
is unchanged (everything fixed, full DRC). PG nets cannot be regular nets
(`DRT-0305`), so all routing in `initNetsFromDesign()` is non-PG and loaded
fixed; PG geometry only ever comes from special nets via `initDesign()`.

### 2.2 Min-area is reported (not patched) in PG mode

`checkMetalShape_minArea` (`FlexGC_main.cpp`) is normally gated to detailed
routing (`!targetNet_` → return) and *fixes* a min-area violation by adding a
patch (needs the DR worker). In PG-only mode there is no DR worker, so the
check is (a) allowed to run when `DRC_CHECK_PG`, (b) guarded so it never
dereferences a null `drWorker_`, and (c) made to **emit a marker** (the
`AreaConstraint`) instead of a patch. This is the only check that needed a
code change to be reportable; it stays inert during routing
(`DRC_CHECK_PG` is false there).

## 3. The logic chain (top to bottom)

```
check_drc (Tcl)                         src/drt/src/TritonRoute.tcl
  └─ sets $check_pg from the -check_pg flag
  └─ drt::check_drc_cmd ... $check_pg    src/drt/src/TritonRoute.i

TritonRoute::checkDRC(..., check_pg)     src/drt/src/TritonRoute.cpp
  └─ DRC_CHECK_PG = check_pg             // propagate switch to globals
  └─ [debug] "[check_drc] entry: mode=..."
  └─ initDesign(); initGuide()
  └─ [debug] "[check_drc] effective drc box=..."
  └─ getDRCMarkers(markers, box)         src/drt/src/TritonRoute.cpp
       └─ for each GC worker:
            worker->init(design)         // → FlexGCWorker::Impl::initDesign
            worker->main()               // run the DRC checks
  └─ [debug] "[check_drc] done: N marker(s) reported (PG-only)"
  └─ reportDRC(...)
  └─ DRC_CHECK_PG = false                // reset switch

FlexGCWorker::Impl::initDesign(design)   src/drt/src/gc/FlexGC_init.cpp
  └─ region-query every fixed object in the worker box (no object is dropped)
  └─ for each object:
       isFixed = DRC_CHECK_PG ? !isPGObj(obj) : true   // PG→non-fixed, else fixed
  └─ [debug] "[init] PG(non-fixed)=N, background-fixed(signal/obs)=M"

FlexGCWorker::Impl::initNetsFromDesign() src/drt/src/gc/FlexGC_init.cpp
  └─ for each design net: load its routing as fixed background
       initRouteObj(obj, net, /*isFixed=*/ DRC_CHECK_PG && !net->isSupply())
  └─ [debug] "[netinit] load non-PG net <name> as fixed background"
```

PG shapes are the only non-fixed geometry handed to `worker->main()`, so every
marker it produces has a PG shape on at least one side.

## 4. Reading the debug log

Enable the debug stream (tag `checkPG`, group `DRT`):

```tcl
set_debug_level DRT checkPG 1
check_drc -output_file out.drc -check_pg
```

Typical output (one `[init]` line per GC worker):

```
[DEBUG DRT-checkPG] [check_drc] entry: mode=PG-ONLY (-check_pg), box=(0,0)-(0,0)
[DEBUG DRT-checkPG] [check_drc] effective drc box=(0,0)-(20000,20000)
[DEBUG DRT-checkPG] [init] PG(non-fixed)=10, background-fixed(signal/obs)=5 (DR objs skipped)
[DEBUG DRT-checkPG] [netinit] load non-PG net net1 as fixed background
[DEBUG DRT-checkPG] [netinit] load non-PG net net2 as fixed background
[DEBUG DRT-checkPG] [check_drc] done: 3 marker(s) reported (PG-only)
```

* `PG(non-fixed)` vs `background-fixed` shows how the worker classified the
  geometry: PG shapes under test vs. everything else loaded as fixed backdrop.
* each `[netinit] load non-PG net <name> as fixed background` line shows a
  signal/clock net being loaded as a fixed obstacle (so PG-vs-that-net is
  checked but that-net-vs-another-signal is suppressed).
* `done: N marker(s)` is the count of PG-involving violations reported.

## 5. Files touched

| File                                   | Change                                              |
| -------------------------------------- | --------------------------------------------------- |
| `src/drt/src/TritonRoute.tcl`          | `-check_pg` flag, pass to `check_drc_cmd`           |
| `src/drt/src/TritonRoute.i`            | `check_drc_cmd` takes `bool check_pg`               |
| `src/drt/include/triton_route/TritonRoute.h` | `checkDRC(..., bool check_pg=false)`          |
| `src/drt/src/TritonRoute.cpp`          | set/reset `DRC_CHECK_PG`, debug logic-chain logs    |
| `src/drt/src/global.h` / `global.cpp`  | `DRC_CHECK_PG` global (default false)               |
| `src/drt/src/serialization.h`          | serialize `DRC_CHECK_PG` for distributed workers    |
| `src/drt/src/gc/FlexGC_impl.h`         | declare `isPGObj`; `initRouteObj(..., bool isFixed)` |
| `src/drt/src/gc/FlexGC_init.cpp`       | `isPGObj`; PG→non-fixed / non-PG→fixed classification in `initDesign`; non-PG routing loaded fixed in `initNetsFromDesign`; debug |
| `src/drt/src/gc/FlexGC_main.cpp`        | `checkMetalShape_minArea`: report a marker (not a patch) in PG mode; null-`drWorker_` guard (see §2.2) |
| `src/drt/test/drc_test_pg*`             | single toy (`drc_test_pg.lef` = Nangate45 stack + injected `AREA`/`MINSTEP`/`MINIMUMCUT`) covering all PG rules |

## 6. Test

`src/drt/test/drc_test_pg.tcl` lays out spatially-separated clusters so the
golden exercises every rule expressible in PG-only mode. It uses
`drc_test_pg.lef` (the Nangate45 stack with `AREA`/`MINSTEP`/`MINIMUMCUT`
injected into metal2, which stock Nangate45 lacks). `check_drc -check_pg`
produces 11 markers, all PG-involving:

| Cluster | Marker | Source |
| ------- | ------ | ------ |
| PG-PG       | Short + Metal Spacing | VDD vs VSS (metal2) |
| PG-signal   | Short + Metal Spacing | VDD vs net1/net2 (metal2) |
| PG-blockage | Short + Metal Spacing | VDD vs metal2 OBS |
| PG width    | Min Width | narrow VDD strap |
| PG grid     | Off Grid | off-grid VDD strap |
| PG via      | Cut Spacing | two VDD via1 cuts too close |
| PG area     | Min Area | small VDD metal2 shape |
| PG min-cut  | Minimum Cut | wide VDD strap, single via1 cut |

The Metal Spacing markers use metal2's width-dependent `SPACINGTABLE` (PRL), so
they also cover layer/width-dependent spacing. The pure signal-to-signal short
(net1/net2 on metal1) is **not** reported — proving non-PG-vs-non-PG is
suppressed. Min Step is not surfaced by standalone `check_drc` and the GC engine
has no boundary check; both are out of scope (see §7).

Run with the prebuilt binary:

```
./src/drt/test/regression drc_test_pg            # pass/fail vs golden
# or, from inside src/drt/test/:
../../../build/src/openroad -no_init -no_splash -exit drc_test_pg.tcl
```

## 7. PG rule coverage (what `-check_pg` actually checks)

In a standalone `check_drc` the GC worker runs its **full** check sequence with
`targetNet_ == nullptr` (no DR worker), and skips spacing/short checks for pairs
where **both** shapes are fixed. Combined with PG→non-fixed / non-PG→fixed
loading, every check below runs; only violations that involve a (non-fixed) PG
shape are reported.

Worker check sequence (`FlexGCWorker::Impl::main`):
`checkMetalCornerSpacing → checkMetalSpacing → checkMetalShape →
checkMetalEndOfLine → checkCutSpacing → checkMetalSpacingTableInfluence →
checkMinimumCut → checkMetalWidthViaTable`. Each has a standalone (non-
`targetNet_`) branch, so all run under `check_drc`.

Legend — **check_drc?**: ✅ runs in standalone `check_drc`; ⚠️ runs but only
fires when the tech defines the matching LEF/LEF58 rule (absent in Nangate45);
❌ detailed-routing only (gated by `targetNet_`). **PG?**: exercised by
`drc_test_pg` (`-check_pg`).

| Violation (`getViolName`) | What it checks | Check fn | check_drc? | PG? |
| --- | --- | --- | --- | --- |
| `Short` | metal overlap / short | `checkMetalSpacing_short` | ✅ | ✅ |
| `Metal Spacing` | min spacing incl. PRL `SPACINGTABLE` | `checkMetalSpacing_prl` | ✅ | ✅ |
| `NS Metal` | non-sufficient metal (notch) overlap | `checkMetalSpacing` | ✅ | ❌ |
| `SpacingTable`, `SpacingTableTw` | table / two-width spacing | `checkMetalSpacing` | ✅ | ❌ (toy PRL reports as `Metal Spacing`) |
| `MetSpacingInf` | spacing-table influence | `checkMetalSpacingTableInfluence` | ✅ | ❌ |
| `EOL Spacing`, `SpacingEOLParallelEdge` | end-of-line spacing | `checkMetalEndOfLine` | ✅ | ❌ |
| `Min Width` | min metal width | `checkMetalShape_minWidth` (skips fully-fixed) | ✅ | ✅ |
| `Off Grid` | off manufacturing grid | `checkMetalShape_offGrid` | ✅ | ✅ |
| `Min Area` | min metal area | `checkMetalShape_minArea` | ✅ **only under `-check_pg`** (emits marker, not patch; §2.2) | ✅ |
| `Min Step` | min step / jog | `checkMetalShape_minStep` | ✅ (runs, but needs jog geometry — not exercised) | ❌ |
| `Min Hole` | min enclosed (hole) area | `checkMetalShape_minEnclosedArea` | ✅ | ❌ |
| `Rect Only`, `RightWayOnGridOnly` | LEF58 RECTONLY / wrong-way on-grid | `checkMetalShape` | ⚠️ | ❌ |
| `Corner Spacing` | convex-corner spacing | `checkMetalCornerSpacing` | ✅ | ❌ |
| `Cut Spacing` | cut-to-cut spacing | `checkCutSpacing_spc` | ✅ | ✅ |
| `Short` on CUT (shown `Cut Short`) | cut overlap short | `checkCutSpacing_short` | ✅ | ❌ |
| `Minimum Cut` | min #cuts on wide metal | `checkMinimumCut_main` | ✅ | ✅ |
| `MetalWidthViaMap` | LEF58 `METALWIDTHVIATABLE` | `checkMetalWidthViaTable` | ⚠️ | ❌ |
| `Lef58SpacingEndOfLine` (+`Within`/`EndToEnd`/`EncloseCut`/`ParallelEdge`/`MaxMinLength`), `Lef58EolKeepOut`, `Lef58EolExtension` | LEF58 EOL family | `checkMetalEndOfLine` | ⚠️ | ❌ |
| `Lef58CornerSpacingConcaveCorner`/`ConvexCorner`/`Spacing`/`Spacing1D`/`Spacing2D` | LEF58 corner family | `checkMetalCornerSpacing` | ⚠️ | ❌ |
| `Lef58CutSpacingTable`/`TablePrl`/`TableLayer`/`ParallelWithin`/`AdjacentCuts`/`Layer`, `Lef58CutClass` | LEF58 cut family | `checkLef58CutSpacing` | ⚠️ | ❌ |
| `Lef58SpacingTable` | LEF58 metal spacing table | `checkMetalSpacing` | ⚠️ | ❌ |
| `Lef58Area` | LEF58 min area | `checkMetalShape_lef58Area` | ❌ **DR-only** (not enabled for `-check_pg`, unlike `Min Area`) | ❌ |
| `Recheck` | internal "needs recheck" flag (not a DRC rule) | — | — | — |
| Die/block boundary, ring/mesh perimeter | — | none (no such check in the GC engine) | ❌ | ❌ (out of scope) |

`drc_test_pg` (`-check_pg`) produces exactly seven types: `Short`,
`Metal Spacing`, `Min Width`, `Off Grid`, `Min Area`, `Cut Spacing`,
`Minimum Cut`.

Notable code facts:
* `Min Area` is the only check specially enabled for PG mode — `DRC_CHECK_PG`
  ungates it and makes it emit a marker instead of a DR patch (§2.2).
* `Lef58Area` is **not** enabled for `-check_pg` (still `targetNet_`-gated), so
  LEF58 area violations are not reported by `check_drc`; mirroring the
  `Min Area` change would close that gap.
* `Min Step` runs in `check_drc` but the engine only emits it for specific jog
  geometry, so the toy does not exercise it.
* NDR / metal multi-patterning (SAMEMASK) is not a separate violation type here
  and is not surfaced by `-check_pg`.
