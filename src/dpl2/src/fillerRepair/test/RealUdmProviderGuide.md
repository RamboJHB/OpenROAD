# Real UDM E2E provider contract

`e2e_cases.cpp` already contains every E2E call and assertion. A destination
adds one data-only implementation of `makeE2ETestProvider()` from
`E2ETestProvider.h`; it must not duplicate or wrap the test cases.

## Canonical design

- site width 1, row height 8, five standard rows, 20 sites per row;
- implant layers `VTL_N/P`, `VTH_N/P`, `VTS_N/P` with WIDTH 6 and SPACING 2;
- masters: `SL6` (VTL std width 6), `TL4` (VTL std width 4), `TH4` (VTH std
  width 4), and fillers `FL2/FH2/FS2` (width 2);
- `FX2` is an uninstantiated VTH filler used to verify init-time candidate
  registration;
- row 2 is `SL6 FL2 TL4 FL2 SL6`; `TL4` is the target;
- rows 0, 1, 3 and 4 are `SL6 SL6 SL6 FL2`;
- orientations alternate so the checker's expected bottom-band polarity is
  satisfied.

The provider maps these objects to `CellRole`/`MasterRole`; case code never
assumes numeric UDM ids.

## Required operations

`E2ETestDesign` exposes the real `Design`, `PhysDesMgr`, role-based cell/master
handles, row origins and standard-row count. `moveCell()` is used only to
construct precheck gap/overlap inputs. `snapshot()` returns stable tuples of
cell id, origin, master id, status and orientation so cases prove that
`precheck()` and `repair()` do not mutate UDM. `activate()` makes this design
the UDM Session current design.

`E2ETestInfrastructure` exposes the Grid/Network already initialized exactly
as DePlace initializes them. `row0TailHardBlockage` and `row0TailHaloWidth`
must affect that legal Grid domain. The provider never constructs an
ImplantLayerChecker or FillerRepairEngine; shared case code does that.

`DesignSetup` also requests shifted row origins, leading/trailing pad rows and
unused implant-rule layers. These variations drive all 52 cases.

## Build

Pass the implementation to portable CMake:

```sh
cmake -S fillerRepair/test -B build-real \
  -DDPL2_BUILD_REAL_UDM_CASES=ON \
  -DDPL2_REAL_UDM_INCLUDE_DIRS='<includes>' \
  -DDPL2_REAL_UDM_LIBRARIES='<targets>' \
  -DDPL2_REAL_UDM_PROVIDER_SOURCE='<RealUdmE2ETestProvider.cpp>'
cmake --build build-real
ctest --test-dir build-real -R '^real-udm\.'
```

The real provider is necessarily destination-owned because fillerRepair has no
authority over the real UDM design creation/loading and mutation APIs.
