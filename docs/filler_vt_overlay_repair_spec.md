# Filler VT overlay repair specification

## Scope

Filler repair is a pre-commit, non-mutating search used by detailed placement.
It tries to make one proposed standard-cell placement implant-legal by swapping
the masters of nearby, already placed fillers.

The supported target operation is exactly one in-place, same-footprint
replacement:

- `isLegal`: a temporary standard-cell `Node` replaces one committed standard
  cell;
- `findLegal`: a temporary standard-cell `Node` replaces one committed filler.

One- and two-row target masters are supported. The target may use `R0`, `R180`,
`MX`, or `MY`. Moving a target, changing its footprint, covering empty space,
or replacing multiple committed nodes is rejected.

Filler repair does not insert fillers, delete fillers, retile a footprint, or
commit any database change.

## Request and result

The public checker request is:

```cpp
struct CheckRequestOverlay {
  const Node* cell;                       // temporary proposed std cell
  GridX x;
  GridY y;
  PhysOrientation orientation;
  std::vector<CellChangeRecord> overlayChanges;
};
```

`overlayChanges` must contain exactly one `OpType::Delete` record whose
`LeafCellID` resolves to the committed Network node being replaced. The Delete
record is overlay input only; it is not returned as repair output.

The engine API is:

```cpp
explicit FillerRepairEngine(const ipl::ImplantLayerChecker& checker);
bool isReady() const;
RepairOutcome repair(const ipl::CheckRequestOverlay& request) const;
```

`RepairOutcome::changes` contains only same-footprint `OpType::Replace` records
for surrounding filler instances. It never contains the target node and never
contains `Add` or `Delete`. A failed repair returns an empty change list; partial
repairs are forbidden. Setup and skipped-request reasons are written to the
structured `[fr]` log; the engine does not expose a second diagnostic API.

## Integration flow

`DRCChecker::check` and `PlacementDRC::checkDRC` use two record vectors:

```cpp
bool check(const Node* temporary,
           GridX x,
           GridY y,
           const PhysOrientation& orientation,
           std::vector<CellChangeRecord>& fillerChanges,
           std::vector<CellChangeRecord>& overlayChanges) const;
```

`overlayChanges` is input. `fillerChanges` is output. `PlacementDRC` evaluates
all registered checkers against a local trial vector and publishes it only if
all checkers pass.

`dbToOpendp::createNetwork()` imports the complete library master catalog.
The final fillerSetting is already present during import, so Network classifies
masters and nodes before `initPlacementDRC()` constructs the checker set.
`initPlacementDRC()` installs exactly one ImplantLayerChecker together with the
other available DRC checkers. No checker replacement, late classification, or
target-specific `addMaster` is needed.

The ordinary `PlacementDRC::checkDRC` overload calls every checker's direct
interface and never repairs. The overload carrying `fillerChanges` and
`overlayChanges`, used by `isLegal` and `findLegal`, calls every checker's
explicit overlay interface. Only ImplantLayerChecker may append filler repair
records, and the local trial vector is discarded if any checker fails.

Only after direct DRC finds a violation does the published checker create its
repair engine lazily with `std::call_once`. The engine reads Grid, Network,
checker `MasterItem` metadata, and optional fillerSetting candidates through
that checker. Individual unusable masters/nodes/candidates are logged and
skipped. Only missing Grid/Network or invalid row/site geometry makes the
engine unavailable; that one-shot result is shared by all checker workers.

The request flow is:

```text
isLegal:  temp std -> one old std Delete overlay -> surrounding filler Replace
findLegal: temp std -> one old filler Delete overlay -> surrounding filler Replace
```

For `findLegal`, DePlace accepts a candidate site only when every target pixel
belongs to the same filler and that filler's complete footprint equals the
temporary node footprint.

Neither checker nor engine mutates UDM, Grid, Network, Node, or fillerSetting.
Opto owns the target replacement and commits the returned filler replacements.

## Placement and algorithm rules

- Request shape, target identity, footprint, orientation, and location are
  validated at the checker boundary. The engine consumes `CheckRequestOverlay`
  without repeating those policy checks.
- Configured filler masters are preferred; checker-helper data falls back to
  Network masters already classified as fillers.
- A filler swap preserves width, height, location, and orientation.
- The target overlay node is excluded from the editable filler universe.
- The planner keeps adaptive-L1 window growth, ranking, subset search, cache,
  checker budgets, and baseline-delta acceptance.
- Every proposed batch is accepted only by `checkPlaceWithOverlays`.
- Request-local oracle state makes simultaneous repairs read-only and
  deterministic.
- Repair permission is selected by the `PlacementDRC::checkDRC` overload, not
  by changing checker state around a call.

## Revision lifetime

The checker/engine/master catalog is immutable after `initPlacementDRC()`.
All masters and the final fillerSetting are imported before construction.
Database commits and fillerSetting changes must not race with checker calls.
After a committed placement revision, the owner must publish a new checker
revision before starting another repair phase.

## Tests

The repository runs four layers:

- pure GoogleTest planner coverage with no UDM;
- final-checker overlay and planner E2E through `ImplantLayerCheckerHelper`;
- runtime E2E using the test-only fake UDM provider while compiling the real
  infrastructure, checker, and engine sources.
- a compact `fillerRepair2` GoogleTest that uses
  `ImplantLayerCheckerHelper`, invokes the checker-owned engine, and verifies
  the returned swap without mutating Network nodes.

Required runtime cases cover std-to-std replacement, filler-to-new-std
replacement, rotation, two-row targets, shifted row origins, malformed or
multi-node overlays, footprint mismatch, deterministic concurrent checker
calls, Replace-only output, and zero database mutation.
