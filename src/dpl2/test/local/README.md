# Repository-local fillerRepair harness

This directory is intentionally not part of the migration payload. It owns the
UDM-compatible include tree, the 91-case `E2ETestProvider` suite and local
runner scripts.

The portable 82-case planner suite and its database-free doubles now live below
`src/dpl2/src/fillerRepair/test/` as same-level sources; `run_planner_tests.sh` is only a
convenience entry point. The runtime engine E2E source and its fake-UDM
provider remain here. No local fake/provider include crosses into the
migration payload.

```sh
src/dpl2/test/local/run_planner_tests.sh
SANITIZE=address src/dpl2/test/local/run_planner_tests.sh
src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address src/dpl2/test/local/run_fake_udm_e2e.sh
```
