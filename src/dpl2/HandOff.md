# HandOff — filler VT overlay repair

Updated: 2026-07-18. Branch: `claude/wizardly-carson-secahu`.

## Result

The swap-only filler repair project is integrated and locally verified. The
final E2E has one test-data substitute: fake UDM. It links production
infrastructure, final checker, unified adapter and planner.

```text
fake UDM (tech / masters / rows / physical cells)
                         |
             RepairInfrastructure::build
                         |
             production Network + Grid
                         |
               final ImplantLayerChecker
                         |
           adapter::PlacementView::repair
                         |
                 FillerRepairEngine
                         |
                ipl::FillerChanges
```

`src/dpl2/test/smoke_main.cpp` is the canonical wiring example.

## Completed work

- V2.1 planner algorithms are complete: OracleGate corrections,
  filler-domain enumeration, adaptive-L1 expansion, per-band polarity
  filtering/ranking and deterministic all-or-nothing output.
- `adapter::PlacementView` is the only production boundary: planner view,
  checker oracle and repair entry.
- Final checker alignment is complete:
  `Node::getId()`/`Master::getId()`, `FillerCellRecord`, ordered blocking
  results and `IntraRow`/`InterRow`.
- `RepairInfrastructure` builds real Network/Grid from `PhysDesMgr` physical
  data plus caller-supplied design leaf IDs.
- It registers all placed masters, the target new master and every master from
  `fillerSetting::getFillerMasters()` before checker construction.
- Real `Grid.cpp` is compiled and executed with Boost and TBB.
- Grid link stubs, Helper-based Grid injection, fake PlacementDRC and test
  DePlace/Network shims were removed from the E2E.
- Standalone CMake/CTest entry is available in `src/dpl2/test/CMakeLists.txt`.

## Data authority

| Data | Authority |
|---|---|
| row/core/site geometry | `PhysDesMgr` |
| placement status, origin, orientation and physical master | `PhysDesMgr` |
| leaf-cell universe | embedding application supplies `LeafCellID` list |
| instance/master topology and checker IDs | production `Network` built by `RepairInfrastructure` |
| filler candidate allow-list | `fillerSetting::getFillerMasters()` |
| VT family and band polarity | `PhysLibCell` implant shapes + checker layers |
| legality | final `ImplantLayerChecker` |

The explicit leaf-ID list keeps hierarchy traversal in the embedding
application. It carries identifiers only; all precheck/placement properties
are read back from `PhysDesMgr`.

## Lifetime and threading

`PhysDesMgr`, `fillerSetting`, Network, Grid, checker and adapter must describe
one design revision. `RepairInfrastructure` validates the design association
and is a one-build snapshot. After commit, construct a new infrastructure,
checker and adapter.

Adapter placement data is immutable. Coverage caching uses `call_once`; use
one engine per thread. Checker calls are serialized by the adapter because the
current checker const path updates internal counters.

## Build environment

Required local packages: Boost headers, TBB, a C++20 compiler and CMake 3.20+.
On Apple Silicon with Homebrew:

```sh
brew install boost tbb cmake
```

`build_all.sh` discovers TBB through pkg-config or `/opt/homebrew`. For Apple
ASan it links Homebrew's static TBB archive to avoid the dynamic oneTBB
finalizer-order crash.

## Verified commands

```sh
# 87 planner unit tests
src/dpl2/src/fillerRepair/test/run_tests.sh
SANITIZE=address src/dpl2/src/fillerRepair/test/run_tests.sh

# fake-UDM-only production E2E; -Wall -Wextra -Werror
src/dpl2/test/build_all.sh
SANITIZE=address src/dpl2/test/build_all.sh

# CMake/CTest
cmake -S src/dpl2/test -B src/dpl2/test/build-cmake
cmake --build src/dpl2/test/build-cmake -j2
ctest --test-dir src/dpl2/test/build-cmake --output-on-failure

cmake -S src/dpl2/test -B src/dpl2/test/build-cmake-asan \
  -DDPL2_ENABLE_ASAN=ON
cmake --build src/dpl2/test/build-cmake-asan -j2
ctest --test-dir src/dpl2/test/build-cmake-asan --output-on-failure
```

2026-07-18 result: planner 87/87 normal and ASan; E2E normal and ASan;
CTest 1/1 normal and ASan.

## Test-double boundary

The final E2E links no `fillerRepair/fake/*` files. The 87 white-box planner
tests retain private Design/checker doubles for malformed-protocol and precise
search-state injection; they are a separate unit-test executable and are not
production interfaces or E2E dependencies.

## Integration into the complete destination tree

Add `RepairInfrastructure.cpp`, planner sources and
`adapter/PlacementView.cpp` to the destination dpl2 target, then reuse the
smoke fixture or register the CTest target. The complete destination may use
its own hierarchy traversal to produce leaf IDs. No changes to checker DRC or
planner algorithms are required.

## Red lines

- Do not reproduce implant DRC in the planner.
- Do not introduce a second production adapter or placement model.
- Do not parse VT from master names.
- Do not mutate the design during an overlay query.
- Do not broaden this stage to merge/split/rewrite.
