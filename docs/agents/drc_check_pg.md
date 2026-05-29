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

With `-check_pg`, the DRC engine only loads and checks **power/ground (PG)
objects**:

* it checks each PG object's own properties (min-area, min-width, min-step,
  etc.), and
* it checks the relationships **between PG objects** (PG-to-PG spacing,
  PG-to-PG short, …).

Signal, clock and every other non-PG *net* object is **completely excluded** —
it is never even loaded into a GC worker, so it can neither produce a violation
on its own nor interact with a PG shape to produce one.

**Blockages/obstructions/keepouts are an exception: they are kept.** They are
not signal/clock geometry — they are constraint objects that PG must respect.
Keeping them lets the engine report PG-to-blockage spacing/short violations
(otherwise that whole class would be silently dropped). They are loaded as
*fixed*, while PG shapes are loaded as *non-fixed* (see §2.1), so a PG shape
that overlaps or crowds an obstruction is flagged with an `obstruction:` source.

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
| `frcBlockage`/`frcInstBlockage`     | `false` (not PG) — but **kept as a constraint**    |
| anything else                       | `false` (not PG → excluded)                        |

This is the **hard part**: the filter has to recognise PG geometry across the
several different object types the region query returns, and treat "no net"
shapes as non-PG. The filter in `initDesign_skipObj()` keeps an object when it
is PG **or** a blockage/obstruction; only true non-PG net objects are dropped.

### 2.1 Fixed vs non-fixed in PG-only mode

The GC engine skips spacing/short checks when **both** shapes of a pair are
fixed (`FlexGC_main.cpp`). PG geometry comes from special nets, which are
loaded fixed by default — so two fixed PG straps that overlap would *not* be
flagged. To make `-check_pg` actually evaluate PG relationships,
`initDesign()` loads:

* **PG shapes as non-fixed** (`isFixed = false`) — they are the geometry under
  test, so PG-to-PG and PG-to-obstruction pairs are no longer "both fixed".
* **blockages/obstructions as fixed** — they are the constraint objects.

So `isFixed = DRC_CHECK_PG ? !isPGObj(obj) : true`. Normal mode is unchanged
(everything fixed).

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
  └─ region-query every fixed/DR object in the worker box
  └─ for each object: initDesign_skipObj(obj)
       └─ if DRC_CHECK_PG && !isPGObj(obj):
            [debug] "[filter] skip non-PG obj typeId=..."
            return true                  // object dropped, never checked
  └─ [debug] "[init] fixed objs read=R kept(PG)=K; dr objs read=.. kept(PG)=.."
```

The kept PG objects are the only geometry handed to `worker->main()`, so the
markers it produces can only involve PG shapes.

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
[DEBUG DRT-checkPG] [filter] skip non-PG obj typeId=frcInstTerm   # a signal pin
[DEBUG DRT-checkPG] [init] fixed objs read=15 kept(PG/obs)=11 (DR objs skipped)
[DEBUG DRT-checkPG] [filter] skip non-PG net net1
[DEBUG DRT-checkPG] [check_drc] done: 2 marker(s) reported (PG-only)
```

* `read` vs `kept(PG/obs)` shows exactly how many objects were seen by the
  region query and how many survived the filter (PG geometry + kept blockages).
* each `skip non-PG obj` / `skip non-PG net` line shows a single object/net
  being dropped, so you can see precisely what was excluded. Note that
  blockages do NOT appear in skip lines — they are intentionally kept.

## 5. Files touched

| File                                   | Change                                              |
| -------------------------------------- | --------------------------------------------------- |
| `src/drt/src/TritonRoute.tcl`          | `-check_pg` flag, pass to `check_drc_cmd`           |
| `src/drt/src/TritonRoute.i`            | `check_drc_cmd` takes `bool check_pg`               |
| `src/drt/include/triton_route/TritonRoute.h` | `checkDRC(..., bool check_pg=false)`          |
| `src/drt/src/TritonRoute.cpp`          | set/reset `DRC_CHECK_PG`, debug logic-chain logs    |
| `src/drt/src/global.h` / `global.cpp`  | `DRC_CHECK_PG` global (default false)               |
| `src/drt/src/serialization.h`          | serialize `DRC_CHECK_PG` for distributed workers    |
| `src/drt/src/gc/FlexGC_impl.h`         | declare `isPGObj`                                   |
| `src/drt/src/gc/FlexGC_init.cpp`       | `isPGObj`, PG filter (keeps blockages) in `initDesign_skipObj`/`initNetsFromDesign`, non-fixed PG load, debug |

## 6. Test

`src/drt/test/drc_test_pg.tcl` builds a tiny case (Nangate45) containing a
signal-to-signal short (net1/net2, metal1), a PG-to-PG short (VDD/VSS, metal2)
and a PG-to-obstruction short (VDD vs a metal2 OBS). It runs
`check_drc -check_pg` and diffs the report against
`src/drt/test/drc_test_pg.drcok`. The golden contains **only** the two PG
violations (VDD/VSS and VDD/obstruction), proving the signal violation was
filtered out while the obstruction constraint was honored.

Run it with the prebuilt binary:

```
./src/drt/test/regression drc_test_pg          # harness, gives pass/fail vs golden
# or, from inside src/drt/test/:
../../../build/src/openroad -no_init -no_splash -exit drc_test_pg.tcl
```

## 7. PG rule coverage (what `-check_pg` actually checks)

In a standalone `check_drc` the GC engine runs with `targetNet_==nullptr` and
no DR worker, and it skips spacing/short checks for pairs where **both** shapes
are fixed. Combined with the PG filter + non-fixed PG load, coverage is:

| Rule category | Covered? | Notes |
| ------------- | -------- | ----- |
| PG-to-PG spacing / short | ✅ | `checkMetalSpacing_prl` / `_short` (incl. PRL/TW spacing tables) |
| PG to blockage / keepout | ✅ | blockages kept (fixed) + PG non-fixed → `checkMetalSpacing_short_obs`; EOL keepout also applies |
| Layer-dependent spacing | ✅ | spacing tables via `checkMetalSpacing_prl` |
| NDR / metal multi-patterning | ⚠️ | PG special nets rarely carry NDR; metal SAMEMASK is unsupported by the engine |
| Min width / off-grid (track) | ✅ | `checkMetalShape_minWidth` / `_offGrid` (single-shape, skip fully-fixed) |
| Min area / min-enclosed-area | ❌ | gated by `targetNet_`, not run in standalone `check_drc` |
| Via cut spacing / short | ✅ | `checkCutSpacing*` (needs PG vias in the design) |
| Via enclosure / min-cut | ⚠️ | involves `targetNet_`; explicit cut-size min/max is not a dedicated check |
| Die/block boundary, ring/mesh perimeter | ❌ | the GC engine has no boundary DRC at all |
