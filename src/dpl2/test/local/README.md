# Repository-local fillerRepair harness

This directory is intentionally not part of the migration payload. It owns the
complete local regression copy: all 81 planner unit cases and doubles, the
UDM-compatible include tree, the 64-case `E2ETestProvider` suite
and both runners.

The planner suite is self-contained below `local/planner/` and links only the
production planner sources. The historical production-facade E2E source and
its fake-UDM provider now both live here. The migration payload instead owns a
separate helper-built final-checker E2E, so no fake/provider include crosses
the directory boundary. Nothing below this directory is copied to production.

```sh
src/dpl2/test/local/run_planner_tests.sh
SANITIZE=address src/dpl2/test/local/run_planner_tests.sh
src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address src/dpl2/test/local/run_fake_udm_e2e.sh
```
