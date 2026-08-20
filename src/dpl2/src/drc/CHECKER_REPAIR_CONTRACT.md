# fillerRepair ↔ delivered code: contract and change list

Updated: 2026-08-18. Working branch: `codex/filler-repair-hardening`.

Everything fillerRepair needs from outside itself, and every edit it required
in code it does not own. Edits are tagged `[fillerRepair-fix]` in the source so
they can be found with one grep:

```sh
grep -rn "fillerRepair-fix" src/dpl2/src src/dpl2/include
```

Two greps, two different questions. This one finds **edits we made to code we
do not own** — every one of them must travel or the payload does not build.
The other, `[PORT-` inside `fillerRepair/`, finds **decisions the destination
has to make** about code we do own; see `HandOff.md` §8.

Checker DRC rules, shapes, scan behaviour and blocking-violation logic are
**unchanged**. Nothing below alters what the checker decides.

The four checker sources `DRCChecker.h`, `ImplantLayerChecker.h`,
`ImplantLayerChecker.cpp`, and `ImplantLayerCheckerHelper.cpp` use a
formatting-only 120-column pass so declarations, calls, conditions, and short
enums stay together when readable. Includes and comments are not reordered or
reflowed. A whitespace-stripped content hash was compared before and after the
pass; all four files retained exactly the same non-whitespace content.

---

## 1. Runtime contract

### Entry

```cpp
bool ImplantLayerChecker::check(const Node* node, GridX x, GridY y,
                                const PhysOrientation& orient,
                                std::vector<CellChangeRecord>& fcRecord) const;
```

The caller (opto) owns the vector. On a DRC-illegal candidate the checker
consults the repair engine; when a checker-verified swap set exists, `check()`
returns true and **appends** the records. The checker stores no filler-change
member — `getFillerChanges()`, `initFillerRepair()`, `updateFillerRepair()` and
`precheckFillerRepair()` do not exist.

The same `check()` entry supports std-to-std and filler-to-std. For a
non-mutating proposal, the temporary Node supplies the same-footprint
standard-cell master and orientation but is not inserted into Network and does
not reuse the committed instance ID. The checker maps it to the unique
committed std cell or filler exactly covering the requested footprint, then
`checkDirect()` excludes that resolved `CheckRequest::instanceId` from the
snapshot without rewriting Network or UDM. Ambiguous, partial, or empty
coverage fails closed. A filler target is also excluded from planner
candidates, and successful output contains only surrounding filler `Replace`
records.

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

### Lazy engine and initialization failures

The engine is created on the first failing check. The checker reads
`PhysDesMgr` only from the manager retained by Grid.
The filler setting arrives through either
`setFillerRepairContext()` (harnesses) or the provider registered through
`setFillerRepairSettingProvider()` (production — dependency inversion, so the
checker never names `DePlace`).

`set_filler_option` and checker initialization both precede repair. Missing
`fillerSetting` or `PhysDesMgr` is therefore an integration error, not a
retryable state. Missing context and structural
`FillerRepairEngine::init()` failures both disable repair for that checker.
`setFillerRepairContext()` is the explicit reset path used by harnesses and
integrations that replace the context.

### IDs and wire

- checker `InstanceId` = `Node::getId()`; `MasterId` = `Master::getId()`;
- physical handles: `LeafCellID` / `LibCellID`;
- shared record (owned by `infrastructure/Objects.h`):
  `CellChangeRecord{Replace, CellData{LeafCellID}, x_, y_,
  orig_lib_cell_, new_lib_cell_, orientation_}`; `ipl::FillerChanges` is the
  checker-side vector alias. `CellData` may also carry a `std::string` for a
  future named `Add`, but the current swap-only overlay rejects anything other
  than `Replace + LeafCellID`.

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
   into the caller's `fcRecord`. Members added: `desMgr_`,
   `repairSetting_`, `repairEngine_`, `repairEngineFailed_`, plus
   `setFillerRepairContext()` and the static
   `setFillerRepairSettingProvider()`.

2. **Grid-bound design context**
   `ImplantLayerChecker(Grid*, Network*)` is the only constructor. It obtains
   `PhysDesMgr` from `Grid::getDesMgr()` and fails closed when Grid, Network or
   the Grid manager is absent. `FillerRepairEngine::init()` still verifies the
   `fillerSetting` Design manager, explicit manager and Grid manager agree
   before creating its private checker. No global design state is consulted.

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

**One filler authority**: `fillerSetting::isFillerCell(LibCellID)`, which checks
membership in its configured `core_` list. `Network::addMaster` stores that
answer on `Master`; `addNode` and `updateNode` inherit the Master type.
`set_filler_option` runs before filler placement, checker initialization and
repair initialization, so classification is established as objects enter
infrastructure. `Node::isFiller()` and `Master::isFiller()` are the only
downstream queries. UDM macro filler flags are not classification inputs.

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
occupancy independent of the filler/non-filler distinction.

### `infrastructure/network.cpp`, `Object.cpp`

`Object.cpp` returns the filler type stored on `Master`, and
**`Node::isStdCell()` now excludes fillers through `Node::isFiller()`**.
`PhysMacroType::isCore()` is
true for `CORE_FILLER`, so it used to answer "yes, a standard cell" for every
filler in the design — the mirror image of the `DePlace` bug and the same
confusion: *is a standard cell* is not *stands on a site*. Ask `!isTerminal()`
for the latter. Behaviour change in delivered code: grep the destination for
other callers.

`network.cpp`, three fixes beyond the orientation one below:

- **`addMaster` classifies only with `fillerSetting::isFillerCell()`**.
  `set_filler_option` has already configured the core list before filler
  placement, and `addNode` inherits the stored Master type.
- **`updateNode` sets the type.** It refreshed master, size, orientation and
  status but never the type, so a swap that changes filler-ness left
  `Node::isFiller()` answering about the previous master.
  `ImplantLayerChecker::checkOverlap` uses that to decide whether an occupant
  is an excludable filler or a hard `placement_overlap_in_input`, and
  `validateOverlayRequest` uses it to accept a changed instance at all — both
  now reachable, because the grid finally carries filler occupants.
- **`updateNode` refuses an unregistered master.** `getMaster()` returns
  nullptr for one, and that nullptr was stored and then dereferenced a few
  lines later (`getBottomPowerType`): the node was left half-updated and the
  process died. It now returns `false` — which is what the `bool` return was
  always for — and leaves the node untouched.
- **Network import fails before mutation on incomplete inputs.** `addNode`
  now returns `false` for a missing manager, invalid physical cell, or
  unregistered master; `updateNode` also rejects a null node/manager or
  invalid physical mapping. The owned-object overloads reject null
  `unique_ptr`s, and `addMaster` rejects missing Grid/edge-table dependencies.

The shared runtime boundaries are also fail-closed: Grid row import tolerates
a missing manager, cleared/unallocated pixel storage is bounds-checked,
checker target/overlay requests reject missing nodes and masters, engine
snapshot construction treats null Network slots/mappings as fatal, and the
planner rejects an empty view or unknown target before window construction.
These checks do not change rule evaluation, candidate generation, ranking, or
the accepted repair wire.

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
| `network.h` dropped `#include <memory>` while still using `std::unique_ptr` | currently resolves transitively |
| **`PhysObjStatus` may carry values beyond `PLACED` / `LOC_FIXED`** | `addNode` derives `isPlaced()`/`isFixed()` from those two alone, and `setFixedGridCells`/`setPlacedGridCells` paint only nodes matching one of them. If the real enum has a third "placed and immovable" value (a DEF `COVER`, say), instances carrying it are painted by neither loop — the same hole as the filler one, on the status axis. The stand-in UDM here has only the three values, so this cannot be settled in this repository. **Confirm the real enum.** |
| `Grid::visitCellPixels` and `Grid::paintPixel` disagree on obstruction-bearing masters | `visitCellPixels` (DePlace's initial paint) paints only OVERLAP-layer obstruction rects when the master has any, while `paintPixel` (every later repaint) always paints the whole footprint. A master whose obstruction is smaller than its outline therefore has different occupancy depending on which path last touched it, and leaves unpainted sites at init. Deliberate mechanism for macros, so not changed here |
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

The retained post-baseline helper
`DePlace::registerFillerRepairMasters()` uses DePlace's real Grid and edge
table to register every master in the active fillerSetting. It then refreshes
already-imported matching Nodes to `Node::FILLER`. The current
`test_filler_repair` calls this once before constructing its checker; the
planner and engine algorithms remain those from `944ce7ba66`.

`include/dpl2/DePlace.h` now forward-declares `Pixel`, `GridPt`, `GridRect`,
`DbuPt`, and `DbuRect` as `struct`, matching their infrastructure definitions.
This is declaration-only: it removes Clang `-Wmismatched-tags` failures under
`-Werror` and does not alter layout or runtime behavior.

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

`fillerSetting` Design, `PhysDesMgr`, `Grid`, `Network` and one engine describe
one design revision.
Network must contain every placed/fixed physical instance that can intersect
the core, hard macros included; placement blockages remain Grid state and are
not Network Nodes. The engine borrows the initialized Grid/Network, registers
all `fillerSetting::getFillerPhysCells()` candidates, verifies the setting's
manager equals the supplied manager and `Grid::getDesMgr()`, then constructs
its private checker from Grid/Network. Registration is not skipped for an
existing master:
`Network::addMaster(..., fillerSetting, ...)` must refresh
`Master::isFiller` before checker construction. No Session fallback is
allowed. Calls on one checker/engine pair must not overlap.

Initialization builds a compatibility catalog keyed by placed filler master.
Only configured masters with identical width/height, different known VT and
matching bottom-band polarity enter a catalog entry. A globally empty placed
catalog is non-fatal to checker initialization, but a failing baseline returns
`NoCompatibleFillerCandidate` without planner enumeration. Any later checker
metadata disagreement still blocks and returns no partial repair.

---

## 6. Verified boundary

92 portable planner cases and 79 portable real-checker cases build, link and
run in **both** harness modes — fake-UDM and the destination-shaped migration
gate (171/171, normal and ASan). Repository-local fake-UDM engine regression:
117 cases. Full local suite 288/288, normal and ASan.

Those counts build the full `fillerRepair/` verification package. The sibling
`fillerRepair2/` runtime-only projection is not compiled or compared by these
CTest gates and must be built separately against the destination dependency
target before migration.

The fixture invariants the real-checker cases depend on — rule and layer ids as
container indices, the band-polarity model, the `maxRuleValue_`-sized snapshot
window, what `xWindow` means per rule kind, and min width applying to every run
— are documented in `../fillerRepair/README.md`.
