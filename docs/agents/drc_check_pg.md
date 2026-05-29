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

## 6. Test

`src/drt/test/drc_test_pg.tcl` builds a tiny case (Nangate45) containing a
signal-to-signal short (net1/net2, metal1), a PG-to-signal short (net1/VDD,
metal2), a PG-to-PG short (VDD/VSS, metal2) and a PG-to-obstruction short (VDD
vs a metal2 OBS). It runs `check_drc -check_pg` and diffs the report against
`src/drt/test/drc_test_pg.drcok`. The golden contains the **three PG-involving
violations** (net1/VDD, VDD/VSS, VDD/obstruction) and **not** the pure
signal-to-signal short — proving signal-vs-signal is suppressed while every
PG interaction (including PG-to-signal) is reported.

Run it with the prebuilt binary:

```
./src/drt/test/regression drc_test_pg          # harness, gives pass/fail vs golden
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
| Layer-dependent spacing | ✅ | spacing tables via `checkMetalSpacing_prl` |
| NDR / metal multi-patterning | ⚠️ | PG special nets rarely carry NDR; metal SAMEMASK is unsupported by the engine |
| Min width / off-grid (track) | ✅ | `checkMetalShape_minWidth` / `_offGrid` (single-shape, skip fully-fixed) |
| Min area / min-enclosed-area | ❌ | gated by `targetNet_`, not run in standalone `check_drc` |
| Via cut spacing / short | ✅ | `checkCutSpacing*` (needs PG vias in the design) |
| Via enclosure / min-cut | ⚠️ | involves `targetNet_`; explicit cut-size min/max is not a dedicated check |
| Die/block boundary, ring/mesh perimeter | ❌ | the GC engine has no boundary DRC at all |
