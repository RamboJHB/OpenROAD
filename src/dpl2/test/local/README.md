# Repository-local fillerRepair harness

Updated: 2026-08-06.

**Not part of the migration payload.** It exists so the feature can be
developed and gated with no real UDM present. It owns the UDM-compatible
include tree (`fake_udm/`), the 108-case engine regression and its
`E2ETestProvider`, and the runner scripts. No fake header or provider crosses
into `src/dpl2/src/fillerRepair/`.

The engine regression is the one target that cannot be built against a real
UDM: its data comes from `FakeUdmE2ETestProvider`, which constructs fake-UDM
objects directly. Everything else it links is the same runtime chain a
destination builds.

```sh
ALL=1 ./run_fake_udm_e2e.sh                     # whole suite, 278 cases
SANITIZE=address ALL=1 ./run_fake_udm_e2e.sh
./run_fake_udm_e2e.sh                           # engine regression only
./run_migration_gate.sh                         # destination code path, 170
SANITIZE=address ./run_migration_gate.sh
```

## The migration gate

`run_migration_gate.sh` builds the full `src/dpl2/src/fillerRepair/` package
with `DPL2_TEST_USE_FAKE_UDM=OFF` — no fake-only
target, no test provider — supplying the fake headers through the *real*-UDM
knob. That is deliberate: the point is not that the headers are real, it is
that this exercises the **destination code path** and proves every source
compiles and every executable's link closure is complete.

A static compile-check library cannot prove that. Archives do not resolve
symbols; only linking an executable does. This gate exists because a divergent
real-UDM CMake branch once quietly stopped linking `FillerRepairEngine`, and
nobody could see it without a real UDM.

Both local and migration configurations stage `fillerRepair2/` under the
destination directory name and strictly compile its runtime sources. Runtime
behavior remains exercised through the source-of-truth `fillerRepair/` tests.
