# Filler repair — dpl integration (multi-height)

Two packages, **one shared algorithm**:

```
                 ┌─────────────────────────────────────────┐
                 │  SHARED CORE  (algorithm, multi-height)  │
                 │  src/dpl/src/FillerRepair.{h,cpp}        │
                 │  + FillerGrid (abstract interface)       │
                 └───────────────┬───────────┬─────────────┘
                                 │           │
            ┌────────────────────┘           └────────────────────┐
   ┌────────▼─────────┐                              ┌─────────────▼────────┐
   │  dpl package      │                              │  portable package    │
   │  (this doc)       │                              │ (filler_repair_      │
   │  DplFillerGrid    │  ← real odb/dpl adapter      │  porting.md)         │
   │  + Tcl command    │                              │  FakeFillerGrid +    │
   │  + CMake/Bazel    │                              │  adapter template +  │
   │  + tests          │                              │  standalone tests    │
   └───────────────────┘                              └──────────────────────┘
```

The algorithm (`FillerRepair`) is identical for both; only the `FillerGrid`
implementation and the build/command wiring differ.

> Build status: **VERIFIED in a full OpenROAD build.** `openroad` was built
> from source and `repair_dirty_fillers` runs end-to-end on the test LEF/DEF:
> the dirty filler `dirtyF` is deleted and `FILLER_REPAIR_0_2_0` is created in
> its place, logging `[INFO DPL-0206] Filler repair: placed 1, unsolved 0.`
>
> Building OpenROAD in a network-restricted sandbox (git-clone of third-party
> deps is blocked, but direct `curl` of release tarballs works):
> - CUDD / Lemon / spdlog-1.15.0: `curl` the GitHub tarballs and build (the
>   stock `DependencyInstaller.sh` uses `git clone`, which is blocked).
> - or-tools: download the **prebuilt** release tarball (no source build).
> - GTest / yaml-cpp / OpenGL / readline / pcre2: `apt`.
> - Configure with `-DBUILD_GUI=OFF -DENABLE_TESTS=OFF` and the dep ROOTs.
> Gotcha: the apt `libfmt-dev` (fmt 9) is incompatible with `utl` — use the
> spdlog-1.15.0 bundled fmt (install spdlog 1.15.0 to /usr/local). odb headers
> need **C++20** (OpenROAD already builds C++20).

---

## 1. Files (dpl package)

| File | Role |
|---|---|
| `src/dpl/src/FillerRepair.{h,cpp}` | shared algorithm (multi-height) |
| `src/dpl/src/DplFillerGrid.{h,cpp}` | odb adapter: implements `FillerGrid`, builds the library, `repairDirtyFillers()` driver |
| `src/dpl/src/RepairDirtyFillers.cpp` | Tcl-command glue: resolve dirty inst names → `dbInst*`, call the driver |
| `src/dpl/test/repair_dirty_fillers.tcl` | integration test |
| `src/dpl/test/repair_dirty_fillers_data/{impl.lef,design.def}` | minimal implant LEF/DEF for the test |

The driver entry point:
```cpp
dpl_fr::RepairResult dpl_fr::repairDirtyFillers(
    odb::dbBlock* block,
    const std::set<odb::dbInst*>& dirty,   // upstream-marked dirty fillers
    const std::vector<odb::dbMaster*>& masters,  // user filler lib, in order
    bool preserve_user_order,
    int min_implant_width,
    utl::Logger* logger);
```

## 2. How the adapter maps OpenROAD → algorithm

| dpl/odb | → FillerGrid |
|---|---|
| `dbRow` + `dbSite` width/height | site/row geometry (sites/rows) |
| placed `dbInst`, not filler | `SiteKind::Cell`, vt = `implantVt(master)` |
| `CORE_SPACER` inst not in `dirty` | `SiteKind::CleanFiller` |
| `CORE_SPACER` inst in `dirty` | `SiteKind::DirtyFiller` |
| block master / fixed keep-out | `SiteKind::Blocked` |
| `getObstructions()` IMPLANT layer name | the VT id (mirrors `FillerPlacement::getImplant`) |
| `clearSite` | `dbInst::destroy(dirty inst)` |
| `placeFiller` | `dbInst::create(..physical_only)` + orient(row) + location |

**Dirty marking** is upstream (DRC). The command receives the dirty set; in
practice identify them from a `dbProperty`/marker the DRC step set, or by a
name/category filter — pass that set into `repairDirtyFillers`.

## 3. Wiring (apply in a build environment)

### CMake — `src/dpl/CMakeLists.txt`
Add to the `add_library(dpl_lib ...)` source list:
```cmake
  src/FillerRepair.cpp
  src/DplFillerGrid.cpp
  src/RepairDirtyFillers.cpp
```

### Bazel — `src/dpl/BUILD.bazel`
Add to the dpl library `srcs`/`hdrs`:
```python
    "src/FillerRepair.cpp", "src/FillerRepair.h",
    "src/DplFillerGrid.cpp", "src/DplFillerGrid.h",
    "src/RepairDirtyFillers.cpp",
```

### SWIG — `src/dpl/src/Opendp.i`
```swig
%{
#include "DplFillerGrid.h"
namespace dpl_fr {
dpl_fr::RepairResult repairDirtyFillersByName(
    odb::dbBlock*, const std::vector<odb::dbMaster*>&,
    const std::vector<std::string>&, bool, int, utl::Logger*);
}
%}
%inline %{
void repair_dirty_fillers_cmd(const std::vector<odb::dbMaster*>& masters,
                              const std::vector<std::string>& dirty,
                              bool preserve, int min_w) {
  auto* block = ord::getDb()->getChip()->getBlock();
  dpl_fr::repairDirtyFillersByName(block, masters, dirty, preserve, min_w,
                                   ord::OpenRoad::openRoad()->getLogger());
}
%}
```

### Tcl — `src/dpl/src/Opendp.tcl`
```tcl
sta::define_cmd_args "repair_dirty_fillers" {
  -masters filler_masters -dirty inst_names
  [-preserve_user_order] [-min_implant_width n] }
proc repair_dirty_fillers { args } {
  sta::parse_key_args "repair_dirty_fillers" args \
    keys {-masters -dirty -min_implant_width} flags {-preserve_user_order}
  set masters [dpl::get_masters_arg "-masters" $keys(-masters)]
  set dirty {}
  if { [info exists keys(-dirty)] } { set dirty $keys(-dirty) }
  set minw 1
  if { [info exists keys(-min_implant_width)] } { set minw $keys(-min_implant_width) }
  set preserve [info exists flags(-preserve_user_order)]
  dpl::repair_dirty_fillers_cmd $masters $dirty $preserve $minw
}
```
`-dirty` is the list of upstream-flagged dirty filler instance names. In a real
flow these come from the DRC step (property/category); the command just consumes
them.

## 4. Test — `src/dpl/test/repair_dirty_fillers.tcl`
Concrete, minimal integration test (this commit):
- `repair_dirty_fillers_data/impl.lef` — implant LEF: `core` site, `LVT`/`HVT`
  IMPLANT layers, a `CELL_L` cell and `FILL_L2/L4/L6` LVT fillers.
- `repair_dirty_fillers_data/design.def` — one 10-site row: `CELL_L | FILL_L6
  (dirty) | CELL_L`.
- `repair_dirty_fillers.tcl` — marks `dirtyF` dirty, runs
  `repair_dirty_fillers -masters {FILL_L6 FILL_L4 FILL_L2} -dirty {dirtyF}`,
  `check_placement`, writes/diffs the DEF.

Register in `src/dpl/test/CMakeLists.txt` `or_integration_tests` (and the Bazel
test list) like the existing `fillers*` tests; generate the golden `.defok` on
the first successful run.

> Not run in the sandbox: the command must be wired (§3) and OpenROAD built
> first. The **algorithm** is already covered stand-alone in `dpl2`
> (39/39, incl. multi-height), and the **adapter logic** by the odb-mock test
> (§4.1, 11/11); this test only exercises the real-odb command path.

### 4.1 Adapter logic-test against an odb mock (sandbox-runnable)
`DplFillerGrid` is also compiled and logic-tested without a full OpenROAD build,
against a minimal odb/utl mock (`src/dpl/test/filler_repair_mock/`):
```sh
g++ -std=c++17 -I src/dpl/src -I src/dpl/test/filler_repair_mock \
    src/dpl/src/DplFillerGrid.cpp src/dpl/src/FillerRepair.cpp \
    src/dpl/test/dpl_filler_grid_test.cpp -o /tmp/dpl_test && /tmp/dpl_test
# => DplFillerGrid mock test: 11 passed, 0 failed.
```
This proves the adapter is well-formed C++ and its logic is correct (grid build,
dirty delete, multi-height refill, master mapping, unsolvable).  The mock encodes
the *assumed* odb signatures, so real-odb signature correctness is still
confirmed only by building inside OpenROAD.

## 5. Checklist
- [ ] Add `FillerRepair.cpp` + `DplFillerGrid.cpp` to CMake **and** Bazel.
- [ ] SWIG: expose `repair_dirty_fillers_cmd`.
- [ ] Tcl proc `repair_dirty_fillers` (above).
- [ ] Decide how the dirty set is passed (property/marker/category).
- [ ] Integration test + golden; register CMake+Bazel.
- [ ] Build, run unit test (39/39) + integration test.
