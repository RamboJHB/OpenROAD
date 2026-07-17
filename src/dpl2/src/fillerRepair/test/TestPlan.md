# Test Plan — fillerRepair

Updated: 2026-07-18. Status: complete for the V2.1 swap-only scope.

## Test tiers

| Tier | Command | Boundary |
|---|---|---|
| Planner unit | `src/dpl2/src/fillerRepair/test/run_tests.sh` | 87 white-box planner tests; private doubles allowed |
| Production E2E | `src/dpl2/test/build_all.sh` | fake UDM data only; real infrastructure/checker/adapter/planner |
| CTest | `src/dpl2/test/CMakeLists.txt` | same production E2E target |

All tiers support AddressSanitizer. Production E2E also compiles with
`-Wall -Wextra -Werror`.

## Final E2E composition

Included:

- fake UDM tech, masters, rows and physical-cell records;
- production `RepairInfrastructure`, Network and real `Grid.cpp`;
- production `fillerSetting::getFillerMasters()` candidate path;
- final `ImplantLayerChecker` initialized from the fixture `PhysDesMgr`;
- unified `adapter::PlacementView`;
- production `FillerRepairEngine`.

Excluded:

- `fillerRepair/fake/*`;
- Grid link stubs;
- fake PlacementDRC;
- RD Helper Grid injection;
- test DePlace/Network shims;
- manually constructed Network masters/nodes in the fixture.

## Assertions

The smoke verifies:

1. production infrastructure builds successfully;
2. Network nodes are imported from PhysDesMgr cell handles;
3. placed, target-new and all configured filler masters are registered;
4. unified adapter is ready;
5. adapter placement/precheck geometry agrees with PhysDesMgr;
6. candidate IDs are exactly the `getFillerMasters()` allow-list;
7. repair finds the required VTH filler swap;
8. output is an exact replacement `FillerCellRecord`;
9. only a valid adjacent filler is changed;
10. a second run produces an identical result without DB mutation.

The fixture exercises an uninstantiated target-new master and replacement
candidate registration, three VT domains, row-orientation alternation,
minimum-width repair and deterministic ranking.

## Unit-test isolation

The 87 planner tests use private Design/checker/scripted doubles for exact
window/search states and malformed-oracle protocol injection. A correct final
checker cannot generate those fault shapes on demand. These doubles remain in
the standalone unit executable only; they are not E2E, production or public
adapter dependencies.

## Commands and result

```sh
src/dpl2/src/fillerRepair/test/run_tests.sh
SANITIZE=address src/dpl2/src/fillerRepair/test/run_tests.sh

src/dpl2/test/build_all.sh
SANITIZE=address src/dpl2/test/build_all.sh

cmake -S src/dpl2/test -B src/dpl2/test/build-cmake
cmake --build src/dpl2/test/build-cmake -j2
ctest --test-dir src/dpl2/test/build-cmake --output-on-failure

cmake -S src/dpl2/test -B src/dpl2/test/build-cmake-asan \
  -DDPL2_ENABLE_ASAN=ON
cmake --build src/dpl2/test/build-cmake-asan -j2
ctest --test-dir src/dpl2/test/build-cmake-asan --output-on-failure
```

2026-07-18:

- planner normal: 87/87;
- planner ASan: 87/87;
- E2E script normal: pass;
- E2E script ASan: pass;
- CTest normal: 1/1;
- CTest ASan: 1/1.

## Regression rules

- Fixed input must produce identical changes, diagnostics and checker calls.
- Planner tests assert oracle handling, not independent DRC semantics.
- New production-boundary behavior needs a fake-UDM E2E assertion.
- No test may restore a non-UDM substitute to the E2E link target.
