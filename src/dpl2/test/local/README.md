# Repository-local fillerRepair harness

This directory is intentionally not part of the migration payload. It owns the
UDM-compatible include tree, the 101-case `E2ETestProvider` suite and local
runner scripts.

The suite includes focused initialization coverage for non-uniform row-site
widths, integer-multiple mixed row heights and conflicting Node/physical
filler classifications. It verifies that mixed heights use the smallest base
height, Node/configured-list authority permits initialization, and precheck is
non-mutating.

The portable 82-case planner suite and its database-free doubles now live below
`src/dpl2/src/fillerRepair/test/` as same-level sources; `run_planner_tests.sh` is only a
convenience entry point. The runtime engine E2E source and its fake-UDM
provider remain here. No local fake/provider include crosses into the
migration payload.

```sh
src/dpl2/test/local/run_planner_tests.sh
SANITIZE=address src/dpl2/test/local/run_planner_tests.sh
src/dpl2/test/local/run_fake_udm_e2e.sh              # engine regression only
ALL=1 src/dpl2/test/local/run_fake_udm_e2e.sh        # whole suite, 203 cases
SANITIZE=address ALL=1 src/dpl2/test/local/run_fake_udm_e2e.sh
```

## Migration gate (no UDM required)

`run_migration_gate.sh` configures with `DPL2_TEST_USE_FAKE_UDM=OFF` -- the
destination code path -- and supplies the fake headers through the real-UDM
knob, so it runs with no UDM installed. Point `DPL2_UDM_INCLUDE_DIRS` /
`DPL2_UDM_LIBRARIES` at a genuine install to use one.

```sh
src/dpl2/test/local/run_migration_gate.sh            # 141 portable cases
SANITIZE=address src/dpl2/test/local/run_migration_gate.sh
```

Its value is exercising the destination configuration (no fake test provider,
no fake-only target), not the headers being real. It proves every supplied and
runtime source compiles and that the executable **link closure** is complete:
a source missing from a target surfaces as an undefined symbol. A static
compile-check library cannot prove this -- archives do not resolve symbols,
only linking an executable does.

Both UDM modes now build the same targets from the same sources, with
`dpl2_test_udm` as the only switch. Keeping two divergent branches is how the
real-UDM configuration silently stopped linking once the checker took
ownership of `FillerRepairEngine`: nothing local could build that branch.
Run this gate after any change to the target/source wiring.
