# fillerRepair2 — copy-only runtime payload

Updated: 2026-08-04.

Copy this directory's contents into the destination's existing
`src/dpl2/src/fillerRepair/`, add the directory, and link its target:

```cmake
add_subdirectory(<srcroot>/fillerRepair fillerRepair)
target_link_libraries(<owner> PRIVATE dpl2::fillerRepair)
```

The parent may define `dpl2_filler_repair_deps` to supply UDM,
infrastructure and checker targets. No fake UDM, fixture, standalone discovery,
or test source is present here.

Runtime API:

```cpp
FillerRepairEngine(Grid*, Network*);
bool init(PhysDesMgr*, const fillerSetting&);
RepairOutcome repair(const ipl::CheckRequest&);
```

Infrastructure must register configured filler masters and target replacement
masters in Network with its real edge table. The engine never calls
`Network::addMaster`; `ensureMasterRegistered()` only sets the existing
configured master's filler flag.

The eight runtime/API files are byte-identical to the corresponding files in
`../fillerRepair/`. The repository test CMake compares SHA-256 hashes at
configure time, so a source/mirror mismatch fails immediately.

The checker remains the caller-facing boundary and sole DRC authority. Repair
returns `ipl::FillerChanges` without mutating UDM, Grid, or Network placement;
opto/infrastructure owns commit.
