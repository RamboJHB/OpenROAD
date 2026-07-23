# HandOff — filler VT overlay repair

Updated: 2026-07-22. Branch: `claude/wizardly-carson-secahu`.

## Result

The destination already supplies complete infrastructure and checker sources.
Runtime integration uses `src/dpl2/src/fillerRepair/`, the infrastructure
`Network::updateNodes()` refresh seam, and the small wiring now placed in
`ImplantLayerChecker::check()`. Its `test/` subtree contains 153
portable GoogleTests: 82 database-free planner cases and 71 final-checker/
precheck E2E cases. No DEF/LEF reader, fake UDM tree or destination fixture
provider is needed. The repository-local fake-UDM checker/engine harness remains
outside this payload at `src/dpl2/test/local/`.
Checker DRC rules and scan behavior are unchanged.

The portable E2E matrix now controls filler density inside each target-local
repair neighborhood, not only across the whole synthetic design. All four
width/spacing classes run at 50:50, 30:70, 20:80, 10:90 and 5:95. See
`docs/filler_repair_dense_placement_analysis.md` for the observed limitation,
fast no-solution proposal and future filler-span rewrite boundary. That note is
future design guidance; production remains V2.1 swap-only.

The caller boundary is the existing checker. It owns the engine, so opto does
not construct a second repair object:

```cpp
ImplantLayerChecker(Grid* grid, Network* network);
bool initFillerRepair(PhysDesMgr* desMgr,
                      const fillerSetting& fillerSetting);
void setFillerRepairDebugLogging(bool enabled);  // optional
ipl::CheckResult precheckFillerRepair() const;
bool check(const Node* node, GridX x, GridY y,
           const PhysOrientation& orient) const;
const ipl::FillerChanges& getFillerChanges() const;
const ipl::DiagVec& getFillerRepairDiagnostics() const;
bool updateFillerRepair(PhysDesMgr* desMgr,
                        const fillerSetting& fillerSetting);
```

The checker-owned engine borrows Grid/Network and its `Impl` privately owns an
oracle checker, immutable planner snapshot and oracle calls. It does not import hierarchy cells or repaint a
second Grid. The pure search pipeline is
`internal::FillerRepairPlanner`. Initial `initFillerRepair()` must succeed before
use: until it does, precheck and repair fail closed
(`precheck_not_initialized` / `engine_not_initialized`).

## Required opto sequence

1. Construct the existing `ImplantLayerChecker` with `DePlace::getGrid()` and
   `DePlace::getNetwork()`, then call `initFillerRepair()` once with
   `DePlace::getDesMgr()` and the filler setting. Do not construct a separate
   `FillerRepairEngine`.
2. Before any cell mutation, call `precheckFillerRepair()`.
3. If `precheckFillerRepair().isLegal == false`, stop. `Gap`/`Overlap` diagnostics are
   warnings for logging, but the bool is a hard blocking contract.
4. Call the existing `check(node, x, y, orient)`. It builds one
   `ipl::CheckRequest` and passes that exact request to the owned engine.
   Before registering a replacement master, repair repeats the coverage check
   over the initial target influence. Adaptive requests that edit farther rows
   expand the checked range before entering the checker batch. A defect outside
   rows touched by a request is left to the global `precheckFillerRepair()` gate.
5. If `check()` returns true, opto/infrastructure reads
   `getFillerChanges()` and commits that list together with its target
   mutation. The list is valid until the next `check()`; every new check clears
   it first. On false, inspect `getFillerRepairDiagnostics()` and do not commit.
6. After a position/master commit that keeps the same instance set and Grid
   topology, call `updateFillerRepair()` before the next query. Rebuild Grid/Network first
   when rows, blockages or the instance set changed.

fillerRepair provides the gate and checker callback; it does not commit.
Precheck and repair are both non-mutating.

## Precheck scope

Precheck checks only placement gaps and overlaps inside coverage-required
legal row segments. The supplied Grid is the domain authority: maximal runs
whose pixels satisfy `is_valid && padding_reserved_by == nullptr` are checked
exactly once. Hard blockages, fragmented-row holes, halo/padding reservations
and other non-placeable legal whitespace are outside that domain. Soft
blockages remain placeable under the existing Grid policy. Standard cells,
fillers and hard macros are Network Nodes; their intersections with legal rows
count as coverage. Cell coverage comes from `PhysDesMgr` physical cells
and the Network cell universe. Precheck does not check:

- target or proposed master;
- master-size compatibility;
- candidate availability;
- Node/Master ID mapping;
- site alignment or a separate out-of-bounds category;
- implant DRC.

`isLegal=true` means no gap/overlap in those legal segments. `isLegal=false`
means at least one
`Gap`/`Overlap` diagnostic and both opto and `repair()` must block.

## Repair semantics

The checker request master is already represented in Network because the
request comes from `Node::getMaster()->getId()`. Repair validates target type
and equal dimensions; if DePlace added that master after repair initialization,
the engine rebuilds its private oracle/snapshot before checking. The retained
direct UDM-handle overload can validate and lazily register an uninstantiated
master for focused engine tests. The first implant query then overlays the target new
master with an empty filler change list. No violation returns success with
empty changes. Violations enter
the unchanged adaptive-L1/ranker/subset/cache/budget/baseline-delta planner.
A clean solution returns `ipl::FillerChanges`; no solution returns failure and
empty changes. The final checker remains the only DRC oracle.

Optional debug logging is enabled with
`checker.setFillerRepairDebugLogging(true)`. It emits
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
| hard macros | Network Nodes; placed/fixed footprint supplies coverage |
| hard/soft blockages and padding | Grid; hard is invalid, soft remains valid, padding is reserved |
| filler allow-list | `fillerSetting::getFillerMasters()` |
| VT/band polarity | `PhysLibCell` implant shapes + checker `Layer::Vt/Polar` |
| implant legality | final `ImplantLayerChecker` |
| commit | opto/infrastructure |

Placed masters come from the existing Network. Checker repair initialization
idempotently registers configured candidate masters before constructing its
private oracle.
The direct test overload may idempotently register an uninstantiated target-new
master and rebuild that checker/snapshot before querying it. These registry updates do not mutate UDM
placement. Network must contain every placed/fixed physical instance that can
intersect the core, including hard macros; placement blockages are represented
by Grid and are not Network Nodes.

## Migration to the destination environment

Integration files:

1. `src/dpl2/src/fillerRepair/` -> next to the destination's existing
   `infrastructure/` and `drc/` directories (the runtime sources include
   `infrastructure/...` and `drc/ImplantLayerChecker.h` relative to that common
   source root).
2. Apply the branch's small `ImplantLayerChecker.h/.cpp` entry wiring: owned
   engine initialization/update/precheck, the `check()` call, and last-result
   accessors. This fills the destination checker's existing TODO and does not
   alter its DRC rule/scan algorithms.
   The branch also follows the destination checker's accessor-based
   `Layer`/`Rule` metadata model; there is no `ImplantLayer` compatibility
   struct to copy.
3. Preserve the branch's `Network::updateNodes(const PhysDesMgr*, const Grid*)`
   declaration/implementation. It refreshes existing Node state without
   changing Node/Master IDs. The destination infrastructure remains responsible
   for initially importing the complete instance universe.

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
`Network::addMaster(...)`/`updateNodes(...)`. No RepairInfrastructure,
leaf traversal or placement importer is copied into runtime.

Do not hand-copy file names -- both runtime and test CMake include the same
`sources.cmake`. Do not add `fillerRepair/test/*` to a runtime target.
The migrated E2E uses real Grid/Network/checker code and helper-built data.

Portable test wiring in the destination environment:

```sh
# Planner tests plus runtime engine + planner + final checker/helper E2E:
cmake -S <srcroot>/fillerRepair/test -B build-e2e \
  -DDPL2_UDM_INCLUDE_DIRS='<real UDM include dirs>' \
  -DDPL2_RUNTIME_LIBRARIES='<existing infra/checker targets>' \
  -DDPL2_UDM_LIBRARIES='<real UDM libs/targets>'
cmake --build build-e2e
ctest --test-dir build-e2e --output-on-failure
```

The standalone CMake builds one pure-planner target from the planner source
list. Its E2E target always consumes `${DPL2_FILLER_REPAIR_SOURCES}`, so it
compiles and links `FillerRepairEngine.cpp` against the destination headers.
`DPL2_RUNTIME_LIBRARIES` should name the existing
dpl2/checker owning targets; if omitted, the fallback compiles the adjacent
supplied infrastructure/checker sources.

## Build and verification

The CMake below `fillerRepair/test` is a portable test package, not the
destination's runtime owner. It builds an 82-case planner executable plus a
71-case E2E executable that compiles the complete checker/engine source list.
The separate local harness retains the 97 fake-UDM checker/engine cases.

Test dependencies: GoogleTest, Boost, TBB, C++17/C++20 and CMake 3.20+. Commands:

```sh
src/dpl2/test/local/run_planner_tests.sh
SANITIZE=address src/dpl2/test/local/run_planner_tests.sh
src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address src/dpl2/test/local/run_fake_udm_e2e.sh

cmake -S src/dpl2/test -B src/dpl2/test/build-cmake
cmake --build src/dpl2/test/build-cmake -j2
ctest --test-dir src/dpl2/test/build-cmake --output-on-failure
```

The migration payload has 26 final-checker fixture/overlay cases, 33
planner-to-final-checker cases and 12 internal exact-coverage precheck
cases. Each dense checker fixture contains eight rows and 200 sites. The four
rule classes and their planner repairs run at exact target-local-window
filler:standard-cell ratios 50:50, 30:70, 20:80, 10:90 and 5:95 while
preserving the same implant geometry.
The non-parameterized guard case proves that a target violation is still
detected when the changed neighbor lies outside the guard.
The planner matrix covers all four rule classes, clean/empty repair, determinism,
batch invariance, candidates, third VT, budgets, baseline consistency,
same-size edits, new-violation avoidance, a two-swap solution and a
checker-legal three-swap repair when the best residual initially points toward
a blocked adaptive side. The internal precheck matrix covers gaps, overlaps,
clipping, legal holes, row ordering and deterministic coalescing. Planner
doubles are same-level sources under `fillerRepair/test`; only the fake-UDM runtime suite
lives under `src/dpl2/test/local/`. Runtime cases cover
the internal repair precheck gate, hard macro and hard/soft blockage semantics,
side-effect-free invalid replacement requests and snapshot update.

2026-07-23 normal verification result: portable package 153/153 (planner 82/82
plus final-checker/precheck E2E 71/71), checker/engine fake-UDM E2E 97/97, and
full normal CTest 250/250. The last full ASan and `-Wall -Wextra -Werror`
verification predates the density expansion and must be rerun before updating
those claims. On Apple with an
unsanitized Homebrew GoogleTest, ASan discovery and CTest use
`ASAN_OPTIONS=detect_container_overflow=0` to avoid incompatible libc++ container annotations. The
engine cases include three layouts proving that unused-layer persistent
checker diagnostics remain non-blocking while a used implant layer with a
missing rule makes initialization fail closed.

The 97 checker/engine cases genuinely exercise Session, PhysDesMgr, physical
IDs, filler-master lookup and Network refresh, so the repository-local
test-only UDM-compatible provider and its one CMake include switch are still
required. Runtime sources contain no fake include or conditional.

## Integration risks

- Opto should call the checker precheck before mutation for early rejection.
  Repair checks the initial target influence before master registration and
  prechecks any farther row an adaptive candidate would edit. Illegal requests
  return `PrecheckFailed`; defects outside touched rows remain the global
  precheck's responsibility. After any placement mutation, call
  `updateFillerRepair()` before the next repair because local live reads cannot
  discover an object moved in from another snapshot row.
- Grid/Network/PhysDesMgr must describe the same revision and outlive the
  borrowing engine. `update()` refreshes existing Node state and atomically
  replaces the private snapshot. A failed update invalidates that snapshot and
  leaves queries fail-closed. New/deleted instances or row/blockage changes
  require infrastructure rebuild first.
- Network completeness is an infrastructure contract: every placed/fixed
  physical instance, including hard macros, must be a Node. Hard blockages
  belong to Grid, not Network.
- Supported design envelope (validated per node at init, Fatal otherwise):
  no pad row before a standard row, y-sorted row iteration, one shared row
  origin X equal to the core left edge, single contiguous span per row,
  orientations R0/R180/MX/MY. See CHECKER_REPAIR_CONTRACT.md "Row/column
  frames" for why (the checker mixes an iteration frame and a Grid frame).
- The supplied PhysDesMgr must be the UDM Session current design because the
  final checker constructor reads Session; init validates and fails closed on
  mismatch. The UDM design/library objects must outlive the engine.
- Destination build must consume `sources.cmake`, the checker entry patch and
  the Network refresh seam listed above; no other repair-specific file is part
  of the delivery.
- Destination verification still depends on its UDM include directories and
  link libraries/targets because Grid/Network headers use UDM types. Test data
  itself has no UDM/DEF/LEF dependency.
- Calls on one checker/engine pair must not overlap. The engine rejects
  reentrant repair and serializes private oracle calls; `getFillerChanges()`
  describes only the most recent completed `check()`.
- The runtime engine owns checker diagnostics translation; planner test doubles
  must remain outside runtime targets.
- Engine metadata extraction requires the checker to populate
  `Layer::TechLayerId`; it joins `PhysLibCell` shapes to checker layers by
  `TechLayerRelativeID`, not by a repeated layer-name lookup.
- `Types.h` remains a standalone bottom-level model header. Oracle-only
  `OracleRequest`/`OracleResult`/`OracleStatus` and `PlannerOracle` live in
  `OracleGate.h`; the public `RepairOutcome` remains in `FillerRepairEngine.h`.
  This avoids a second wire format and keeps Engine/Planner/Oracle dependencies
  one-way.
- On the checker path, a request master added to Network after initialization
  triggers a private snapshot rebuild. The direct test overload may add a
  previously uninstantiated target master after validation. Target/type/size
  validation and placement-precheck failures leave the registry unchanged.
  Once registration starts, a later rebuild failure leaves the engine
  fail-closed because Network has no transactional master rollback.
- Adaptive growth is still heuristic: it follows the best residual first and
  falls back to the opposite side only when that primary side adds nothing.
  Long irrelevant contiguous filler runs may therefore require several
  budgeted windows before the fallback is reached. Search failure remains
  atomic and returns no partial changes.
- A destination whose `Network::addMaster` overload has a different signature
  needs one mechanical change in the private `ensureMasterRegistered()` seam;
  no planner or checker change is involved.
