# Repository-local fillerRepair harness

This directory is intentionally not part of the migration payload. It owns the
complete local regression copy: all 81 planner unit cases and doubles, the
UDM-compatible include tree, the local `E2ETestProvider` and both runners.

The planner suite is self-contained below `local/planner/` and links only the
production planner sources. The local production-chain E2E runner reuses the
real-UDM assertion source under `fillerRepair/test` but supplies its local data
provider at link time. Nothing below this directory is copied to production.

```sh
src/dpl2/test/local/run_planner_tests.sh
SANITIZE=address src/dpl2/test/local/run_planner_tests.sh
src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address src/dpl2/test/local/run_fake_udm_e2e.sh
```
