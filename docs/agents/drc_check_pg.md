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

Signal, clock and every other non-PG object is **completely excluded** — it
is never even loaded into a GC worker, so it can neither produce a violation
on its own nor interact with a PG shape to produce one.

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
| `frcBlockage`/`frcInstBlockage`/…   | `false` (not on a PG net → excluded)               |

This is the **hard part**: the filter has to recognise PG geometry across the
several different object types the region query returns, and treat
"no net" / blockages as non-PG.

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
[DEBUG DRT-checkPG] [filter] skip non-PG obj typeId=27   # a signal pathSeg
[DEBUG DRT-checkPG] [init] fixed objs read=42 kept(PG)=6; dr objs read=0 kept(PG)=0
[DEBUG DRT-checkPG] [check_drc] done: 1 marker(s) reported (PG-only)
```

* `read` vs `kept(PG)` shows exactly how many objects were seen by the region
  query and how many survived the PG filter.
* each `skip non-PG obj` line shows a single object being dropped (with its
  `frBlockObject::typeId()`), so you can see precisely what was excluded.

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
| `src/drt/src/gc/FlexGC_init.cpp`       | `isPGObj`, PG filter in `initDesign_skipObj`, debug |

## 6. Test

`src/drt/test/drc_test_pg.tcl` builds a tiny case (Nangate45) containing both
a signal-to-signal short and a PG-to-PG (VDD/VSS) short, runs
`check_drc -check_pg`, and diffs the report against
`src/drt/test/drc_test_pg.drcok`. The golden contains **only** the PG
violation, proving the signal violation was filtered out.
