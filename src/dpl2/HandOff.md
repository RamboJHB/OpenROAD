# HandOff — filler VT overlay repair

Updated: 2026-07-29. Branch: `claude/wizardly-carson-secahu`.

What this feature does: opto changes one standard cell's VT. The fillers around
it still carry the old implant type, which is an MW/MS violation. This finds a
set of same-size filler master swaps that removes the violation, validates them
with the real `ImplantLayerChecker`, and hands the records back to opto to
commit. It never mutates UDM, Network or Grid.

---

## 0. Migration snapshot

**`bfe8642f23`** — take the payload from this commit. Everything below
describes exactly that state. (This section is the only thing that changed
afterwards, to record the id.)

```sh
git tag fillerRepair-migration-20260729 bfe8642f23
```

(The tag exists locally only; this environment's git proxy accepts writes to
the working branch and refuses tag refs, so the commit id is the reference
that actually travels.)

Verified at that commit:

| | |
|---|---|
| local suite | 241/241, normal and ASan |
| migration gate (destination code path) | 160/160, normal and ASan |
| standalone module | 160/160 — configure, build and test with no harness |
| `testFillerRepairCmd` | syntax-checked against the real dpl2 headers, `-Wall -Wextra`; **never linked** here |

---

## 1. What to take

| | |
|---|---|
| **Payload** | `src/dpl2/src/fillerRepair/` — whole directory, including its `CMakeLists.txt` and `test/` |
| **Test command** | `src/dpl2/dpl2ui/testFillerRepairCmd.{hh,cc}` — `test_filler_repair`, optional |
| **Patches to delivered code** | **not in either path above** — eleven files, all tagged `[fillerRepair-fix]`, itemised with reasons in `src/dpl2/src/drc/CHECKER_REPAIR_CONTRACT.md`: `drc/DRCChecker.h`, `drc/ImplantLayerChecker.{h,cpp}`, `drc/ImplantLayerCheckerHelper.cpp`, `infrastructure/Objects.h`, `infrastructure/Object.cpp`, `infrastructure/Grid.{h,cpp}`, `infrastructure/network.cpp`, `DePlace.cpp`, `include/dpl2/DePlace.h`. Without them the payload does not build. The `DePlace` / `Grid::isFullUtil` pair is a **correctness fix in delivered code, independent of repair** — grid occupancy was missing every filler |
| **Not part of the payload** | `src/dpl2/test/` — the repository-local harness (fake UDM tree, engine regression, runner scripts). It exists so this can be developed and gated without a real UDM. |

The payload needs no DEF/LEF reader, no fake UDM, and no fixture provider from
the destination. `src/dpl2/src/fillerRepair/README.md` is the module's own
documentation and travels with it.

---

## 2. The caller boundary

`ImplantLayerChecker::check()` is the only entry. Opto owns the record vector;
the checker appends into it and keeps no filler-change member state.

```cpp
std::vector<FillerCellRecord> fcRecord;
bool legal = deplace->isLegal(cellId, lcId, fcRecord);   // -> checker.check(...)
if (legal && !fcRecord.empty()) {
  commitFillerSwaps(fcRecord);   // commit stays with opto/infrastructure
}
```

There is no `initFillerRepair`, `precheckFillerRepair`, `updateFillerRepair` or
`getFillerChanges` to call — those are gone. The engine is built **lazily** on
the first DRC-illegal check, so a run whose checks all pass never pays for it.

Two things must reach the checker before the first failing check:

- **`PhysDesMgr`** — the checker's own `init()` already remembers it.
- **`fillerSetting`** — `DePlace` registers a provider once:
  `ImplantLayerChecker::setFillerRepairSettingProvider(&provideSetting)`.
  The checker never names `DePlace`, so builds without it still link.
  A harness with no `DePlace` owner calls
  `checker.setFillerRepairContext(desMgr, &fillerSetting)` instead.

If configuration has not arrived yet, the check simply returns illegal, emits
one `[fr]` notice, and **retries on the next failing check** — it is not a
permanent failure. Only a structural `FillerRepairEngine::init()` failure
disables repair for that checker's lifetime.

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

160 portable tests: 85 database-free planner cases and 75 that drive the **real
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
  The `PhysDesMgr` need *not* be the Session current design: the engine builds
  its private oracle checker with the exact `PhysDesMgr` it was given.
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

## 8. API the destination does not need

Public surface that exists for the repository-local regression suite, which is
not migrated. None of it is called by the payload, by the checker, or by any
production path — listed so a reviewer does not go looking for the caller.

| | Why it exists |
|---|---|
| `FillerRepairEngine::update(desMgr, fillerSetting)` | Pre-dates lazy init. Production refreshes by calling `setFillerRepairContext()`, which drops the engine so the next failing check rebuilds it; there is no path that reaches `update()`. Kept because the local regression drives snapshot refresh through it. |
| `FillerRepairEngine::repair(LeafCellID, const PhysLibCell&)` | The direct UDM-handle entry. Production enters through `ImplantLayerChecker::check()`, which uses the `CheckRequest` overload. Kept as the local regression's entry point. |
| `FillerRepairEngine::setDebugLogging(bool)` | The transcript is on by default and `FR_VERBOSE=0` silences it globally; this is the per-engine override. |
| `isOracleSnapshotClean()` (`RepairOracle.h`) | Assertion helper for the portable planner tests. |

Deleting them is safe for the destination and costs about sixty lines. It is
not recommended: they are the local suite's entry points, so removing them
weakens the ability to reproduce a destination-reported problem here.

Separately, **`Grid::getBoundingBox` / `DePlace::getBoundingBox` is not a
fillerRepair dependency** — the module never calls it. It is an independent
feature that happens to ride in the same patch set, and can be dropped without
affecting repair. `Grid::gridXY` in the same file must stay: the checker uses
it.

---

## 9. State of this branch

| | |
|---|---|
| Portable planner tests | 85 |
| Portable real-checker E2E | 75 |
| Repository-local engine regression | 81 (fake UDM, not migrated) |
| Full local suite | 241/241, normal and ASan |
| Migration gate (destination code path) | 160/160, normal and ASan |
| Standalone module build | 160/160 |

The migration gate builds the payload the way a destination does
(`DPL2_TEST_USE_FAKE_UDM=OFF`, no fake-only target, no test provider) with the
fake headers supplied through the real-UDM knob. It does not prove the headers
are real; it proves every source compiles and every executable's **link closure
is complete** in that configuration. A static compile-check library cannot show
this — archives do not resolve symbols; only linking an executable does.
