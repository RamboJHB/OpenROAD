# Porting `FillerRepair` to another database

This is everything needed to move the DRC-driven dirty-filler repair onto a
different database (OpenROAD `dpl`, or any other DB). The algorithm is written
against an abstract grid interface, so **porting = implement one interface +
build a filler library + call one method**. The core logic does not change.

See also `docs/filler_insertion.md` for the requirement/spec.

---

## 1. What is portable vs what you implement

| Layer | File | Port effort |
|---|---|---|
| Repair algorithm (windows, exact-fill, VT continuity, multi-height) | `src/dpl/src/FillerRepair.{h,cpp}` | **none** — reuse as-is |
| Grid interface (the seam) | `FillerGrid` in `FillerRepair.h` | **implement** against your DB |
| In-memory reference backend (tests) | `src/dpl/src/FakeFillerGrid.h` | replace with your adapter |
| Adapter template (copy & fill) | `src/dpl/src/FillerGridAdapter.example.h` | **your starting point** |
| Stand-alone tests | `src/dpl/test/filler_repair_test.cpp` | keep / extend |

Everything in the core is in **sites / rows**, never DBU. Two things the core
deliberately does NOT model — the **adapter owns** them:
1. **Geometry**: `(row, col)` ↔ physical DBU, site width, row pitch/Y, and
   per-row orientation (R0/MX) so rails/implant line up.
2. **Masters**: map `(vt, width_sites, height_rows)` → your filler master, and
   the reverse (master → `{vt, width, height}`) to build the library.

---

## 2. Three steps to port

### Step 1 — implement `FillerGrid`
```cpp
class MyDbFillerGrid : public dpl_fr::FillerGrid { ... };
```
Six methods (full contract in `FillerRepair.h`):

| Method | Return / effect |
|---|---|
| `numRows()` | rows in the region |
| `numCols(row)` | sites in `row` |
| `kindAt(row,col)` | `Cell` / `CleanFiller` / `DirtyFiller` / `Blocked` / `Empty` |
| `vtAt(row,col)` | VT/implant id of the master there; `""` if none |
| `clearSite(row,col)` | delete the dirty filler at that site → `Empty` |
| `placeFiller(f)` | create **one** instance covering `f`'s block |

### Step 2 — build the filler library
```cpp
std::vector<Filler> lib;          // ORDER MATTERS (preserveUserOrder)
for (master : user_core_fillers)  // in the user-given order
  lib.push_back({widthInSites(master), heightInRows(master),
                 vtOf(master), master.name()});
```

### Step 3 — drive
```cpp
FillerRepair repair(lib, /*preserve_user_order=*/true, rules);
RepairResult r = repair.repair(grid);   // mutates the DB via the grid
//  r.placed   -> new fillers (already created through placeFiller)
//  r.unsolved -> windows filler can't fix; hand back to upstream
```

A copy-paste skeleton with all of this is in
`src/dpl/src/FillerGridAdapter.example.h`.

---

## 3. `SiteKind` classification (the only semantics you must get right)

| Your DB object at (row,col) | Map to |
|---|---|
| placed standard cell | `SiteKind::Cell` |
| filler **not** flagged dirty by upstream | `SiteKind::CleanFiller` |
| filler flagged **dirty** by upstream DRC step | `SiteKind::DirtyFiller` |
| macro / blockage / fixed keep-out / invalid row site | `SiteKind::Blocked` |
| legal free site | `SiteKind::Empty` |

Notes:
- **Dirty marking is upstream** (the DRC step). The adapter just reports it.
- `vtAt` only needs to be correct for `Cell` and `CleanFiller` (they form the
  fixed boundary VT the refill must stay continuous with). Reuse your implant
  lookup — in OpenROAD dpl that is `getImplant(master)`.

---

## 4. Invariants the algorithm relies on (so your adapter is safe)

- **Only-dirty**: the algorithm only ever `clearSite`s sites it saw as
  `DirtyFiller`, and only `placeFiller`s inside those just-cleared windows. It
  never touches `Cell` / `CleanFiller` / `Blocked`. Your adapter therefore never
  needs to protect fixed objects.
- **No overlap**: placements within a window are non-overlapping and exactly
  tile the window; `placeFiller` blocks never overlap each other.
- **Exact-fill (fitGap, fixed on)**: a window is either fully refilled or
  reported in `unsolved` — fillers are never left with an orphan residue gap.
- **Multi-height**: identical per-row dirty windows stacked in consecutive rows
  become one rectangular window; a height-`h` `placeFiller` spans `h` rows with
  one instance. Non-rectangular dirty regions fall back to per-row (height-1).

---

## 5. Geometry & master mapping (adapter responsibilities)

`placeFiller(const PlacedFiller& f)` gives you `{row, col, width, height, vt,
name}`. Turn it into an instance:
```
master = masterFor(f.vt, f.width, f.height);   // your reverse map
x      = regionXMin + f.col * siteWidth;
y      = rowY(f.row);
orient = orientOfRow(f.row);                    // R0 / MX alternation
makeInstance(master, x, y, orient, /*physical_only=*/true);
```
- Build `masterFor` from the same data you used for the library.
- `f.name` is the library master's name if you prefer to key on that.
- For multi-height (`f.height > 1`) the instance is one master that already
  spans `f.height` rows; `y` is the bottom row's Y.

---

## 6. Rules

```cpp
Rules rules;
rules.min_implant_width = N;   // min sites of a STAND-ALONE same-VT implant
                               // strip (one that does not merge into a same-VT
                               // neighbor). 1 == off.
```
Get `N` from your implant rule deck (convert the rule to sites). Spacing is
handled implicitly by keeping implant continuous with the fixed neighbors;
min-width binds only on isolated windows.

---

## 7. Build & test

### Now (sandbox / standalone, no OpenROAD build)
```sh
g++ -std=c++17 -I src/dpl/src \
  src/dpl/src/FillerRepair.cpp src/dpl/test/filler_repair_test.cpp \
  -o /tmp/fr_test && /tmp/fr_test
# => FillerRepair test: 39 checks passed, 0 failed.
```

### In-tree, when wiring into OpenROAD (apply when you have a full build)
- **CMake** (`src/dpl/CMakeLists.txt`): add `src/FillerRepair.cpp` to the dpl
  library sources. Expose a Tcl command via a new `.i` (SWIG) entry if you want
  `repair_dirty_fillers` callable from Tcl.
- **Bazel** (`src/dpl/BUILD.bazel`): add `src/FillerRepair.cpp` /
  `src/FillerRepair.h` to the `srcs`/`hdrs` of the dpl target (dual
  registration is mandatory — see CLAUDE.md).
- The C++ unit test can be added as a `cc_test` (Bazel) / gtest target; dpl's
  current `test/` is Tcl integration (`or_integration_tests`), so the
  stand-alone test stays a separate small target.

> Not done here on purpose: this sandbox cannot build full OpenROAD
> (no swig/bazel, heavy deps), so the CMake/Bazel/SWIG wiring is left as the
> snippets above to apply — and verify — in a real build environment.

---

## 8. Porting checklist

- [ ] Subclass `FillerGrid` (`MyDbFillerGrid`), all 6 methods.
- [ ] `kindAt` maps your objects per §3 (esp. dirty vs clean filler).
- [ ] `vtAt` returns implant/VT for Cell + CleanFiller.
- [ ] `clearSite` deletes the dirty instance; double-delete safe.
- [ ] `placeFiller` creates one instance, correct master/x/y/orient (§5).
- [ ] `buildLibrary` in user order; widths/heights in sites/rows.
- [ ] `Rules.min_implant_width` from your deck.
- [ ] Call `repair.repair(grid)`; route `unsolved` back to upstream.
- [ ] Register sources in CMake **and** Bazel; optional SWIG Tcl command.
- [ ] Re-run the stand-alone tests against your adapter (swap FakeFillerGrid).
