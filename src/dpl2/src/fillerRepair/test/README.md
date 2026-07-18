# Portable fillerRepair tests

This directory is part of the migration payload. It contains one source for
all 81 UDM-free planner unit cases and one provider-neutral source for all 52
production-chain E2E cases.

The E2E assertions do not include fake UDM. `E2ETestProvider.h` is the only
fixture boundary. A destination real-UDM provider creates the canonical five-
row design, exposes its existing Grid/Network, and implements the two test
operations (`moveCell` and `snapshot`). The local fake provider implements the
same boundary outside this directory at `src/dpl2/test/local/`.

## Destination build

Unit tests need only GoogleTest:

```sh
cmake -S fillerRepair/test -B build-unit
cmake --build build-unit --target dpl2_filler_repair_unit_test
ctest --test-dir build-unit -R '^unit\.'
```

Compile the production chain and every E2E assertion against real UDM:

```sh
cmake -S fillerRepair/test -B build-real \
  -DDPL2_BUILD_REAL_UDM_CASES=ON \
  -DDPL2_REAL_UDM_INCLUDE_DIRS='<real UDM includes>' \
  -DDPL2_REAL_UDM_LIBRARIES='<real UDM targets/libraries>'
cmake --build build-real
```

To run the 52 E2E cases, also pass the destination's implementation of
`makeE2ETestProvider()`:

```sh
  -DDPL2_REAL_UDM_PROVIDER_SOURCE='<RealUdmE2ETestProvider.cpp>'
```

That provider is intentionally data-only. All engine calls and assertions
remain in `e2e_cases.cpp`, so the real and local runners cannot drift.
