# HandOff — filler VT overlay repair

Updated: 2026-08-03. Branch: `claude/wizardly-carson-secahu`.

What this feature does: opto changes one standard cell's VT. The fillers around
it still carry the old implant type, which is an MW/MS violation. This finds a
set of same-size filler master swaps that removes the violation, validates them
with the real `ImplantLayerChecker`, and hands the records back to opto to
commit. It never mutates UDM placement or Grid. During initialization it may
register configured masters; infrastructure has already classified filler
Masters and Nodes from `fillerSetting::core_`.

---

## 0. Migration snapshot

**Take the payload from `808c27f`** — the commit this section describes. It is
complete: payload, the null-safety work, and the search as it now performs.
Everything below describes exactly that state.

```sh
git tag fillerRepair-migration-20260802 808c27f
```

(Tags exist locally only; this environment's git proxy accepts writes to the
working branch and refuses tag refs, so the commit id is the reference that
actually travels. This section is the only thing that moved afterwards, to
record the id.)

Earlier reference points, for reading history only — do **not** port from
them:

| | |
|---|---|
| `bfe8642f23` | first complete portable module; predates the null-safety work |
| `8ca27117c6` | the null-safety delta, now folded in |

Verified at `808c27f`:

| | |
|---|---|
| local suite | 274/274, normal and ASan |
| migration gate (destination code path) | 170/170, normal and ASan |
| standalone module | 170/170 — configure, build and test with no harness |
| `testFillerRepairCmd` | syntax-checked against the real dpl2 headers, `-Wall -Wextra`; **never linked** here |

---

## 1. What to take

| | |
|---|---|
| **Payload** | `src/dpl2/src/fillerRepair/` — whole directory, including its `CMakeLists.txt` and `test/` |
| **Test command** | `src/dpl2/dpl2ui/testFillerRepairCmd.{hh,cc}` — `test_filler_repair`, optional. Run `set_filler_option` first, then `test_filler_repair` to sweep, or `test_filler_repair -inst <instance id> -master <master id>` for one VT swap. Both options take id numbers — the same `LeafCellID` / `LibCellID` handles `DePlace::isLegal` takes — and the sweep prints its findings as the `-inst`/`-master` pair that reproduces them |
| **Patches to delivered code** | **not in either path above** — eleven files, all tagged `[fillerRepair-fix]`, itemised with reasons in `src/dpl2/src/drc/CHECKER_REPAIR_CONTRACT.md`: `drc/DRCChecker.h`, `drc/ImplantLayerChecker.{h,cpp}`, `drc/ImplantLayerCheckerHelper.cpp`, `infrastructure/Objects.h`, `infrastructure/Object.cpp`, `infrastructure/Grid.{h,cpp}`, `infrastructure/network.cpp`, `DePlace.cpp`, `include/dpl2/DePlace.h`. Without them the payload does not build. The `DePlace` / `Grid::isFullUtil` pair is a **correctness fix in delivered code, independent of repair** — grid occupancy was missing every filler |
| **Not part of the payload** | `src/dpl2/test/` — the repository-local harness (fake UDM tree, engine regression, runner scripts). It exists so this can be developed and gated without a real UDM. |

The payload needs no DEF/LEF reader, no fake UDM, and no fixture provider from
the destination. `src/dpl2/src/fillerRepair/README.md` is the module's own
documentation and travels with it.

### Null-safety patch must travel

The destination infrastructure and `ImplantLayerChecker` do **not** yet
contain the fail-closed null handling present on this branch. It is already
part of the `808c27f` snapshot above — this section only says which files it
lives in, so a destination that cherry-picks rather than taking the snapshot
does not leave it behind.

The destination-side portion is:

- `infrastructure/Grid.cpp`
- `infrastructure/network.{h,cpp}`
- `drc/ImplantLayerChecker.{h,cpp}`

The payload already carries its matching guards in
`FillerRepairEngine.cpp` and `RepairPlanner.cpp`. The destination patch must
reject missing managers, invalid physical mappings, null Network slots,
missing masters, unallocated/out-of-range Grid access, and invalid checker
targets/overlays before dereference. `Network::addNode` also changes from
`void` to `bool`, so destination callers must compile against that contract.

This is a migration dependency, not optional cleanup. Porting only the
`fillerRepair/` directory leaves earlier infrastructure/checker dereferences
outside the engine's control. After adapting the patch, rerun the portable
suite, migration gate, and an ASan full run before enabling repair.

---

## 2. The caller boundary

`ImplantLayerChecker::check()` is the only entry. Opto owns the record vector;
the checker appends into it and keeps no filler-change member state.

```cpp
std::vector<CellChangeRecord> fcRecord;
bool legal = deplace->isLegal(cellId, lcId, fcRecord);   // -> checker.check(...)
if (legal && !fcRecord.empty()) {
  commitFillerSwaps(fcRecord);   // commit stays with opto/infrastructure
}
```

Each current repair entry is
`CellChangeRecord{Replace, CellData{LeafCellID}, origin_x_, origin_y_,
orig_lib_cell_, new_lib_cell_, orientation_}`. `CellData` can represent a
future named cell with `std::string`, but filler VT repair remains swap-only
and does not emit that alternative.

There is no `initFillerRepair`, `precheckFillerRepair`, `updateFillerRepair` or
`getFillerChanges` to call — those are gone. The engine is built **lazily** on
the first DRC-illegal check, so a run whose checks all pass never pays for it.

Two things must reach the checker before the first failing check:

- **`PhysDesMgr`** — `Grid` retains the manager used by `initGrid()`;
  `ImplantLayerChecker(Grid*, Network*)` reads it through `Grid::getDesMgr()`.
  Missing `Grid`, `Network`, or manager is a fatal checker initialization
  diagnostic.
- **`fillerSetting`** — `DePlace` registers a provider once:
  `ImplantLayerChecker::setFillerRepairSettingProvider(&provideSetting)`.
  The checker never names `DePlace`, so builds without it still link.
  A harness with no `DePlace` owner calls
  `checker.setFillerRepairContext(desMgr, &fillerSetting)` instead.

If configuration has not arrived yet, the check returns illegal, emits one
`[fr]` notice, and disables repair until `setFillerRepairContext()` explicitly
resets the checker. Structural `FillerRepairEngine::init()` failures are also
fail-closed.

**Swap-only.** Same instance, same position, same orientation, same width and
height, different master. A target whose placement moved is refused
(`UnsupportedTargetMove`).

---

## 3. Build wiring

The module owns its targets, so the destination never lists our files:

```cmake
# point the payload at your headers/libraries...
add_library(dpl2_filler_repair_deps INTERFACE)
target_link_libraries(dpl2_filler_repair_deps INTERFACE <udm> <infra/checker>)
# ...then add it and link a target
add_subdirectory(<srcroot>/fillerRepair fillerRepair)
target_link_libraries(<owning-target> PRIVATE dpl2::fillerRepair)
```

| Target | Contents | Standard |
|---|---|---|
| `dpl2::fillerRepair` | complete payload | C++20 |
| `dpl2::fillerRepairPlanner` | pure search pipeline, no database access | C++17 |

`dpl2_filler_repair_deps` is the single external seam — UDM, dpl2
infrastructure, the implant checker, and any global flags such as sanitizers all
arrive through it. Defining it is optional: without it the module falls back to
the in-tree layout plus the `DPL2_UDM_INCLUDE_DIRS` / `DPL2_UDM_LIBRARIES`
cache variables, which is what lets it configure and build standalone.

The only requirement on the destination side is that the common source root is
on the include path — already true if `infrastructure/...`-style includes work
today. Includes use angle brackets throughout
(`<fillerRepair/RepairPlanner.h>`, `<infrastructure/Grid.h>`).

---

## 4. Verifying the port

```sh
cmake -S <srcroot>/fillerRepair -B build-fr \
  -DDPL2_FILLER_REPAIR_BUILD_TESTS=ON \
  -DDPL2_UDM_INCLUDE_DIRS='<real UDM include dirs>' \
  -DDPL2_UDM_LIBRARIES='<real UDM libs/targets>' \
  -DDPL2_RUNTIME_LIBRARIES='<existing infra/checker targets>'
cmake --build build-fr && ctest --test-dir build-fr --output-on-failure
```

170 portable tests: 91 database-free planner cases and 79 that drive the **real
`ImplantLayerChecker`** through `ImplantLayerCheckerHelper`-built input. They
build no UDM objects, so they run before any design is available.

`DPL2_RUNTIME_LIBRARIES` should name the destination's existing infra/checker
targets. If omitted, the fallback compiles the adjacent supplied sources — the
suite still runs before any wiring exists. Either way the E2E executable links
`dpl2::fillerRepair`, so a broken engine/UDM boundary fails the link rather
than passing a planner-only build.

Rerun with `-DDPL2_ENABLE_ASAN=ON` before signing off.

---

## 5. What the destination must guarantee

- **One design revision.** `PhysDesMgr`, `Grid`, `Network` and one engine
  describe the same revision. UDM design/library objects outlive the engine.
  `Grid::getDesMgr()` is the design authority: engine initialization rejects a
  different manager, and the private oracle checker receives only `Grid` and
  `Network`. No global design state is read.
- **Network completeness.** Every placed/fixed physical instance that can
  intersect the core, hard macros included. Placement blockages stay Grid
  state, not Network Nodes.
- **Frames.** `RowId` is the Grid row, x is core-left-relative — the frame
  `check()` already builds its `CheckRequest` in. Infrastructure data is
  consumed as-is; there is no Network↔UDM cross-validation, because with lazy
  init the engine typically runs mid-check while the candidate Node already
  carries its proposed master ahead of the pending UDM commit.
- **Supported design envelope**, validated at init (Fatal otherwise): no pad
  row before a standard row, y-sorted row iteration, one shared row origin X
  equal to the core left edge, single contiguous span per row, orientations
  R0/R180/MX/MY. See CHECKER_REPAIR_CONTRACT.md "Row/column frames" for why.
- **No overlapping calls** on one checker/engine pair. The engine rejects
  re-entry (`ReentrantRepair`), and the planner is one-repair-at-a-time.
- **Commit is the caller's.** Repair is non-mutating end to end.

---

## 6. Placement legality: who checks what

The engine's only placement gate is **regional** — it refuses to run on a
gap/overlap inside the rows it can edit, with legal spans derived lazily from
Grid pixels per row. Whole-design placement legality is infrastructure's own
gate; the engine has no global precheck and does not want one.

---

## 7. Behaviour worth knowing before review

- **Acceptance is baseline-delta, not "zero violations".** A candidate is
  accepted only if it leaves no original violation, adds nothing inside the
  repair window, and adds nothing in the halo related to its own swaps.
  Unrelated pre-existing halo findings are reported, never blocking.
- **Bounded search.** `checkerCallBudgetPerWindow` (512) bounds one window;
  `checkerCallBudgetPerRepair` (2048) bounds one `repair()` across every
  adaptive level. Reaching either ends the search as *truncated* — never a
  wrong answer, only a bounded give-up.
- **The transcript is on by default.** `FR_VERBOSE=0` silences it. A
  production run leaves a diagnosable `[fr][stage]` trail without a rebuild.
- **Determinism.** Same input, same output — ordering is pinned at every
  stage, and the answer cache is never iterated.

---

## 8. Every decision the destination has to make — the `[PORT-*]` tags

Everything that needs a yes/no from the integrator is tagged in the source.
One grep is the complete list:

```sh
grep -rn "\[PORT-" <srcroot>/fillerRepair
```

The legend lives at the top of `fillerRepair/RepairTypes.h`, next to the
conventions, so it is the first thing a reader of the payload meets:

| Tag | Meaning | Optional? |
|---|---|---|
| **`[PORT-ADAPT]`** | Will not compile, or will be quietly wrong, until you change it. Each one names the destination-side thing it depends on. | **No.** Work through all four before the first run. |
| **`[PORT-DROP]`** | You do not need this. Each says what it costs to keep and what breaks if you delete it — which is nothing in production. | Yes |
| **`[PORT-TUNE]`** | A number or a strategy chosen from measurements taken *here*, against a synthetic oracle. Each says what to measure on your hardware first. | Yes — safe as shipped |

### The eleven ADAPTs

Sorted by what happens if you get them wrong. **The first five compile
cleanly and are silently wrong** — no crash, no diagnostic, just answers about
the wrong thing. Read those first.

| Where | What it depends on | If wrong |
|---|---|---|
| `FillerRepairEngine.cpp` `buildPlannerData` — row frame | RowId is the Grid row, x is core-left-relative — the same frame the checker builds `CheckRequest` in. Nothing re-derives or re-validates it | **silent**: every lookup is about the wrong place |
| `FillerRepairEngine.cpp` — init-diagnostic strip | Checker *behaviour*: it repeats its init diagnostics into every result. A different count, or not as a leading run | **silent**: every candidate comes back illegal, repair never finds anything |
| `FillerRepairEngine.cpp` `checkPlaceWithOverlays` | Batch semantics: one `FillerChanges` = one candidate, results correlate **by input order**, count must match | **silent**: answers mis-attributed to candidates |
| `FillerRepairEngine.cpp` — initial halo sizing | `getMaxRuleValue()` means checker reach in **sites** and remains the correctness floor; the other input is the widest configured filler master, never an arbitrary placed standard cell or macro. The later planner guard uses the actual two-cell ring | **silent**: too-small reach truncates runs; global placed-master sizing makes macro designs pathologically slow |
| `RepairPlanner.cpp` `finalizeWindow` — guard rows | Inter-row rules reach **one** row boundary, so ±2 rows of guard covers it. Horizontal reach is not guessed like this; it comes from the checker | **silent**: the checker is never shown the row a new violation appeared in |
| `FillerRepairEngine.cpp` `init` | `Grid::getDesMgr()`, so Grid/Network/checker/engine provably describe one design revision | caught: fatal init diagnostic |
| `FillerRepairEngine.cpp` `ensureMasterRegistered` | `Network::addMaster`'s signature — the only version-sensitive *signature* in the payload, already changed twice | caught: compile error |
| `FillerRepairEngine.cpp` `cellChangeRecord` | `CellChangeRecord`'s shape. Fill **every** field; `orientation_` is read when the checker evaluates the swapped filler | mixed |
| `FillerRepairEngine.cpp` `buildPlannerData` — filler identity | Infrastructure's single filler authority (`Master`/`Node` carry it). Never re-derive from UDM macro flags — they disagree, and that was a real bug | mixed |
| `FillerRepairEngine.cpp` `implantLayerOf` | Reads master implant shapes the way the checker does (layer identity via `TechLayerRelativeID`, band anchored at the bottommost rect) | mixed |
| `fillerRepair/CMakeLists.txt` | Defining `dpl2_filler_repair_deps` — that target *is* the integration | caught: link error |

### The DROPs

| Where | Costs to keep | What you lose by deleting |
|---|---|---|
| `FillerRepairEngine::update()` | ~30 lines | The repository-local regression's snapshot-refresh entry. Production refreshes via `setFillerRepairContext()` instead |
| `FillerRepairEngine::repair(LeafCellID, PhysLibCell)` | ~25 lines | The same, for callers holding UDM handles and no `CheckRequest` |
| `FillerRepairEngine::setDebugLogging()` | ~10 lines | Per-engine transcript control. `FR_VERBOSE=0` already does it globally |
| `dpl2::fillerRepairPlanner` target | one extra compile | The guard that keeps UDM out of the search. Recommended to keep |
| `CMakeLists.txt` standalone fallback | a `if(NOT TARGET ...)` branch + two cache vars | The ability to build and test the payload with no destination wiring at all. Keep until your build is proven |

**Not droppable, despite looking like it:** `isOracleSnapshotClean()`
(`RepairOracle.h`) and the `PlacementView::getUsableMasterCandidates` virtual.
Both are used by the portable tests, and those travel with the payload.

### The TUNEs — and why they are not just fixed here

Four, all in `RepairPlanner.{h,cpp}`. Each one is a question whose answer
lives on the destination's hardware, not in this repository: the oracle these
were measured against is synthetic and answers in ~0 ns, so the very quantity
that decides them is missing here.

| Knob | The number that decides it | Why this repo cannot supply it |
|---|---|---|
| `checkerCallBudgetPerWindow` (512), `checkerCallBudgetPerRepair` (2048) | cost of one real DRC call | These are really "how much DRC time may one repair cost". At 50 µs/call 2048 is 0.1 s; at 5 ms/call it is 10 s. A 100x spread inverts the answer, and here every call is free |
| `batchSize` (32) | the checker's `parallelFor` width, and its per-batch fixed cost | A batch is one call whose candidates run in parallel over one region scan. The synthetic oracle has neither a thread pool nor a fixed cost |
| `maxAdaptiveLevels` (32) | how far from a target a usable filler actually sits, on real designs | A safety valve sized against synthetic fixtures. Your `grown xN` histogram answers it in one run |
| batch coalescing (not done) | thread count **and** the cost of a speculative candidate | Trades a fixed per-batch cost against checking candidates an earlier answer may make unnecessary. Which way it lands is a hardware question |

They are safe as shipped — reaching a budget only ends a search early, it
never yields a wrong answer — and the transcript already prints what you need:
`checker requests=` per repair, and which level each answer came from.

**One was not a real TUNE and has been settled here instead.**
`OverlayKey::kInlineSwaps` depends only on this module's own enumerator, so
pushing it downstream would have been passing on work that was ours. Measured
over the worst-case search, key sizes top out at 5 (of 85 160 keys: 43 560 at
size 2, 640 at size 5), so 8 covers every one without touching the heap. That
measurement is now in the comment in place of the tag.

### Rides along, not a dependency

**`Grid::getBoundingBox` / `DePlace::getBoundingBox`** — the module never
calls it. It is an independent feature that happens to be in the same patch
set and can be dropped without affecting repair. `Grid::gridXY` in the same
file must stay: the checker uses it.

---

## 9. State of this branch

| | |
|---|---|
| Portable planner tests | 91 |
| Portable real-checker E2E | 79 |
| Repository-local engine regression | 106 (fake UDM, not migrated) |
| Full local suite | 276/276, normal and ASan |
| Migration gate (destination code path) | 170/170, normal and ASan |
| Standalone module build | 170/170 |

The migration gate builds the payload the way a destination does
(`DPL2_TEST_USE_FAKE_UDM=OFF`, no fake-only target, no test provider) with the
fake headers supplied through the real-UDM knob. It does not prove the headers
are real; it proves every source compiles and every executable's **link closure
is complete** in that configuration. A static compile-check library cannot show
this — archives do not resolve symbols; only linking an executable does.
