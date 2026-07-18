# HandOff — filler VT overlay repair

Updated: 2026-07-19. Branch: `claude/wizardly-carson-secahu`.

## Result

The destination already supplies complete infrastructure and checker sources.
Production and tests migrate together by copying only
`src/dpl2/src/fillerRepair/`. Its `test/` subtree contains only the 52
real-UDM production E2E assertions, fixture contract and standalone CMake.
All 81 fake-based planner unit tests and the repository-local UDM-compatible
harness are outside this payload at `src/dpl2/test/local/`.
No infrastructure/checker source or API change is required.

The production boundary is one checker-style class that reuses DePlace's
already initialized infrastructure:

```cpp
FillerRepairEngine(Grid* grid, Network* network);
void setDebugLogging(bool enabled);  // optional, disabled by default
bool init(PhysDesMgr* desMgr,
          const fillerSetting& fillerSetting);
ipl::CheckResult precheck() const;
RepairOutcome repair(LeafCellID targetCell, const PhysLibCell& newMaster);
```

The engine borrows Grid/Network and privately owns the final checker plus the
view/oracle/wire conversion. It does not import hierarchy cells or repaint a
second Grid. The pure search pipeline is
`internal::FillerRepairPlanner`. `init()` is one-shot and must succeed before
use: until it does, `precheck()` and `repair()` fail closed
(`precheck_not_initialized` / `engine_not_initialized`).

## Required opto sequence

1. Construct one `FillerRepairEngine` with `DePlace::getGrid()` and
   `DePlace::getNetwork()`, then call `init()` once with
   `DePlace::getDesMgr()` and the filler setting. No hierarchy leaf list,
   proposed target master or separate repair infrastructure/checker object is
   supplied.
2. Before any cell mutation, call `precheck()`.
3. If `precheck().isLegal == false`, stop. `Gap`/`Overlap` diagnostics are
   warnings for logging, but the bool is a hard blocking contract.
4. Call `repair(targetCell, newMaster)` while the DB still contains the old
   target master. `newMaster` is an overlay.
5. If repair succeeds, opto/infrastructure commits the returned
   `ipl::FillerChanges` together with its own target mutation.

fillerRepair provides the gate; it does not modify opto and does not commit.
`precheck()` and `repair()` are both non-mutating.

## Precheck scope

Precheck checks only placement gaps and overlaps inside coverage-required
legal row segments. The supplied Grid is the domain authority: maximal runs
whose pixels satisfy `is_valid && padding_reserved_by == nullptr` are checked
exactly once. Hard blockages, fragmented-row holes, halo/padding reservations
and other non-placeable legal whitespace are outside that domain and are not
reported as gaps. Cell coverage still comes from `PhysDesMgr` physical cells
and the Network cell universe. Precheck does not check:

- target or proposed master;
- master-size compatibility;
- candidate availability;
- Node/Master ID mapping;
- site alignment or a separate out-of-bounds category;
- implant DRC.

`isLegal=true` means no gap/overlap in those legal segments. `isLegal=false`
means at least one
`Gap`/`Overlap` diagnostic and opto must block. `repair()` deliberately does
not call precheck again.

## Repair semantics

If the target new master is not yet represented in Network (for example it is
uninstantiated), repair registers it with `Network::addMaster()` and rebuilds
its private checker/view. The first implant query then overlays the target new
master with an empty filler change list. No violation returns success with
empty changes. Violations enter
the unchanged adaptive-L1/ranker/subset/cache/budget/baseline-delta planner.
A clean solution returns `ipl::FillerChanges`; no solution returns failure and
empty changes. The final checker remains the only DRC oracle.

Optional debug logging is enabled with `engine.setDebugLogging(true)`. It emits
a deterministic `[fr][stage]` transcript for planner configuration,
normalization, window growth, swap generation, ranking, enumeration,
checker/cache/budget activity and the final decision. It is disabled by
default and does not affect search behavior.

## Data authority

| Data | Authority |
|---|---|
| rows and physical placement | `PhysDesMgr` |
| precheck coverage-required legal segments | supplied Grid valid, unreserved pixels |
| cell/master topology and checker IDs | production Network |
| filler allow-list | `fillerSetting::getFillerMasters()` |
| VT family/band polarity | `PhysLibCell` implant shapes + checker layers |
| implant legality | final `ImplantLayerChecker` |
| commit | opto/infrastructure |

Placed masters come from the existing Network. Engine init idempotently
registers configured candidate masters before constructing its private checker.
Repair idempotently registers an uninstantiated target-new master and rebuilds
that checker/view before querying it. These registry updates do not mutate UDM
placement.

## Migration to the destination environment

What to copy (one directory, nothing else):

1. `src/dpl2/src/fillerRepair/` -> next to the destination's existing
   `infrastructure/` and `drc/` directories (the production sources include
   `infrastructure/...` and `drc/ImplantLayerChecker.h` relative to that
   common source root). This directory contains the complete production
   delivery plus its real-UDM `test/` package. It contains no local test double
   or UDM-compatible test-data implementation.

Production wiring (their CMake, 2 lines):

```cmake
include(<srcroot>/fillerRepair/sources.cmake)
target_sources(<owning-target> PRIVATE ${DPL2_FILLER_REPAIR_PRODUCTION_SOURCES})
```

`<owning-target>` is the target that already compiles Grid/Network/checker,
so its UDM include paths and libraries apply to our sources unchanged. The
only modifications that may be needed on their side:

- the common source root must be on the include path (theirs already is if
  `infrastructure/...`-style includes work today);
- C++17 or newer for the production sources (the harness builds them at 17
  for the planner and 20 for the chain).

The reused-infrastructure boundary requires the APIs already present in this
branch: `DePlace::getGrid()`, `getNetwork()`, `getDesMgr()` and idempotent
`Network::addMaster(const PhysLibCell&, const Grid*)`. No RepairInfrastructure,
leaf traversal or placement importer is copied into production.

Do not hand-copy file names -- both production and test CMake include the same
`sources.cmake`. Do not add `fillerRepair/test/*` to a production target.
Every migrated E2E source is compiled against real UDM.

Real-UDM test wiring in the destination environment:

```sh
# Complete chain + all 52 assertion objects against REAL UDM:
cmake -S <srcroot>/fillerRepair/test -B build-real \
  -DDPL2_REAL_UDM_INCLUDE_DIRS='<real UDM include dirs>' \
  -DDPL2_REAL_UDM_LIBRARIES='<real UDM libs/targets>'
cmake --build build-real
```

To run those 52 cases, pass
`DPL2_REAL_UDM_PROVIDER_SOURCE=<RealUdmE2ETestProvider.cpp>`. The provider
implements only canonical fixture creation/loading and the operations declared
in `E2ETestProvider.h`; the shared source owns every engine call/assertion.
This provider is destination-specific because UDM design-construction/loading
APIs are not part of fillerRepair.

## Build and verification

The CMake below `fillerRepair/test` is a real-UDM test package, not production
CMake. It compiles the complete chain/E2E assertions against real UDM. The
separate local harness retains the 81 planner tests and UDM-compatible E2E data
provider only for repository verification.

Test dependencies: GoogleTest, Boost, TBB, C++20 and CMake 3.20+. Commands:

```sh
src/dpl2/test/local/run_planner_tests.sh
SANITIZE=address src/dpl2/test/local/run_planner_tests.sh
src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address src/dpl2/test/local/run_fake_udm_e2e.sh

cmake -S src/dpl2/test -B src/dpl2/test/build-cmake
cmake --build src/dpl2/test/build-cmake -j2
ctest --test-dir src/dpl2/test/build-cmake --output-on-failure
```

All 81 planner cases and 52 production E2E cases are GoogleTests. Planner cases
and doubles live only under `src/dpl2/test/local/planner`; `fillerRepair/test`
contains only real-UDM E2E sources, and `sources.cmake` exports only production
and real-UDM E2E lists. The local E2E harness reuses `e2e_cases.cpp`, so case
additions and changes are automatically synchronized.
Each behavior has three independently discovered cases; every case constructs
at least five standard rows. Coverage includes clean/gap/overlap precheck,
hard-blockage and instance-halo exclusions, a real gap inside the remaining
legal segment,
opto-blocking values, deterministic/non-mutating repair, persistent checker
diagnostics, candidate-universe failures and the row/column frame gates (trailing pad
accepted, leading pad and off-origin rows refused).

2026-07-19 split result: local planner 81/81 and production E2E through the
local UDM-compatible provider 52/52 in both normal and ASan builds; full
CTest 133/133; `-Wall -Wextra -Werror` clean.

## Integration risks

- Opto must honor the explicit precheck ordering; repair has no fallback gate.
- Grid/Network/PhysDesMgr must already describe the same revision and must
  outlive the borrowing engine. Init is one-shot; construct a new engine after
  commit.
- Supported design envelope (validated per node at init, Fatal otherwise):
  no pad row before a standard row, y-sorted row iteration, one shared row
  origin X equal to the core left edge, single contiguous span per row,
  orientations R0/R180/MX/MY. See CHECKER_REPAIR_CONTRACT.md "Row/column
  frames" for why (the checker mixes an iteration frame and a Grid frame).
- The supplied PhysDesMgr must be the UDM Session current design because the
  final checker constructor reads Session; init validates and fails closed on
  mismatch. The UDM design/library objects must outlive the engine.
- Destination build must consume `sources.cmake`; nothing else is part of the
  production delivery.
- Real-UDM verification still depends on the destination providing its UDM
  include directories and link libraries/targets; no code port remains.
- Checker calls are serialized inside one engine because the checker const
  overlay path updates counters. The checker is private and cannot be
  accidentally shared across engines.
- The public facade owns production translation; planner fake/checker types
  must remain outside production targets.
- `repair()` may idempotently add a previously uninstantiated target master to
  Network and rebuild its private checker/view; it still never mutates UDM.
- A destination whose `Network::addMaster` overload has a different signature
  needs one mechanical call-site adaptation in `FillerRepairEngine.cpp`; no
  planner or checker change is involved.
