# Real-UDM fillerRepair tests

This directory is part of the migration payload. It contains one source for
all 52 production-chain E2E cases and the fixture contract implemented by the
destination's real UDM test environment.

`E2ETestProvider.h` is the only fixture boundary. A destination provider
creates the canonical five-row design with real UDM, exposes its existing
Grid/Network, and implements the two test operations (`moveCell` and
`snapshot`). No planner test double or UDM-compatible local test data lives in
this directory.

## Destination build

Compile the production chain and every E2E assertion against real UDM:

```sh
cmake -S fillerRepair/test -B build-real \
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
remain in `e2e_cases.cpp`.

The separate local regression copy, including all 81 fake-based planner unit
tests and the local UDM-compatible E2E provider, lives at
`src/dpl2/test/local/` and is not part of the migration payload.
