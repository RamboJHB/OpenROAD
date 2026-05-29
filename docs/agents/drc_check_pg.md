# `check_drc -check_pg` — PG-only DRC logic chain

This document describes the `-check_pg` option added to the `check_drc`
command in TritonRoute (the `drt` module), how it works, and the exact
logic chain you can follow in the debug log.

## 1. What the option does

```tcl
check_drc -output_file <file> [-box {x1 y1 x2 y2}] [-check_pg] \
          [-pg_boundary_margin <um>]
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
| `src/drt/src/TritonRoute.cpp` / `.h` / `.i` / `.tcl` | `-pg_boundary_margin`: PG-to-die-boundary check (the ring/mesh-perimeter rule) |
| `src/drt/src/frBaseTypes.h` / `db/tech/frConstraint.h` | new `frcPGBoundaryConstraint` + "PG Boundary Spacing" name |
| `src/drt/test/drc_test_pg*`             | toy (Nangate45) for spacing/short/width/off-grid/cut-spacing |
| `src/drt/test/drc_test_pg_adv*`         | companion toy (Nangate45 stack + injected `AREA`/`MINIMUMCUT`) for min-area & minimum-cut |

## 6. Test

`src/drt/test/drc_test_pg.tcl` (Nangate45) lays out spatially-separated
clusters so the golden exercises every rule the Nangate45 tech can express in
PG-only mode. `check_drc -check_pg -pg_boundary_margin 0.25` produces 10
markers, all PG-involving:

| Cluster | Marker | Source |
| ------- | ------ | ------ |
| PG-PG       | Short + Metal Spacing | VDD vs VSS (metal2) |
| PG-signal   | Short + Metal Spacing | VDD vs net1/net2 (metal2) |
| PG-blockage | Short + Metal Spacing | VDD vs metal2 OBS |
| PG width    | Min Width | narrow VDD strap |
| PG grid     | Off Grid | off-grid VDD strap |
| PG via      | Cut Spacing | two VDD via1 cuts too close |
| PG boundary | PG Boundary Spacing | VDD strap at the die edge |

The Metal Spacing markers use metal2's width-dependent `SPACINGTABLE` (PRL), so
they also cover layer/width-dependent spacing. The pure signal-to-signal short
(net1/net2 on metal1) is **not** reported — proving non-PG-vs-non-PG is
suppressed. Rules Nangate45 cannot express (Min Step, Min Area, Minimum Cut)
are covered by the companion `drc_test_pg_adv` test (custom tech LEF).

Run with the prebuilt binary:

```
./src/drt/test/regression drc_test_pg drc_test_pg_adv   # pass/fail vs golden
# or, from inside src/drt/test/:
../../../build/src/openroad -no_init -no_splash -exit drc_test_pg.tcl
```

## 7. PG rule coverage (what `-check_pg` actually checks)

In a standalone `check_drc` the GC engine runs with `targetNet_==nullptr` and
no DR worker, and it skips spacing/short checks for pairs where **both** shapes
are fixed. Combined with PG→non-fixed / non-PG→fixed loading, coverage is:

| Rule category | Covered? | Notes |
| ------------- | -------- | ----- |
| PG-to-PG spacing / short | ✅ | `checkMetalSpacing_prl` / `_short` (incl. PRL/TW spacing tables) |
| PG-to-signal spacing / short | ✅ | signal loaded fixed, PG non-fixed → reported with both net names |
| PG to blockage / keepout | ✅ | blockages loaded fixed + PG non-fixed → `checkMetalSpacing_short_obs`; EOL keepout also applies |
| Layer-dependent spacing | ✅ | width-dependent PRL `SPACINGTABLE` via `checkMetalSpacing_prl`; tested in `drc_test_pg` |
| Min width / off-grid (track) | ✅ | `checkMetalShape_minWidth` / `_offGrid` (single-shape, skip fully-fixed); tested in `drc_test_pg` |
| Min area | ✅ (PG-only) | `checkMetalShape_minArea` is ungated under `DRC_CHECK_PG` and emits a marker instead of a DR patch (see §2.2); tested in `drc_test_pg_adv` |
| Minimum cut | ✅ | `checkMinimumCut` runs standalone (else-branch); tested in `drc_test_pg_adv` |
| Via cut spacing / short | ✅ | `checkCutSpacing*`; tested in `drc_test_pg` (PG via1 cuts) |
| Via enclosure (metalWidthViaTable) | ⚠️ | `checkMetalWidthViaTable` runs standalone but needs a LEF58 `METALWIDTHVIATABLE` rule (absent in Nangate45) |
| NDR / metal multi-patterning | ⚠️ | PG special nets rarely carry NDR; metal SAMEMASK is unsupported by the engine |
| Die/block boundary, ring/mesh perimeter | ✅ (opt-in) | the GC engine has no boundary DRC, so this is a dedicated check added for `-check_pg`: `-pg_boundary_margin <um>` flags PG special-net shapes whose distance to the die boundary is below the margin (constraint `frPGBoundaryConstraint`, "PG Boundary Spacing"); tested in `drc_test_pg` |
| Min step | ⚠️ | the engine's `checkMetalShape_minStep` does not surface markers in a standalone `check_drc` (no targetNet_/DR context) even when the tech defines `MINSTEP`; not exercised |
