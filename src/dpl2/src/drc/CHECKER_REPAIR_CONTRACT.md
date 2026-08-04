# fillerRepair ↔ checker/infrastructure contract

Updated: 2026-08-05.

This file records every boundary assumption and checker/infrastructure change
needed by filler repair. The planner does not reproduce implant DRC. Rule
evaluation, violation creation and final legality remain owned by
`ImplantLayerChecker`.

## 1. Caller and result wire

The caller reaches repair only through the five-argument checker entry:

```cpp
bool ImplantLayerChecker::check(
    const Node* node,
    GridX x,
    GridY y,
    const PhysOrientation& orient,
    std::vector<CellChangeRecord>& fcRecord) const;
```

`check()` builds one `CheckRequest`, runs `checkDirect()`, and invokes repair
only when the direct result is illegal. A successful repair appends records to
the caller's vector. The checker retains no change vector and the repair path
does not commit placement.

The one shared record is owned by `infrastructure/Objects.h`:

```cpp
struct CellChangeRecord {
  OpType op_;
  CellData cell_data_;       // LeafCellID for Replace
  UvDist x_;
  UvDist y_;
  LibCellID orig_lib_cell_;
  LibCellID new_lib_cell_;
  PhysOrientation orientation_;
};
```

`ipl::FillerChanges` is a vector alias of this record. Planner requests,
checker overlays and the caller result use the same representation.

## 2. Checker repair lifecycle

- `ImplantLayerChecker(Grid*, Network*)` obtains `PhysDesMgr` only from
  `Grid::getDesMgr()` and stores the actual return value of `init()` in
  `infrastructureReady_`.
- The active `fillerSetting` comes from `setFillerRepairContext()` or the
  provider registered by the infrastructure owner.
- Each failing direct check constructs and initializes a fresh
  `FillerRepairEngine`. This ensures its placement snapshot reflects the
  latest committed UDM revision and cannot retain a previous transient Node
  overlay.
- Missing/mismatched context disables repair until an explicit valid context
  resets it. An engine data failure fails that request but is retryable after
  infrastructure state is corrected.
- One checker/engine repair is not re-entrant. The planner and its private
  oracle are one-request-at-a-time.

The engine snapshots each placed instance's committed `PhysCell` master and
maps that `LibCellID` back to Network. It deliberately does not treat the live
Node's possibly temporary opto master as committed placement.

## 3. Master ownership and filler classification

Infrastructure owns Network Master construction because it owns the real edge
table. After the filler allow-list changes,
`DePlace::registerFillerRepairMasters()` registers:

- every configured filler master, including an uninstantiated spare.

It also refreshes the `Node::FILLER` type for already-imported instances that
reference those masters; changing only `Master::isFiller()` would leave the
engine's placed-instance classification stale.

The existing opto/infrastructure path separately registers every standard-cell
master that opto may place as the target candidate.

`FillerRepairEngine::ensureMasterRegistered()` never calls
`Network::addMaster`. It only resolves an existing configured master and calls
`setFiller(true)`. An unexpectedly missing configured master is safely omitted
because that can only shrink the search space; `init()` fails when no
registered configured master remains.

The request target master must resolve by Network index, carry the same
`Master::getId()`, and map back through `getMasterId(LibCellID)` to that index.
The engine does not build a fallback Master with an empty edge table.

Filler sources are intentionally narrow:

- configured replacement universe: `fillerSetting` list;
- configured Network master flag: refreshed to true by engine init;
- placed instance flag: `Node::isFiller()`;
- UDM macro-type filler flags: diagnostic context only, never a veto.

## 4. Checker request geometry fixes

The checker must evaluate the requested overlay, not the live Node footprint:

- `CheckRequest.rowId` comes from the `y` argument passed to `check()`;
- target width/height come from `masterItems_[request.masterId]`;
- overlap scanning uses requested row, column and requested master footprint;
- direct and overlay target x intervals use requested master width;
- snapshot row/column extent uses requested master width and height;
- direct and overlay validation reject a footprint that crosses any Grid edge.

These changes are required when DePlace has already put the candidate master
on the Node while UDM commit is still pending.

## 5. Checker model validation and reach

Initialization and lazy master refresh are fail-closed:

- `buildMasters()` resets its indexed table before rebuilding and returns a
  status;
- a master containing shapes from more than one known implant VT family emits
  `master_implant_family_mismatch` and fails initialization;
- the portable helper calls the same family validator after injecting master
  data;
- a rule referencing an unknown implant group emits a diagnostic and fails
  `buildRules()` instead of asserting;
- lazy master expansion propagates `buildMasters()`/`buildMstIntervals()`
  failure to the result.

`getMaxRuleValue()` is the checker neighborhood reach in sites. It is computed
as the ceiling of the largest rule query radius divided by site width. Query
radius includes:

- `minValue`;
- absolute PRL when present;
- LENGTH when present.

The site reach is clamped to the Grid row width before conversion to `int`;
scanning farther than the entire row cannot discover another neighbor and the
clamp prevents overflow/pathological guards from malformed rule data. The
engine multiplies this value by site width and exposes it to the planner as
`PlacementView::checkerReachX()`. The guard must never be narrower.

## 6. PlacementDRC publication contract

Each checker may append repair records. `PlacementDRC::checkDRC()` therefore
uses a copy of the caller's vector as staging for the full checker chain and
moves it back only if every checker returns true. Checkers therefore retain the
same append-only input view, while a later checker failure publishes no partial
repair.

Additional build/lifetime fixes:

- `PlacementDRC.h` includes `<drc/DRCChecker.h>` from its actual location;
- orientation uses `eUTL::PhysOrientation`, matching `DRCChecker`;
- the unused `uv3d/Uv3d.hh` dependency is removed;
- `PlacementDRC` deletes the global TBB arena only when that instance created
  it.

The local branch does not contain the destination's complete
`initPlacementDRC()` definition. The destination must register
`ImplantLayerChecker` and route opto through this five-argument path.

## 7. Grid/Network placement assumptions

- Grid, Network, `PhysDesMgr` and the Design held by `fillerSetting` describe
  one revision.
- Row ids are Grid row indices and x is core-left-relative.
- Network contains every placed/fixed physical instance intersecting the core,
  including fillers and hard macros. Placement blockages remain Grid state.
- Legal coverage is derived per row from valid Grid pixels with no padding
  reservation. Gaps between legal segments are allowed.
- The smallest positive non-pad row-frame height is the base height; every
  other non-pad row height must be an integer multiple.
- Supported orientations are R0, R180, MX and MY.

Existing infrastructure fixes on this branch also ensure fillers are painted
into Grid, `Node::isStdCell()` excludes fillers, `updateNode()` rejects an
unregistered master before mutation, and physical orientation is preserved.
`Grid.cpp` and `Object.cpp` contain warning-cleanup changes required by the
strict build; they do not change filler-repair search policy.

## 8. Oracle protocol

The engine calls:

```cpp
std::vector<CheckResult> checkPlaceWithOverlays(
    const CheckRequest& target,
    const Rect& guard,
    const std::vector<FillerChanges>& candidates) const;
```

One `FillerChanges` is one atomic candidate. Results correlate by input order
and counts must match. The engine strips only the approved repeated checker
initialization diagnostics; any other persistent diagnostic blocks init or the
candidate. A clean result must have no violations, and an illegal result must
explain itself with violations or diagnostics. Except when removing those
known initialization diagnostics makes the result clean, disagreement between
`isLegal` and the returned findings is a checker protocol error.

## 9. Verified regressions

The portable checker suite locks:

- requested-row and requested-footprint behavior;
- full-footprint Grid rejection;
- PRL/LENGTH reach;
- mixed-family master rejection;
- malformed overlay fail-closed behavior;
- PlacementDRC all-or-nothing record publication;
- exact ordered batch behavior and real-checker repair outcomes.

The local engine suite additionally verifies that the infrastructure
registration path supplies configured masters before engine init, missing
configured masters are still skipped without engine-side Network mutation, an
entirely unavailable candidate list and missing target masters fail closed,
stale configured flags are refreshed with `setFiller(true)`, and repair leaves
UDM/Grid/Network placement unchanged.
