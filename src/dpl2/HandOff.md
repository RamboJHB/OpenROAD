# HandOff — filler VT overlay repair

Updated: 2026-07-18. Branch: `claude/wizardly-carson-secahu`.

## Result

The destination already supplies complete infrastructure and checker sources.
Production migration therefore copies only `src/dpl2/src/fillerRepair/` and
adds its production `.cpp` files to the destination's existing target. No
infrastructure/checker source or API change is required, and this change set
keeps both directories at zero diff.

The production boundary is now one checker-style class:

```cpp
FillerRepairEngine(Grid* grid, Network* network);
bool init(PhysDesMgr* desMgr,
          const ImplantLayerChecker* checker,
          const fillerSetting* fillerSetting);
ipl::CheckResult precheck() const;
RepairOutcome repair(LeafCellID targetCell, const PhysLibCell& newMaster);
```

Production callers no longer construct an adapter or planner PlacementView.
`adapter/PlacementView.{h,cpp}` and `CheckerApi.h` were deleted. The production
view/oracle/wire conversion lives in `FillerRepairEngine.cpp`; the old pure
search pipeline is isolated as `internal::PlannerEngine`.

## Required opto sequence

1. Construct engine with the current Grid/Network and call `init()`.
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

Precheck checks only placement gaps and overlaps over non-pad rows using
`PhysDesMgr` physical cells and the Network cell universe. It does not check:

- target or proposed master;
- master-size compatibility;
- candidate availability;
- Node/Master ID mapping;
- site alignment or a separate out-of-bounds category;
- implant DRC.

`isLegal=true` means no gap/overlap. `isLegal=false` means at least one
`Gap`/`Overlap` diagnostic and opto must block. `repair()` deliberately does
not call precheck again.

## Repair semantics

The first implant query overlays the target new master with an empty filler
change list. No violation returns success with empty changes. Violations enter
the unchanged adaptive-L1/ranker/subset/cache/budget/baseline-delta planner.
A clean solution returns `ipl::FillerChanges`; no solution returns failure and
empty changes. The final checker remains the only DRC oracle.

## Data authority

| Data | Authority |
|---|---|
| rows and physical placement | `PhysDesMgr` |
| cell/master topology and checker IDs | production Network |
| filler allow-list | `fillerSetting::getFillerMasters()` |
| VT family/band polarity | `PhysLibCell` implant shapes + checker layers |
| implant legality | final `ImplantLayerChecker` |
| commit | opto/infrastructure |

`RepairInfrastructure` registers placed, target-new and configured candidate
masters before checker/engine construction, including uninstantiated masters.

## Destination build wiring

The destination has no supplied dpl2 CMake fragment, so its owner must add
these fillerRepair production sources to the target that already owns
Grid/Network/checker:

```text
FillerRepairEngine.cpp  PlannerEngine.cpp  OracleGate.cpp
PlacementView.cpp       Ranker.cpp          Signature.cpp
SubsetSearch.cpp        Swap.cpp            Window.cpp
```

Do not add `fillerRepair/fake/*`, `fillerRepair/test/*`, or a fake UDM include
path to a production target. No production source uses a fake/real UDM
conditional; it includes the real UDM names already used by infra/checker.

## Build and verification

The standalone test CMake is not a proposed production CMake file. It provides
one interface-only selection point:

- default: `DPL2_TEST_USE_FAKE_UDM=ON`, using the test-only UDM-compatible
  include root selected by `DPL2_TEST_FAKE_UDM_INCLUDE_DIR`;
- real UDM rehearsal: set it `OFF` and provide
  `DPL2_TEST_UDM_INCLUDE_DIRS` and/or `DPL2_TEST_UDM_LIBRARIES`.

Both modes compile the same infra/checker/fillerRepair sources. The fake uses
the real UDM namespaces, type names, signatures and placement behavior; only
include/link configuration changes.

Test dependencies: GoogleTest, Boost, TBB, C++20 and CMake 3.20+. Commands:

```sh
src/dpl2/src/fillerRepair/test/run_tests.sh
SANITIZE=address src/dpl2/src/fillerRepair/test/run_tests.sh
src/dpl2/test/build_all.sh
SANITIZE=address src/dpl2/test/build_all.sh

cmake -S src/dpl2/test -B src/dpl2/test/build-cmake
cmake --build src/dpl2/test/build-cmake -j2
ctest --test-dir src/dpl2/test/build-cmake --output-on-failure
```

All 87 planner cases and the E2E are GoogleTests. The E2E uses fake UDM only as
test data and links supplied Grid/Network,
RepairInfrastructure, final checker, FillerRepairEngine and internal planner.
It covers clean/gap/overlap precheck, opto-blocking return values, deterministic
repair, and byte-equivalent physical snapshots before/after both APIs.

2026-07-18 result: planner 87/87 normal and ASan; E2E normal and ASan; full
CTest 88/88 normal and ASan; all targets passed Werror.

## Integration risks

- Opto must honor the explicit precheck ordering; repair has no fallback gate.
- Engine/Grid/Network/checker/PhysDesMgr must describe one design revision;
  rebuild after commit.
- Destination build must add the nine fillerRepair sources listed above and
  must not add the deleted adapter or standalone `CheckerApi.h`.
- Real-UDM verification still depends on the destination providing its UDM
  include directories and link libraries/targets; no code port remains.
- Checker calls are serialized inside the engine because the checker const
  overlay path updates counters.
- The public facade owns production translation; planner fake/checker types
  must remain outside production targets.
