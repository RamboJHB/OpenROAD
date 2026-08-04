# Portable fillerRepair tests

Updated: 2026-08-04.

These tests travel with the full `fillerRepair/` verification directory. They
construct no UDM object and do not include the repository's fake-UDM headers.

## 93 planner tests

`RepairPlannerTest.cpp` supplies in-memory implementations of `PlacementView`
and `RepairOracle`. Coverage includes:

- request, master, filler and swap validation;
- L0 construction, exact checker-reach guard, quantization and adaptive growth;
- sparse filler density, blocking-participant seeding and opposite-direction
  fallback;
- per-filler domains, ranking, subset enumeration and exact completeness;
- zero/finite window and repair budgets, including invalid batch size;
- ordered batch protocol, canonical cache and determinism;
- baseline reproduction, multiset delta matching and new-violation filtering;
- safe empty failure with no partial changes.

The planner target is compiled independently at C++17. This catches accidental
runtime-layer dependencies even though the complete module uses C++20.

## 83 real-checker tests

`FillerRepairCheckerE2ETest.cpp` drives the actual
`ImplantLayerChecker` over helper-built `ImplantInput`, Grid and Network data.
The tests cover:

- clean, width and spacing results on both implant bands;
- overlay order/count, malformed records and baseline-delta behavior;
- same-size filler-only repairs, one/two/three-swap solutions and budgets;
- target request row/column/orientation and full-footprint bounds;
- checker reach including minimum value, PRL and LENGTH;
- mixed implant-family master rejection;
- `PlacementDRC` record staging: a later checker failure publishes no earlier
  repair changes;
- 50:50, 30:70, 20:80, 10:90 and 5:95 local filler-density matrices.

The fixture starts legal, alternates R0/MX rows, and keeps rule/layer/master ids
aligned with their container indices. A scenario changes one standard-cell VT;
the bridge filler is the expected one-swap solution unless the case explicitly
constructs a larger atomic repair.

## 111 fake-UDM chain tests

These remain in `src/dpl2/test/local/e2e_cases.cpp` and do not travel with the
payload. They verify the engine against real infrastructure/checker sources
while fake UDM only supplies data. Important boundary cases include:

- configured and target masters registered by test infrastructure before
  engine init; the engine only refreshes `setFiller(true)`;
- missing registered masters fail closed without Network mutation;
- mixed-height rows use the smallest base height;
- legal row segments exclude blockage/halo whitespace;
- committed UDM master wins over a transient Node candidate;
- repeated repair, snapshot refresh by reconstruction, and full database
  non-mutation;
- normal checker entry appends shared `CellChangeRecord` values.

## Commands

```sh
ALL=1 src/dpl2/test/local/run_fake_udm_e2e.sh
SANITIZE=address ALL=1 src/dpl2/test/local/run_fake_udm_e2e.sh
src/dpl2/test/local/run_migration_gate.sh
SANITIZE=address src/dpl2/test/local/run_migration_gate.sh
```

Current local total is 287/287. All local targets use
`-Wall -Wextra -Werror`; ASan is applied through the common dependency target.
