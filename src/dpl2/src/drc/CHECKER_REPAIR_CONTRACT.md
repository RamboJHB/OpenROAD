# fillerRepair ↔ delivered code: contract and change list

Updated: 2026-07-29.

Everything fillerRepair needs from outside itself, and every edit it required
in code it does not own. Edits are tagged `[fillerRepair-fix]` in the source so
they can be found with one grep:

```sh
grep -rn "fillerRepair-fix" src/dpl2/src src/dpl2/include
```

Checker DRC rules, shapes, scan behaviour and blocking-violation logic are
**unchanged**. Nothing below alters what the checker decides.

---

## 1. Runtime contract

### Entry

```cpp
bool ImplantLayerChecker::check(const Node* node, GridX x, GridY y,
                                const PhysOrientation& orient,
                                std::vector<FillerCellRecord>& fcRecord) const;
```

The caller (opto) owns the vector. On a DRC-illegal candidate the checker
consults the repair engine; when a checker-verified swap set exists, `check()`
returns true and **appends** the records. The checker stores no filler-change
member — `getFillerChanges()`, `initFillerRepair()`, `updateFillerRepair()` and
`precheckFillerRepair()` do not exist.

### Overlay API (used by the engine's private oracle)

```cpp
std::vector<CheckResult> checkPlaceWithOverlays(
    const CheckRequest& target,
    const eUTL::Rect& guard,
    const std::vector<FillerChanges>& candidates) const;
```

One `FillerChanges` is one atomic candidate. Results correlate **by input
order**; result count must equal candidate count. A missing or extra result
invalidates the whole batch — no finding from a mis-correlated batch is
consumed.

### Lazy engine, and the two failure kinds

The engine is created on the first failing check. Context comes from the
checker's own `init()` (`PhysDesMgr`) plus either
`setFillerRepairContext()` (harnesses) or the provider registered through
`setFillerRepairSettingProvider()` (production — dependency inversion, so the
checker never names `DePlace`).

| | Meaning | Behaviour |
|---|---|---|
| **Not configured yet** | no `fillerSetting` / no `PhysDesMgr` | **Retryable.** With lazy init the first failing check can legitimately precede `set_filler_option`. One `[fr]` notice, then the next failing check tries again. Latching this would silently disable repair for the rest of the run once configuration did arrive. |
| **Structural init failure** | `FillerRepairEngine::init()` returned false | **Permanent** for that checker, until `setFillerRepairContext()` sets a new context. Retrying would fail identically. |

### IDs and wire

- checker `InstanceId` = `Node::getId()`; `MasterId` = `Master::getId()`;
- physical handles: `LeafCellID` / `LibCellID`;
- shared record (owned by `infrastructure/Objects.h`):
  `FillerCellRecord{Replace, cell_id_, origin_x_, origin_y_, orig_lib_cell_,
  new_lib_cell_}`; `ipl::FillerChanges` is the checker-side vector alias.

Planner requests, checker calls and the public result all carry that **same**
record — there is no second representation to drift.

### Persistent init diagnostics

`getDiags()` entries are copied into every result — once by
`checkPlaceWithOverlay` and again inside the embedded `checkOverlayRegion`
result — and folded into `isLegal`. The engine classifies that sequence at
init: `skipped_phys_status`, and missing rule parameters on implant layers no
Network master uses, are non-blocking; anything else makes `init()` fail
closed. After init succeeds the engine strips *every* leading repetition of the
approved sequence from each result. A count-based single-prefix strip is wrong.

---

## 2. Changes to delivered code

### `drc/ImplantLayerChecker.h` / `.cpp`

1. **Repair wiring in `check()`** — the reserved block now calls the owned
   engine with the exact `CheckRequest` that method already built, and appends
   into the caller's `fcRecord`. Members added: `desMgr_`, `repairSetting_`,
   `repairEngine_`, `repairEngineFailed_`, `repairUnconfiguredReported_`, plus
   `setFillerRepairContext()` and the static
   `setFillerRepairSettingProvider()`.

2. **Explicit-design constructor**
   `ImplantLayerChecker(Grid*, Network*, PhysDesMgr*)` — additive. The
   two-argument form takes its design from the global `Session`; fillerRepair
   owns a private oracle checker and already knows the `PhysDesMgr` its engine
   was initialized with, so taking it from `Session` made that oracle depend on
   global state it does not control. Same initialization otherwise.

3. **`checkDirect()` extends `masterItems_` lazily** when the request master
   was registered in Network after checker init — the
   `DePlace::isLegal` addMaster-then-check flow. Both builders are idempotent
   by master id.

4. **`const char[N]` diagnostics** — string literals bound to a `std::string&`
   parameter did not compile; the affected declarations take `const char*`.

### `drc/ImplantLayerCheckerHelper.cpp`

Follows the checker's own rename/removal (`groups_` → the current members;
`buildRules()` reads the members set above). Mechanical, no behaviour change.

### `drc/DRCChecker.h`

Five-argument repair-aware `check()` virtual, defaulting to the four-argument
behaviour and ignoring `fcRecord`, so checkers that cannot repair are
unaffected.

### `infrastructure/Objects.h`

**One filler authority**: `dpl2::isFillerMaster(const PhysLibCell&)`.
`Network::addNode` classifies nodes with it, `Node::isFiller()` and
`Master::isFiller()` report it. fillerRepair asks those rather than
re-deriving anything from UDM macro flags. `fillerSetting` stays a separate
concept — which filler masters may be *offered* as replacements.

### `infrastructure/Grid.h` / `.cpp`

- `gridXY(const Node*)` — the `(column, row)` pair, a composition of
  `gridX`/`gridSnapDownY` so it is always in the frame those two use.
  `ImplantLayerChecker::getSnapshot` needs it.
- `getBoundingBox(const Rect& region, int rings = 3)` — expands a rect by
  whole cells, counting distinct occupants outward. Standard cells are ring
  members like any other and never stop the walk. Exposed on `DePlace` as
  `Rect getBoundingBox(Rect, int rings = 3) const`.
- `#include <tbb/task_arena.h>` — `tbb::task_arena` was used without it.
- `isFullUtil()` — see below; the fix is mostly in `DePlace`, but two defects
  were in this function. It now iterates the **logical** grid
  (`row_count_` x `row_site_count_`) rather than the pixel vector's extent:
  `allocateGrid()` only resizes when `pixels_` is empty and resets exactly the
  logical range, so re-initializing onto a smaller design left stale rows that
  nothing else could reach — `gridPixel()` bounds-checks, so no cell is ever
  painted there — and scanning them made the answer depend on a previous
  design. An empty grid now returns **false**: it used to warn and return
  true, claiming a design is full at the one moment it knows nothing. The
  false path names the first empty site so the next report is diagnosable.

### `DePlace.cpp`

**`setFixedGridCells` / `setPlacedGridCells` painted only
`getType() == Node::CELL`**, which drops every `Node::FILLER` —
`Network::addNode` types core fillers that way. Fillers are exactly what makes
a design full, so on a fully filled design their sites stayed empty in the
grid and `Grid::isFullUtil()` reported false. Anything else reading pixel
occupancy (legality checks, gap detection, `getBoundingBox`) saw the same
holes.

Both loops now share one `paintGridCell(Node*)` and select with
`!cell->isTerminal()` — "does this node stand on sites", not "is it a standard
cell". Asking that rather than listing `CELL || FILLER` also keeps grid
occupancy independent of the filler/non-filler classification, which
`updateNode` does not refresh after a master swap (open item below).

### `infrastructure/network.cpp`, `Object.cpp`

`Object.cpp` routes `Master::isFiller()` through the shared predicate above.

`network.cpp`: **`updateNode` restores `setOrient(inst.getOrient())`**, which
the 2026-07-28 destination update replaced with a hard-coded
`PhysOrientationE::R0`. `DePlace::isLegal` calls `updateNode` immediately
before `checkDRC`, so forcing R0 makes every implant check on an MX-placed row
— odd rows, by the band-polarity model — evaluate the wrong band track. It
also outlives the check: `isLegal` restores the master afterwards but not the
orientation, so the Node keeps a wrong orientation in shared Network state.

Three earlier `network.h`/`.cpp` compile fixes (`id` → `idx`,
`unique_ptr<Node*>` / `inst_to_node_idx__`, `clearEdgeS`) are **no longer
needed** — the destination update fixed them upstream.

### Open items on the destination side

Not patched here; they belong to the integration owner.

| | |
|---|---|
| `network.h` shipped with line numbers pasted into every line, so it is not valid C++ | fixed locally; confirm the source file |
| `Network::addNode` classifies with `isCoreFiller()` alone while `Master::isFiller()` is `isCoreFiller() \|\| isPadFiller()` | the two disagree on pad fillers — pick one predicate |
| `updateNode` no longer sets the node type | stale after a master swap that changes filler-ness |
| `network.h` dropped `#include <memory>` while still using `std::unique_ptr` | currently resolves transitively |
| `PlacementDRC.h` includes `<dpl2/DRCChecker.h>`; the header is at `drc/DRCChecker.h` | does not compile as shipped |
| `PlacementDRC.h` declares `const eUNL::PhysOrientation&`; `DRCChecker` and `ImplantLayerChecker` use `eUTL::PhysOrientation` | namespace mismatch on the call into our checker |
| `initPlacementDRC()` is declared but never defined, `drc_engine_` is never constructed, and nothing calls `PlacementDRC::addChecker` | `DePlace::isLegal` cannot reach any checker; with an empty `checkers_` it would report every candidate legal |
| `DRCCheckerType` has no implant entry (`EdgeSpacing`, `BlockedLayers`, `Padding`, `OneSiteGap`) | no key to register `ImplantLayerChecker` under |

### What fillerRepair adapted to, without patching

`Network::addMaster` lost its two-argument overload and its third parameter
became `const EdgeTypeTable*`. Our three call sites pass an **empty table**:
`EdgeTypeTable` lives in `Objects.h`, which the repair-only link target
already has, so the reason the overload existed (keeping `PlacementDRC` out of
that target) is gone. `addMaster` dereferences the table before any null
check, so `nullptr` is not an option; an empty one returns right after the
geometry the implant oracle reads. This is a fallback path — on the production
route `DePlace` registers the master WITH the real edge table before `check()`
runs, so a Master decorated by us never reaches placement DRC.

### `DePlace`

Registers the setting provider in its constructor, and forwards
`getBoundingBox`. It is the only place that knows both `DePlace` and the
checker.

---

## 3. Row/column frames — why the envelope exists

The checker uses TWO frames internally: `init(desMgr)` and the track pattern
index rows by PhysRow **iteration** order (pad rows included) with x relative
to the row origin, while `scanOverlaySnapshot` resolves neighbours and swapped
fillers through `Grid::gridSnapDownY`/`gridX` (non-pad rows sorted by y, x
relative to the core edge). `CheckRequest.rowId/colId` must be supplied in the
iteration frame — it feeds the track pattern and the footprint index.

The chain is consistent only when both frames coincide for every placed node:
no pad row before a standard row, y-sorted row iteration, and the shared row
origin X equal to the core left edge. `FillerRepairEngine::init()` validates
this per node (`RowFrameMismatch` / `ColFrameMismatch` are Fatal), so a design
outside the envelope fails loudly instead of being checked in mixed frames.

---

## 4. Rule reach: one authority

`getMaxRuleValue()` is the radius, in sites, of the neighbourhood
`getSnapshot` scans. The engine sizes its guard from that number alone and
never re-derives reach from raw `TechLayer` width/spacing.

This matters in one direction only: a guard **narrower** than the reach
truncates the checker's snapshot, and a truncated run looks too narrow — the
checker then reports a min-width violation that does not exist. A wider guard
can only remove such artifacts. Two independent derivations of the same
quantity is how that bug arrived once already, so there is now exactly one.

---

## 5. Shared-state requirements

`PhysDesMgr`, `Grid`, `Network` and one engine describe one design revision.
Network must contain every placed/fixed physical instance that can intersect
the core, hard macros included; placement blockages remain Grid state and are
not Network Nodes. The engine borrows the initialized Grid/Network, registers
all `fillerSetting::getFillerPhysCells()` candidates, then constructs its
oracle checker with the supplied `PhysDesMgr`. Calls on one checker/engine pair
must not overlap.

The engine may still receive `replacement_master_not_filler` for a candidate
the checker's own metadata rejects. It treats that as blocking and returns no
partial repair; it never reinterprets or bypasses checker legality.

---

## 6. Verified boundary

85 portable planner cases and 75 portable real-checker cases build, link and
run in **both** harness modes — fake-UDM and the destination-shaped migration
gate (160/160, normal and ASan). Repository-local fake-UDM engine regression:
81 cases. Full local suite 241/241, normal and ASan.

The fixture invariants the real-checker cases depend on — rule and layer ids as
container indices, the band-polarity model, the `maxRuleValue_`-sized snapshot
window, what `xWindow` means per rule kind, and min width applying to every run
— are documented in `../fillerRepair/README.md`.
