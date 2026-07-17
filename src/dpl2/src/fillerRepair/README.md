# fillerRepair — filler VT overlay repair planner

Updated: 2026-07-18.

This directory contains the deterministic, non-mutating, swap-only V2.1
planner. After an ECO proposes a new standard-cell master, it searches
same-position/same-size filler replacements and asks the implant checker to
validate atomic overlays. It never implements DRC or commits the database.

## Production chain

```text
PhysDesMgr + leaf IDs + fillerSetting + target new master
                         |
                RepairInfrastructure
                         |
                  Network + Grid
                         |
             adapter::PlacementView
                 |               |
           planner view    final checker oracle
                 \               /
                 FillerRepairEngine
                         |
                ipl::FillerChanges
```

The final E2E uses fake UDM only to provide tech/library/placement data. Every
component below that data boundary is production code.

## Main files

| Path | Purpose |
|---|---|
| `Types.h` | planner IDs, geometry, changes, diagnostics and results |
| `PlacementView.h/.cpp` | abstract placement view and utility coverage check |
| `CheckerApi.h` | planner-owned checker oracle interface |
| `Swap.h/.cpp` | atomic filler swap generation/validation |
| `Signature.h/.cpp` | violation normalization, matching and relatedness |
| `Window.h/.cpp` | L0/adaptive-L1 windows and guards |
| `Ranker.h/.cpp` | deterministic filler/domain ranking |
| `SubsetSearch.h/.cpp` | filler combinations × domain assignments |
| `OracleGate.h/.cpp` | batch/cache/baseline-delta acceptance |
| `FillerRepairEngine.h/.cpp` | repair pipeline |
| `adapter/PlacementView.h/.cpp` | production view/oracle/repair boundary |
| `fake/` | private standalone-unit-test doubles; never linked into E2E |

Production infrastructure construction is in
`../infrastructure/RepairInfrastructure.{h,cpp}`.

## Data contract

- physical placement and precheck inputs: `PhysDesMgr`;
- candidate allow-list: `fillerSetting::getFillerMasters()`;
- topology and checker IDs: production Network;
- VT/polarity: physical implant shapes matched to checker layers;
- legality: final checker;
- output: `ipl::FillerChanges` / `FillerCellRecord`.

All placed masters, the proposed target master and all configured filler
masters are registered before checker construction.

## Verification

```sh
# planner unit tests
test/run_tests.sh
SANITIZE=address test/run_tests.sh

# from src/dpl2
test/build_all.sh
SANITIZE=address test/build_all.sh
```

The E2E also has `src/dpl2/test/CMakeLists.txt` and a CTest registration.
Current result: planner 87/87 normal+ASan; fake-UDM-only E2E normal+ASan;
all production-chain sources compile with `-Wall -Wextra -Werror`.
