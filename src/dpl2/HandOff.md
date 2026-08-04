# HandOff — filler VT overlay repair

Updated: 2026-08-04. Branch: `claude/wizardly-carson-secahu`.

## Current result

`ImplantLayerChecker::check()` is the only opto entry. When the proposed
standard-cell master is illegal, the checker creates one `FillerRepairEngine`,
builds a committed-placement snapshot, asks the planner for same-size filler
master swaps, and appends a checker-verified `ipl::FillerChanges` result to the
caller's `fcRecord`. Neither checking nor repair commits UDM, Grid, or Network.

The current local suite contains 287 GoogleTests:

| Group | Count | Purpose |
|---|---:|---|
| planner | 93 | database-independent search, window, cache, budget, protocol |
| checker E2E | 83 | real `ImplantLayerChecker` plus helper-built input |
| fake-UDM chain | 111 | engine/infrastructure wiring and non-mutation |

All 287 tests compile with strict warnings. Normal, ASan and
migration-gate commands are listed below.

## Files to copy

Preferred minimal payload: copy the contents of
`src/dpl2/src/fillerRepair2/` into the destination's
`src/dpl2/src/fillerRepair/` directory. It contains the eight runtime/API
files and a small CMake target. Tests, fake UDM and repository-local runners do
not travel.

For development and verification, `src/dpl2/src/fillerRepair/` is the source
of truth. Its eight runtime files are byte-identical to `fillerRepair2/`; the
local CMake configure step hashes them and fails on drift.

If the destination checker/infrastructure does not already contain this
branch's boundary fixes, port the relevant changes listed in
`src/dpl2/src/drc/CHECKER_REPAIR_CONTRACT.md`. Do not replace the destination's
DRC rules, Grid, Network, or UDM implementation.

## Caller path

```cpp
std::vector<CellChangeRecord> filler_changes;
const bool legal = checker.check(node, x, y, orient, filler_changes);
if (legal) {
  // Opto commits its target change and filler_changes atomically.
}
```

There is no public planner, placement adapter, global precheck, update API, or
raw-UDM repair overload. The runtime API is deliberately small:

```cpp
FillerRepairEngine(Grid*, Network*);
bool init(PhysDesMgr*, const fillerSetting&);
RepairOutcome repair(const ipl::CheckRequest&);
```

The normal caller does not construct the engine. The checker does so only
after `checkDirect()` reports the candidate illegal. A fresh engine is used
for each failing request, which prevents a previous opto overlay or commit
from leaving a stale placement snapshot.

## Required setup

Before a failing checker call:

1. Grid and Network describe the same Design revision as `PhysDesMgr`.
2. Grid retains that manager through `Grid::getDesMgr()`.
3. `fillerSetting` has an active Design and a non-empty configured filler list.
4. Infrastructure has registered every configured filler master and every
   possible target replacement master in Network using its real edge table.
5. `DePlace` has registered the active setting provider, or a harness has
   called `setFillerRepairContext(desMgr, &setting)`.

`FillerRepairEngine::ensureMasterRegistered()` never calls
`Network::addMaster`. It only finds an existing configured master and executes
`setFiller(true)`. A missing configured master makes `init()` fail closed;
an absent or inconsistently indexed target master makes `repair()` fail closed.

Filler identity has one boundary:

- configured candidates come from `fillerSetting::getFillerPhysCells()`;
- engine initialization refreshes those existing Network Masters to filler;
- placed-instance identity comes from `Node::isFiller()`;
- UDM macro flags do not veto these decisions.

## CMake wiring

```cmake
add_library(dpl2_filler_repair_deps INTERFACE)
target_link_libraries(dpl2_filler_repair_deps
  INTERFACE <udm-targets> <dpl2-infra-and-checker-targets>)

add_subdirectory(<srcroot>/fillerRepair fillerRepair)
target_link_libraries(<owner> PRIVATE dpl2::fillerRepair)
```

The destination adds the directory and links the target; it does not enumerate
our `.cpp` files. All includes resolve from the existing dpl2 `src/` root.

## Behavioral contract

- Swap only: same instance, position, orientation, width and height; only the
  filler master changes.
- Regional placement gate only: legal Grid row segments must be covered exactly
  once. Blockages, padding/halo reservations and fragmented-row whitespace are
  excluded from required coverage.
- The checker's reach is authoritative. The engine uses
  `getMaxRuleValue() * siteWidth`, and the checker includes minimum value, PRL
  and LENGTH when calculating that reach.
- Sparse filler layouts are supported. Adaptive growth skips intervening
  standard cells, admits fillers within checker reach, and directly seeds any
  filler named by a blocking violation.
- Bounded failure is safe: budget or adaptive-level exhaustion returns no
  partial changes.
- `PlacementDRC` stages all checker records and publishes them only if every
  checker accepts the candidate.
- Debug output is off by default. Set `FR_VERBOSE=1` for `[fr][stage]` logs.

## Verification

Local fake-UDM chain:

```sh
ALL=1 src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address ALL=1 src/dpl2/test/local/run_fake_udm_e2e.sh
```

Copy-shaped migration gate:

```sh
src/dpl2/test/local/run_migration_gate.sh
SANITIZE=address src/dpl2/test/local/run_migration_gate.sh
```

Destination build:

```sh
cmake -S <srcroot>/fillerRepair -B build-fr \
  -DDPL2_FILLER_REPAIR_BUILD_TESTS=ON \
  -DDPL2_RUNTIME_LIBRARIES='<existing infra/checker targets>' \
  -DDPL2_UDM_INCLUDE_DIRS='<UDM include dirs>' \
  -DDPL2_UDM_LIBRARIES='<UDM targets/libraries>'
cmake --build build-fr
ctest --test-dir build-fr --output-on-failure
```

## Remaining integration checks

- The local branch still does not define the destination's complete
  `DePlace::initPlacementDRC()` registration. Confirm that the destination
  registers `ImplantLayerChecker` and reaches its five-argument `check()`.
- Confirm real `PhysObjStatus` values painted into Grid; an unpainted placed
  status appears as a placement gap.
- Confirm the destination's row iteration and Grid row ids agree. Mixed-height
  rows are supported when their heights are integer multiples of the smallest
  non-pad row height.
- Measure real checker latency before tuning batch size or budgets. Defaults
  change only bounded search effort, never acceptance correctness.
