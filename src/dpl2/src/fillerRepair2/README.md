# fillerRepair2 migration payload

Updated: 2026-08-04.

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

The repository's 278-test local suite, 170-test migration gate and standalone
module build compile the full `fillerRepair/` directory. They do not compile or
compare this projection automatically. Build this directory against the
destination dependency target, or run an equivalent C++20 strict syntax check,
before copying it into place.

Runtime API:

```cpp
FillerRepairEngine(Grid*, Network*);
bool init(PhysDesMgr*, const fillerSetting&);
RepairOutcome repair(const ipl::CheckRequest&);
```

The destination checker constructor must be
`ImplantLayerChecker(Grid*, eUNL::Design*, Network*)`. The caller supplies
Design to the outer checker; engine initialization obtains that same non-owning
Design
from `fillerSetting`, verifies its manager against the explicit `PhysDesMgr`
and Grid manager, and passes it to the private checker. Neither path reads
Session.

Initialization replays every configured filler master through
`Network::addMaster(..., fillerSetting, ...)`, including masters already in
Network, so `Master::isFiller` is refreshed before the private checker is
constructed. The runtime remains one file but separates three private
responsibilities: placement snapshot, compatible filler catalog, and serialized
checker overlay calls. The catalog is built once using same width/height,
different known VT and matching bottom-band polarity. When no placed filler
has a catalog entry, the engine returns `NoCompatibleFillerCandidate` after the
baseline result and skips window/search work.

`ImplantLayerChecker::check()` remains the caller-facing entry. Repair is
non-mutating and returns the shared `ipl::FillerChanges`/`CellChangeRecord`
wire for infrastructure to commit.
