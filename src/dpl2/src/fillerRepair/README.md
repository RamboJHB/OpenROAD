# fillerRepair — filler VT overlay repair

Updated: 2026-07-18.

`FillerRepairEngine` is the only production entry. It exposes a placement-only
precheck plus pre-commit filler repair and never mutates the database.
The destination already owns complete Grid/Network/infrastructure/checker
implementations, so only this directory is migrated; none of those supplied
sources is patched.

```cpp
FillerRepairEngine engine(grid, network);
engine.init(desMgr, checker, fillerSetting);

ipl::CheckResult placement = engine.precheck();  // opto calls before mutation
RepairOutcome outcome = engine.repair(targetCell, newMaster);
```

Precheck reports only `Gap` and `Overlap`. Warning diagnostics explain the
location; `isLegal=false` is the hard opto-blocking value. It does not inspect
target/master/candidates/IDs/implant DRC. Repair never calls precheck.

Repair overlays `newMaster` and first asks the final checker with empty filler
changes. A clean snapshot succeeds with empty changes; otherwise the internal
planner searches same-position/same-size filler swaps. Commit remains with
opto/infrastructure.

## Main files

| Path | Purpose |
|---|---|
| `FillerRepairEngine.h/.cpp` | production API, private view/oracle conversion, precheck and repair |
| `PlannerEngine.h/.cpp` | internal deterministic search pipeline |
| `OracleGate.h/.cpp` | internal checker abstraction, batching, cache and baseline-delta gate |
| `PlacementView.h/.cpp` | planner-only read view and gap/overlap coverage helper |
| `Types.h` | planner-internal IDs, geometry and request/result types |
| `Swap`, `Signature`, `Window`, `Ranker`, `SubsetSearch` | unchanged search stages |
| `fake/` | planner unit-test doubles; never linked into production/E2E |

The production adapter directory and standalone `CheckerApi.h` are deleted.
Planner `OverlayCheckRequest`, `CheckStatus` and requestId stay internal to
`OracleGate`; the public API uses final checker `CheckResult`, `Diagnostic`,
`FillerChanges` and `FillerCellRecord`.

## Verification

```sh
test/run_tests.sh
SANITIZE=address test/run_tests.sh

# from src/dpl2
test/build_all.sh
SANITIZE=address test/build_all.sh
```

The 87 planner cases and E2E are GoogleTests. The test CMake selects fake UDM
only through `dpl2_test_udm` include/link settings; the same source graph can
use real UDM with:

```sh
cmake -S test -B test/build/real-udm \
  -DDPL2_TEST_USE_FAKE_UDM=OFF \
  -DDPL2_TEST_UDM_INCLUDE_DIRS='<real include dirs>' \
  -DDPL2_TEST_UDM_LIBRARIES='<real libraries or CMake targets>'
```

No production source has a fake UDM dependency or compile-time branch. The E2E
uses fake UDM as the test-data provider only; supplied infrastructure/checker
and production fillerRepair compile with `-Wall -Wextra -Werror`. Current
result: planner 87/87 and full CTest 88/88, normal+ASan.
