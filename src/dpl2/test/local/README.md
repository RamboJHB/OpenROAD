# Repository-local fillerRepair harness

This directory is intentionally not part of the migration payload. It owns the
only fake UDM include tree, the fake `E2ETestProvider` and local runners.

Both runners consume the portable sources under `src/fillerRepair/test`; no
unit or E2E case is copied here.

```sh
src/dpl2/test/local/run_planner_tests.sh
SANITIZE=address src/dpl2/test/local/run_planner_tests.sh
src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address src/dpl2/test/local/run_fake_udm_e2e.sh
```
