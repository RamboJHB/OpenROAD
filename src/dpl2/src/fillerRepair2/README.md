# fillerRepair2 migration payload

This directory contains only runtime code used by the destination: the
checker-owned engine, planner, two planner seams, shared types, logging, and a
minimal CMake target. It intentionally contains no tests, fake UDM, standalone
dependency discovery, test-only engine entry points, or migration audit tags.

Copy the **contents** of this directory to the destination's existing
`src/dpl2/src/fillerRepair/` directory. The source keeps the established
`<fillerRepair/...>` include path and `dpl2::fillerRepair` namespace, so the
checker hook needs no source change.

The destination CMake only needs:

```cmake
add_subdirectory(src/dpl2/src/fillerRepair)
target_link_libraries(<dpl2-owner> PRIVATE dpl2::fillerRepair)
```

If the engine is compiled outside the destination's existing dependency
scope, define an interface target named `dpl2_filler_repair_deps` before
`add_subdirectory()` and attach the existing UDM, infrastructure, and checker
include/link dependencies to it.

Runtime API:

```cpp
FillerRepairEngine(Grid*, Network*);
bool init(PhysDesMgr*, const fillerSetting&);
RepairOutcome repair(const ipl::CheckRequest&);
```

`ImplantLayerChecker::check()` remains the caller-facing entry. Repair is
non-mutating and returns the shared `ipl::FillerChanges`/`CellChangeRecord`
wire for infrastructure to commit.
