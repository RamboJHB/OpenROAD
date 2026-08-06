# fillerRepair2 migration payload

Updated: 2026-08-06.

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

## Synchronization and verification

`../fillerRepair/` is the source of truth; this directory is a hand-maintained
runtime-only projection, not generated output. Mirror every runtime algorithm,
API, shared-wire or diagnostic change here before migration.

Test CMake stages this directory under the destination `fillerRepair/` name
and compiles both runtime sources as C++20 with `-Wall -Wextra -Werror`. The
278-test local suite and 170-test migration gate exercise the full
source-of-truth directory.

Runtime API:

```cpp
FillerRepairEngine(Grid*, Network*);
bool init();
RepairOutcome repair(const ipl::CheckRequest&);
```

The destination checker constructor must be
`ImplantLayerChecker(Grid*, Network*)`. It obtains `PhysDesMgr` only from
Grid. DePlace binds its active `fillerSetting` to Network. Engine initialization
reads that binding, verifies its Design manager against Grid's manager, and
then constructs its private checker. Neither path reads Session.

Infrastructure must register every configured filler master with the real edge
table before checking. Initialization only looks up each existing Network
master and applies `Master::setFiller(true)` before the private checker is
constructed; the engine never creates a master. The runtime remains one file but separates three private
responsibilities: placement snapshot, compatible filler catalog, and serialized
checker overlay calls. The catalog is built once using same width/height,
different known VT and matching bottom-band polarity. When no placed filler
has a catalog entry, the engine returns `NoCompatibleFillerCandidate` after the
baseline result and skips window/search work.

`ImplantLayerChecker::check()` remains the caller-facing entry. Repair defaults
on for ordinary checker instances; `ImplantLayerCheckerHelper` switches it off.
Repair is non-mutating and returns the shared
`ipl::FillerChanges`/`CellChangeRecord` wire for infrastructure to commit.
