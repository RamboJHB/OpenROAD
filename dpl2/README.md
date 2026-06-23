# dpl2 — portable filler-repair (migration bundle)

Self-contained copy of the DRC-driven dirty-filler repair, ready to **migrate to
another database**. It shares the **same algorithm** as the OpenROAD `src/dpl`
integration (`FillerRepair`); the two differ only in the `FillerGrid`
implementation.

```
dpl2/
├── src/
│   ├── FillerRepair.{h,cpp}        # the algorithm (DB-agnostic, multi-height)
│   ├── FakeFillerGrid.h            # in-memory reference impl of FillerGrid
│   └── FillerGridAdapter.example.h # adapter template — copy & fill for your DB
└── test/
    └── filler_repair_test.cpp      # stand-alone unit test (39/39)
```

## Build & test (no dependencies)
```sh
g++ -std=c++17 -I dpl2/src dpl2/src/FillerRepair.cpp \
    dpl2/test/filler_repair_test.cpp -o /tmp/dpl2_test && /tmp/dpl2_test
# => FillerRepair test: 39 checks passed, 0 failed.
```

## What to migrate
Copy this whole `dpl2/` directory into your project. Then **implement one
interface** — `FillerGrid` in `src/FillerRepair.h` — against your database:

| Method | Meaning |
|---|---|
| `numRows() / numCols(row)` | grid size (rows / sites) |
| `kindAt(row,col)` | `Cell` / `CleanFiller` / `DirtyFiller` / `Blocked` / `Empty` |
| `vtAt(row,col)` | VT / implant id (for Cell & CleanFiller) |
| `clearSite(row,col)` | delete the dirty filler there → Empty |
| `placeFiller(f)` | create one instance over f's block |

Start from `src/FillerGridAdapter.example.h` (copy, rename, fill the TODOs).
`FakeFillerGrid.h` is the in-memory reference if you want to see a complete
implementation.

The algorithm (window detection, exact-fill / fitGap, VT continuity, true
multi-height) is **not** touched during migration.

## Scope (same as the spec)
- Input: a grid whose violating fillers are already marked DIRTY (the DRC step
  is upstream and not part of this code).
- Action: delete dirty fillers, refill their footprint with correct fillers,
  fixing intra-row spacing / min-width by restoring implant continuity.
- Strict "only-dirty"; exact-fill (no orphan residue); multi-height supported.
- Inter-row MW/MS is Phase II (see ../docs/filler_insertion.md §7).

Full step-by-step, contract, and checklist: `../docs/filler_repair_porting.md`.
The OpenROAD-side integration (real odb adapter, Tcl command, build wiring):
`../docs/filler_repair_dpl.md`.
