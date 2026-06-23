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

> Build note: the shared core + portable package are unit-tested stand-alone
> (`g++`, 39/39). The dpl adapter below depends on odb/utl and is compiled
> **inside the OpenROAD build** — the snippets here are written against the dpl
> APIs used by `FillerPlacement.cpp` but must be compiled/verified in a full
> build environment.

---

## 1. Files (dpl package)

| File | Role |
|---|---|
| `src/dpl/src/FillerRepair.{h,cpp}` | shared algorithm (multi-height) |
| `src/dpl/src/DplFillerGrid.{h,cpp}` | odb adapter: implements `FillerGrid`, builds the library, `repairDirtyFillers()` driver |
| `src/dpl/test/repair_dirty_fillers.tcl` | integration test |

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
```

### Bazel — `src/dpl/BUILD.bazel`
Add to the dpl library `srcs`/`hdrs`:
```python
    "src/FillerRepair.cpp",
    "src/FillerRepair.h",
    "src/DplFillerGrid.cpp",
    "src/DplFillerGrid.h",
```

### SWIG — `src/dpl/src/Opendp.i`
Expose the driver (or a thin Opendp method that calls it):
```swig
%{
#include "DplFillerGrid.h"
%}
// ... wrap a helper that collects -masters and the dirty set, then calls
// dpl_fr::repairDirtyFillers(...).
```

### Tcl — `src/dpl/src/Opendp.tcl`
```tcl
sta::define_cmd_args "repair_dirty_fillers" {
  -masters filler_masters [-preserve_user_order] [-min_implant_width n] }
proc repair_dirty_fillers { args } {
  sta::parse_key_args "repair_dirty_fillers" args \
    keys {-masters -min_implant_width} flags {-preserve_user_order}
  set masters [dpl::get_masters_arg "-masters" $keys(-masters)]
  set minw 1
  if { [info exists keys(-min_implant_width)] } { set minw $keys(-min_implant_width) }
  set preserve [info exists flags(-preserve_user_order)]
  # dirty set comes from the upstream DRC marking (property/category)
  dpl::repair_dirty_fillers_cmd $masters $preserve $minw
}
```

## 4. Test — `src/dpl/test/repair_dirty_fillers.tcl`
A small LEF/DEF with multi-VT implant fillers; mark a few as dirty; run the
command; check the placed/unsolved counts and that no dirty filler remains.
Register in `src/dpl/test/CMakeLists.txt` `or_integration_tests` list (and the
Bazel test list) like the existing `fillers*` tests, with a golden `.ok`.

The **algorithm** itself is already covered by the stand-alone unit test
(`src/dpl/test/filler_repair_test.cpp`, 39/39, incl. multi-height MH1–MH5);
the dpl integration test only needs to exercise the odb adapter wiring.

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
