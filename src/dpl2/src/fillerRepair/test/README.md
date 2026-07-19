# Portable fillerRepair + checker E2E

This directory moves with `fillerRepair/`. It contains one self-contained
GoogleTest source that builds placement, masters, implant layers and rules
through the final checker's `ImplantLayerCheckerHelper`; it does not read
DEF/LEF and does not require a destination-specific UDM fixture/provider.

`FillerRepairCheckerE2ETest.cpp` has 33 cases:

- four final-checker overlay contract cases (intra/inter-row WIDTH/SPACING);
- 17 end-to-end cases that pass checker snapshots to
  `internal::FillerRepairPlanner`, apply its returned overlay to the final
  checker, and cover clean/repair/failure, deterministic batching, candidate
  and budget boundaries, third-VT reachability, baseline consistency,
  same-size changes, new-violation rejection and multi-swap minimum width;
- 12 UDM-free internal precheck cases for exact coverage, leading/middle/
  trailing gaps, overlaps, legal holes, clipping, multiple rows, empty spans
  and deterministic mixed diagnostics, plus empty placement, unordered input,
  triple overlap and touching legal spans.

The test uses no fake checker, fake placement view or fake UDM data. The small
`PortablePlacementView` in the source is only the planner projection of the
same `ImplantInput` owned by `ImplantLayerCheckerHelper`.

## Destination CMake

The standalone CMake works when given the destination UDM includes/targets:

```sh
cmake -S fillerRepair/test -B build-filler-repair-e2e \
  -DDPL2_UDM_INCLUDE_DIRS='<real UDM includes>' \
  -DDPL2_UDM_LIBRARIES='<real UDM targets/libraries>'
cmake --build build-filler-repair-e2e
ctest --test-dir build-filler-repair-e2e --output-on-failure
```

If the destination already has an owning dpl2/checker library, its parent
CMake only needs to include `fillerRepair/sources.cmake`, compile
`${DPL2_FILLER_REPAIR_PLANNER_SOURCES}` plus
`${DPL2_FILLER_REPAIR_PORTABLE_E2E_SOURCE}` and
`drc/ImplantLayerCheckerHelper.cpp`, then link that owning library and
`GTest::gtest_main`.

All fake/checker-double tests and the repository-local fake-UDM 64-case regression
remain outside the migration payload under `src/dpl2/test/local/`.
