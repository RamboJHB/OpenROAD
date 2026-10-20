# Filler overlay repair specification

## Scope

Filler repair evaluates one proposed standard-cell placement before commit.
It may swap surrounding filler masters and insert fillers into sites released
by a removed standard cell or selected filler deletions. Checker and engine calls do not
modify UDM, Grid, Network, Nodes, or fillerSetting.

Supported target forms:

- `isLegal`: replace one committed standard cell with a larger, smaller, or
  same-size master, at its original position or a searched position. The old
  instance ID and connections are preserved.
- `findLeg` / buffer insertion: place a temporary standard cell over one or
  more selected committed fillers. Every target site must be covered by those
  fillers, and every selected filler must intersect the target. Selected
  fillers may extend outside the target.

One- and two-row target/filler masters and `R0`, `R180`, `MX`, and `MY`
orientations are supported, subject to legal site/orientation and checker
constraints. Target coverage containing empty sites and unrelated filler
deletions are outside this contract. A removed old standard cell may be disjoint
from the new target; every selected real filler must intersect the target.

## Request and result

The checker consumes this request directly, without a resolved-request layer:

```cpp
struct CheckRequest {
  const Node* cell = nullptr;
  GridX x{0};
  GridY y{0};
  PhysOrientation orientation = PhysOrientationE::R0;
  std::vector<CellChangeRecord> overlayChanges;
};
```

`cell` supplies the temporary target's identity/master; `x`, `y`, and
`orientation` supply its authoritative placement. DRC and overlap checks do not
use the temporary Node's stored left, bottom, or orientation. The checker
removes the committed Network nodes named by `overlayChanges` and evaluates the
temporary target at the request placement.

Real removal overlays are `OpType::Delete` records carrying unique committed
`LeafCellID`s: at most one old standard cell, plus intersecting fillers. A null
`cell` with exactly one standard-cell Delete prepares removal-only refill.
The repair-capable entry also accepts tentative filler Add/Replace records
from that preparation step; FR normalizes them before invoking its checker
oracle. The checker checks
the target form, registered masters, original-master consistency when supplied,
grid ownership, legal target sites, and complete target coverage. Coverage is
a site-set containment test, not an area comparison: a larger deleted area
cannot compensate for an uncovered target site.

```cpp
explicit FillerRepairEngine(const ipl::ImplantLayerChecker& checker);
bool isReady() const;
RepairOutcome repair(const ipl::CheckRequest& request) const;
```

`RepairOutcome` contains `hasSolution` and `ipl::FillerChanges changes`. Checker
and engine output is the complete filler transaction: unique real filler
`Delete`, final `Add`, and surrounding `Replace` records. It never contains the
old standard-cell Delete or the new target operation. Existing filler Replaces preserve width, height,
origin, and orientation. Added fillers carry request-local names that honor the
configured prefix and avoid live-instance name collisions; IDs are assigned
at commit. Failure returns no solution and no partial changes. Setup and
skipped-request reasons use the `[fr]` log. There is no separate engine init,
bind, update, precheck, adapter, or numeric-request API.

## Repair behavior

Let `D` be all sites owned by the old standard cell and selected deleted fillers,
and `T` the target sites (empty for removal-only preparation). Require `T` to be
a subset of `D`.

- Exact cover (`D == T`) uses the existing surrounding-filler swap search.
  Direct DRC success still returns any required real filler Deletes.
- Non-exact cover (`D != T`) exactly tiles `D - T` using configured,
  site-aligned one- or two-row filler masters. Added fillers use legal site
  orientations and cannot overlap the target or unchanged instances. Released
  sites require filling even when direct target DRC is legal.
- A tiling may pass directly or require changing its new fillers' masters and
  swapping surrounding fillers. Master alternatives preserve each footprint.
  Every accepted batch passes `ImplantLayerChecker::checkPlaceWithOverlays`.

For resize/move, DePlace first prepares a virtual std-cell removal and refill,
then reuses the findLeg diamond search. Each candidate supplies the old std
Delete, preparation fillers, and real intersecting filler Deletes to FR.
FR cancels the tentative Adds and rebuilds `D - T`; it does not emit Deletes
for never-committed fillers. Preparation swaps are seeds that may be kept,
changed, or cancelled. Duplicate real-cell operations and no-op swaps are
removed internally. Preparation failure does not preclude a valid final target.
Nothing is removed, painted, or committed between the two steps.

Resized/moved/removal overlays are compared against the committed placement
over both old and new regions, including surrounding swap effects. A hole
introduced by removal is not a baseline violation that can be grandfathered.

The planner retains adaptive-L1 window growth, ranking, subset search,
request-local caching, and baseline-delta acceptance. Caller-deleted fillers
are excluded from the remaining editable placement. Search is bounded: the
default whole-repair budget is 2048 checker candidates shared by snapshots,
tiling alternatives, and planner work; retile enumeration also has solution
and search-state caps. A legal solution may remain undiscovered when a cap is
reached. Without a checker-approved solution within those caps, repair fails
without partial changes or placement mutation. Truncated enumeration may still
return a complete checked solution from the candidates it retained.

`fillerSetting::getFollowOrder()` controls new-filler candidate order, including
footprint preference before the tiling cap. Site/orientation filtering precedes
ranking. With follow order disabled, the VT preference remains; existing
filler swaps retain their ranking.

Avoid patterns are absolute positive width pairs measured in site counts.
A new filler cannot horizontally abut a forbidden-width filler in any shared
row, whether that neighbor is unchanged or another Add in the tiling.
Caller-deleted fillers and standard cells do not participate. Later master
swaps preserve these widths and therefore preserve this geometry rule.

## DePlace integration and commit

DePlace holds Design references and owns Grid, Network, and fillerSetting.
All library masters must be imported and filler settings finalized before
`initPlacementDRC()`. Setup
refreshes Master/Node filler classification and constructs the checker after
Grid initialization. The local tree registers ImplantLayerChecker; unavailable
destination-checker registrations remain commented out.

```text
DePlace::isLegal / findLeg
  -> PlacementDRC::checkDRC(temp, x, y, orientation, fillerChanges, overlayChanges)
  -> registered DRC checkers
  -> ImplantLayerChecker::check
  -> lazy FillerRepairEngine::repair(CheckRequest)
  -> ImplantLayerChecker::checkPlaceWithOverlays
```

The ordinary direct-check overload does not repair. The explicit overlay
overload publishes a local trial output only if every registered checker
succeeds. ImplantLayerChecker constructs its engine once with `std::call_once`
when repair is needed and enabled; checker-helper use disables repair.
Only exact standard-cell replacement with direct success can bypass the engine.
Real filler Deletes and non-exact coverage use the engine to publish a complete
filler transaction.

DePlace probes assemble complete transactions without mutating placement:

- `isLegal(inst, master, changes)` checks the original position, including
  size up/down. The overload `(inst, master, va, diameter, changes)` also searches
  within a DBU extension and optional absolute-coordinate voltage area; zero
  diameter is strict original-position only. The outer relocation search is
  capped at 256 candidate sites per search rectangle. It appends one
  identity-preserving target `Replace` plus FR's complete filler transaction.
- `findLeg` appends FR's complete filler transaction and one named target `Add`. Its target
  name is `DPL_STD_<row>_<column>_M<LibCellID-index>` with a deterministic
  suffix for collisions with live names or pending Adds.

Pass this batch to `DePlace::commit` without appending target/Delete operations
again. Failure leaves the caller's output unchanged. An existing output prefix
is preserved but is not used as a placement overlay for later probes; separately
successful probes are not jointly DRC-validated. Direct checker callers,
including `test_filler_repair`, append only their target operation to the
returned filler transaction; they must not append the input filler Deletes.

Before publishing success, DePlace checks final target/filler padded footprints
together. A body-only tiling that conflicts with padding fails safely rather
than returning a transaction that commit would reject.

Records use core-relative coordinates; database origins are absolute.
`DePlace::commit` preflights operation IDs, names, masters, geometry, final
occupancy, reservations, and operation feasibility before mutation, then applies
Add/Replace/Delete while updating DB, Network, and Grid. Replace preserves DB
identity. Commit does not run DRC; callers own that validation. Invalid batches
are rejected before placement mutation, but this is not an undo journal for
allocation failures or unexpected UDM exceptions. Fixed/group-owned cells and
non-core changes are rejected. Destination UDM Add/placement signatures and
error semantics still require verification. The local OpenROAD wire mutates
its UDM projection rather than writing back to the source ODB block.

## Lifetime and concurrency

The engine borrows its checker, which exposes the DePlace-owned objects.
Construction caches master, rule, compatibility, and grid-geometry metadata;
it retains no placed-instance snapshot. Each repair reads current Grid/Network
placement and instance names through request-local state. Ordinary committed
Add/Replace/Delete changes are visible to the next repair on the same engine.

Concurrent read-only checks and repairs use request-local search/oracle state.
The owner must establish a barrier between placement readers and commits:
UDM, Grid, and Network cannot change during a check/repair, and all three must
describe the same committed revision before checks resume. Master catalogs,
rules, row/site geometry, and filler settings stay static for the checker
lifetime. Recreate the checker when those setup inputs change; ordinary
placement commits do not require checker replacement.

## Verification coverage

Tests cover the pure planner, checker/engine integration, fake-UDM runtime with
real infrastructure, and executable DePlace/command transactions. Cases include
one/two-row masters, rotation, exact/non-exact multi-filler coverage, released-site
tiling, Add-plus-Swap repair, configured order/avoid patterns, bounded failure,
engine reuse after commits, concurrency, unchanged checker goldens, and probe
non-mutation. Resize/move tests include partial/disjoint moves, shifted core
coordinates, one/two-row height changes, pending Add cancellation, preparation
failure followed by final success, padding rejection, and preserved Node/DB
identity and pin/edge bindings after commit. Integration fixtures cover 50%, 75%, and 90% occupied sites;
internal cases also exercise larger mixed-width standard-cell/filler layouts.
Fake UDM testing does not establish destination ABI/link compatibility or
source-ODB writeback.
