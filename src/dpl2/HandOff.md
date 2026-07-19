# HandOff — filler VT overlay repair

Updated: 2026-07-20. Branch: `claude/wizardly-carson-secahu`.

## Result

The destination already supplies complete infrastructure and checker sources.
Runtime and tests migrate together by copying only
`src/dpl2/src/fillerRepair/`. Its `test/` subtree contains 33 portable
GoogleTests built from the final checker's `ImplantLayerCheckerHelper` and a
standalone CMake. No DEF/LEF reader or destination fixture provider is needed.
All 82 fake-based planner unit tests and the repository-local UDM-compatible
harness are outside this payload at `src/dpl2/test/local/`.
No infrastructure/checker source or API change is required.

The runtime boundary is one checker-style class that reuses DePlace's
already initialized infrastructure:

```cpp
FillerRepairEngine(Grid* grid, Network* network);
void setDebugLogging(bool enabled);  // optional, disabled by default
bool init(PhysDesMgr* desMgr,
          const fillerSetting& fillerSetting);
ipl::CheckResult precheck() const;
RepairOutcome repair(LeafCellID targetCell, const PhysLibCell& newMaster);
```

The engine borrows Grid/Network and its `Impl` privately owns the final checker,
immutable planner snapshot and oracle calls. It does not import hierarchy cells or repaint a
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
its private checker/snapshot. The first implant query then overlays the target new
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
| cell/master topology and checker IDs | runtime Network |
| filler allow-list | `fillerSetting::getFillerMasters()` |
| VT family/band polarity | `PhysLibCell` implant shapes + checker layers |
| implant legality | final `ImplantLayerChecker` |
| commit | opto/infrastructure |

Placed masters come from the existing Network. Engine init idempotently
registers configured candidate masters before constructing its private checker.
Repair idempotently registers an uninstantiated target-new master and rebuilds
that checker/snapshot before querying it. These registry updates do not mutate UDM
placement.

## Migration to the destination environment

What to copy (one directory, nothing else):

1. `src/dpl2/src/fillerRepair/` -> next to the destination's existing
   `infrastructure/` and `drc/` directories (the runtime sources include
   `infrastructure/...` and `drc/ImplantLayerChecker.h` relative to that
   common source root). This directory contains the complete runtime
   delivery plus its portable final-checker `test/` package. It contains no local test double
   or UDM-compatible test-data implementation.

Runtime wiring (their CMake, 2 lines):

```cmake
include(<srcroot>/fillerRepair/sources.cmake)
target_sources(<owning-target> PRIVATE ${DPL2_FILLER_REPAIR_SOURCES})
```

`<owning-target>` is the target that already compiles Grid/Network/checker,
so its UDM include paths and libraries apply to our sources unchanged. The
only modifications that may be needed on their side:

- the common source root must be on the include path (theirs already is if
  `infrastructure/...`-style includes work today);
- C++17 or newer for the runtime sources (the harness builds them at 17
  for the planner and 20 for the chain).

The reused-infrastructure boundary requires the APIs already present in this
branch: `DePlace::getGrid()`, `getNetwork()`, `getDesMgr()` and idempotent
`Network::addMaster(const PhysLibCell&, const Grid*)`. No RepairInfrastructure,
leaf traversal or placement importer is copied into runtime.

Do not hand-copy file names -- both runtime and test CMake include the same
`sources.cmake`. Do not add `fillerRepair/test/*` to a runtime target.
The migrated E2E uses real Grid/Network/checker code and helper-built data.

Portable E2E wiring in the destination environment:

```sh
# Full runtime engine + planner + final checker/helper against destination:
cmake -S <srcroot>/fillerRepair/test -B build-e2e \
  -DDPL2_UDM_INCLUDE_DIRS='<real UDM include dirs>' \
  -DDPL2_RUNTIME_LIBRARIES='<existing infra/checker targets>' \
  -DDPL2_UDM_LIBRARIES='<real UDM libs/targets>'
cmake --build build-e2e
ctest --test-dir build-e2e --output-on-failure
```

The standalone target always consumes `${DPL2_FILLER_REPAIR_SOURCES}`, not only
the planner subset, so it compiles and links `FillerRepairEngine.cpp` against
the destination headers. `DPL2_RUNTIME_LIBRARIES` should name the existing
dpl2/checker owning targets; if omitted, the fallback compiles the adjacent
supplied infrastructure/checker sources.

## Build and verification

The CMake below `fillerRepair/test` is a portable final-checker test package,
not the destination's runtime owner. Its executable nevertheless compiles
the complete runtime engine source list, checker/helper and 33 portable
cases, which makes real boundary compilation part of the migration gate. The
separate local harness retains the 82 planner tests and 67 fake-UDM engine
cases for repository regression.

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

The migration payload has four direct checker overlay cases, 17
planner-to-final-checker cases and 12 internal exact-coverage precheck
cases. Each dense checker fixture contains eight rows and 200 sites. The
planner matrix covers all four rule classes, clean/empty repair, determinism,
batch invariance, candidates, third VT, budgets, baseline consistency,
same-size edits, new-violation avoidance, a two-swap solution and a
checker-legal three-swap repair when the best residual initially points toward
a blocked adaptive side. The internal precheck matrix covers gaps, overlaps,
clipping, legal holes, row ordering and deterministic coalescing. Planner doubles and
the fake-UDM suite live only under `src/dpl2/test/local/`; its 12 newly added
external instances call the real public `precheck()` API in opto order.

2026-07-20 verification result: planner 82/82, engine
fake-UDM E2E 67/67 and portable final-checker/precheck E2E 33/33 in both normal
and ASan builds; full normal and ASan CTest 182/182;
`-Wall -Wextra -Werror` clean. The
engine cases include three layouts proving that unused-layer persistent
checker diagnostics remain non-blocking while a used implant layer with a
missing rule makes initialization fail closed.

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
  runtime delivery.
- Destination verification still depends on its UDM include directories and
  link libraries/targets because Grid/Network headers use UDM types. Test data
  itself has no UDM/DEF/LEF dependency.
- Checker calls are serialized inside one engine because the checker const
  overlay path updates counters. The checker is private and cannot be
  accidentally shared across engines.
- The engine owns checker diagnostics translation; planner fake/checker types
  must remain outside runtime targets.
- `Types.h` remains a standalone bottom-level model header. Oracle-only
  `OracleRequest`/`OracleResult`/`OracleStatus` and `PlannerOracle` live in
  `OracleGate.h`; the public `RepairOutcome` remains in `FillerRepairEngine.h`.
  This avoids a second wire format and keeps Engine/Planner/Oracle dependencies
  one-way.
- `repair()` may idempotently add a previously uninstantiated target master to
  Network and rebuild its private checker/snapshot; it still never mutates UDM.
- Adaptive growth is still heuristic: it follows the best residual first and
  falls back to the opposite side only when that primary side adds nothing.
  Long irrelevant contiguous filler runs may therefore require several
  budgeted windows before the fallback is reached. Search failure remains
  atomic and returns no partial changes.
- A destination whose `Network::addMaster` overload has a different signature
  needs one mechanical change in the private `ensureMasterRegistered()` seam;
  no planner or checker change is involved.
