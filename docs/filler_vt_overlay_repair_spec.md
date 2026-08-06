# Filler VT overlay repair specification

This document is the current behavioral contract for filler repair. Migration,
build, and validation instructions live only in `src/dpl2/HandOff.md`.

## 1. Purpose and scope

Opto may replace one standard-cell master with a same-size VT alternative at
the same placement. That overlay can make nearby filler implant shapes violate
minimum-width or minimum-spacing rules. Filler repair searches for a set of
same-size filler master replacements and asks `ImplantLayerChecker` to validate
every candidate.

The current implementation supports:

- one target standard cell per request;
- target master replacement at the same row, x, and orientation;
- one or more filler `Replace` operations;
- same filler instance, position, orientation, width, and height;
- checker-verified minimum-width and minimum-spacing repair;
- no database, Grid, or Network mutation.

It does not move cells, change filler footprints, add/delete instances,
split/merge fillers, repair multiple target cells together, or commit results.
The caller owns commit and rollback.

## 2. Runtime API and ownership

```cpp
namespace dpl2::fillerRepair {

struct RepairOutcome
{
  bool hasSolution = false;
  ipl::FillerChanges changes;
  std::vector<ipl::Diagnostic> diagnostics;
};

class FillerRepairEngine
{
 public:
  FillerRepairEngine(Grid* grid, Network* network);
  bool init(const ipl::ImplantLayerChecker& checker);
  RepairOutcome repair(const ipl::CheckRequest& request);
};

}  // namespace dpl2::fillerRepair
```

The lifecycle owner constructs one checker and one engine for one immutable
placement revision:

```cpp
ipl::ImplantLayerChecker checker(grid, network);
fillerRepair::FillerRepairEngine engine(grid, network);
if (!engine.init(checker)) {
  return false;
}
checker.setFillerRepairEngine(&engine);
```

The checker borrows the engine and the engine borrows the checker. Neither owns
the other. Bind them before starting worker threads and keep both alive until
all checks finish. After any Grid, Network, UDM, filler-setting, instance, or
master-registration change, stop workers and create a new pair.

`ImplantLayerChecker::check(...)` is the caller-facing entry. It first performs
the ordinary target overlay check. If that check fails, filler repair is
enabled, and an initialized engine is bound, it calls `engine.repair(request)`.
On success it appends the returned records to the caller-owned vector. The
checker stores no repair result.

Repair is enabled by default on an ordinary checker. Checker-only helper tests
disable it explicitly. The overlay oracle methods never call repair, which
prevents recursion.

## 3. Shared data contract

The checker, planner boundary, result, and caller use the one record owned by
`infrastructure/Objects.h`:

```cpp
enum class OpType : uint8_t { Replace, Delete, Add };
using CellData = std::variant<std::string, eUNL::LeafCellID>;

struct CellChangeRecord
{
  OpType op_;
  CellData cell_data_;
  eUTL::UvDist x_;
  eUTL::UvDist y_;
  eLIB::LibCellID orig_lib_cell_;
  eLIB::LibCellID new_lib_cell_;
  eUTL::PhysOrientation orientation_;
};

using FillerChanges = std::vector<dpl2::CellChangeRecord>;
```

Current repair emits only `Replace + LeafCellID`. Each record contains every
field, including the unchanged position and orientation. Failure always
returns an empty change list; partial repairs are never exposed.

ID authorities are fixed:

- planner/checker instance ID: `Node::getId()`;
- planner/checker master ID: `Master::getId()`;
- physical instance/master handles: `LeafCellID` and `LibCellID`;
- row and column: the same Grid frame used to create `ipl::CheckRequest`;
- x inside the planner: core-left-relative DBU.

## 4. Infrastructure authorities

`Grid` supplies `PhysDesMgr` and legal placement pixels. `Network` supplies the
placed Nodes, registered Masters, and a non-owning pointer to DePlace's active
`fillerSetting`. Global Session state is not consulted.

Filler identity has two explicit authorities:

- a placed instance is editable only when `Node::isFiller()` is true;
- replacement masters come only from
  `fillerSetting::getFillerPhysCells()`.

UDM macro-type flags and master-name prefixes are not used by the engine.
Infrastructure must register configured filler masters with the real edge
table before engine initialization. The engine may refresh the filler flag on
an existing Master, but it never creates an incomplete Master.

Network must contain every placed/fixed object that can cover the core,
including hard macros. Placement blockages remain Grid state and are not
represented as Nodes.

## 5. Initialization contract

`init(checker)` is one-shot and fail-closed. It verifies the shared design
context, row/column frames, master metadata, configured filler mappings, and
checker initialization diagnostics. It then freezes:

- the placement/master snapshot used by the planner;
- legal row segments derived from Grid pixels;
- a compatible replacement catalog for every placed filler master;
- the checker rule reach and initial snapshot halo.

A replacement enters a filler catalog entry only when it is configured,
registered, filler-classified, the same width and height, a different known VT,
and has compatible bottom-band polarity. An empty catalog is allowed at init;
an illegal target snapshot then returns `NoCompatibleFillerCandidate` without
starting combinational search.

The supported coordinate envelope is validated rather than guessed: physical
row iteration and Grid row IDs must agree, x origins must use the same frame,
and supported orientations are R0, R180, MX, and MY. A mismatch is fatal.

## 6. Placement coverage gate

There is no public whole-design precheck. Infrastructure owns whole-design
placement legality. `repair()` performs a regional fail-closed gate only on
rows it may edit.

At initialization, each row is split into maximal legal segments whose Grid
pixels are valid and not reserved by halo/padding. At repair time, placed/fixed
physical spans are clipped to those segments and swept:

- zero coverage produces a `Gap` warning;
- coverage greater than one produces an `Overlap` warning;
- blockages, fragmented-row holes, reserved padding/halo, and other invalid
  Grid pixels are outside the required coverage domain.

The initial target influence rows are checked before the target overlay. If an
adaptive candidate reaches additional rows, those rows are checked before that
candidate enters a checker batch. A failing candidate does not invalidate
other candidates in the same batch. The gate does not inspect implant rules or
modify placement.

## 7. Repair flow

### 7.1 Request validation and initial snapshot

The engine resolves the `CheckRequest` against its immutable snapshot and
rejects an unknown/non-standard target, unsupported orientation, size-changing
master, moved target, unregistered master, or master added after init.

It then calls the same checker with:

- the proposed target placement;
- an initial guard based on checker rule reach and the widest configured
  filler master;
- empty `FillerChanges`.

If this snapshot is legal, repair succeeds with no changes. If it is illegal,
its violations become the planner input.

### 7.2 Candidate generation and search

The planner is database-free and receives two seams from the engine:

- `PlacementView`: immutable geometry and compatible master queries;
- `RepairOracle`: ordered batch calls to the same checker.

The search pipeline is:

1. Normalize violations without geometrically deduplicating distinct rules,
   layers, bands, or participants.
2. Build L0 from participants, target-adjacent fillers, and bridge fillers.
3. Generate all legal same-footprint swaps in the current window.
4. Rank fillers deterministically; keep every compatible master in each
   filler's domain.
5. Enumerate filler subsets and domain assignments in ranked order.
6. Validate candidates in ordered checker batches.
7. If needed, grow adaptive-L1 toward the best residual violation. If that
   side is blocked, try the opposite side so a farther required filler remains
   reachable.

Small windows are enumerated completely when their assignment space fits the
remaining budget. Larger windows use bounded subset/member caps. A canonical
key consisting of guard plus sorted `(instanceId, newMasterId)` pairs prevents
duplicate checker calls.

The guard is larger than the editable window. It includes the checker's rule
reach horizontally and neighboring rows/cell rings needed to observe migrated
violations. Fillers in guard-only space are check-only and cannot appear in a
candidate.

### 7.3 Acceptance rule

Each guard first receives an empty-filler baseline query. The baseline must
reproduce the original snapshot; otherwise the request is stale or
inconsistent and search stops with `BaselineMismatch`.

A candidate is accepted only when all of the following hold:

- checker result count/order matches the request batch;
- the result has no fatal/protocol diagnostic;
- `isLegal` agrees with whether blocking violations are empty;
- every original violation disappears;
- no violation is newly introduced inside the repair window;
- no new or migrated violation related to a changed span appears in the
  guard halo.

Baseline and candidate violations are compared as multisets with one-to-one
signature consumption. Unrelated pre-existing findings in guard-only space do
not block repair. The first accepted overlay is returned; there is no redundant
second check of the identical overlay and guard.

## 8. Search limits and determinism

Defaults are defined in `RepairConfig`:

| Setting | Default | Meaning |
|---|---:|---|
| `checkerCallBudgetPerWindow` | 512 | candidate calls allowed for one window |
| `checkerCallBudgetPerRepair` | 2048 | total candidate calls for one repair |
| `batchSize` | 32 | ordered candidates per checker batch |
| `maxSubsetSize` | 4 | largest subset in truncated search |
| `memberCapSize2/3/4` | 24/12/8 | ranked filler caps by subset size |
| `adaptiveStepFillers` | 2 | fillers added per row and direction |
| `maxAdaptiveLevels` | 32 | adaptive growth safety bound |

Exhausting a budget or adaptive bound is a safe failure, not proof that no
solution exists. It returns no changes. Complete enumeration is definitive
only for the last window actually searched.

Ordering is pinned at every stage, so an immutable input produces the same
checker request sequence, result, and diagnostics. Each `repair()` creates its
own planner, cache, and output. Concurrent calls may share an initialized
checker/engine pair only while Grid, Network, UDM, and `fillerSetting` remain
read-only.

## 9. Diagnostics and logging

Structural setup errors are fatal and prevent binding. Request-local invalid
inputs fail without changing registries or placement. Gap/overlap is reported
as warning diagnostics and blocks that repair. Checker errors, stale baselines,
mapping loss, and stale snapshots fail closed.

The deterministic `[fr][stage]` transcript is enabled by default and can be
disabled with `FR_VERBOSE=0`. It records request/frame data, candidate catalog
statistics, windows and guards, enumeration/batch counts, baseline decisions,
budgets, and the final outcome. Logging must not affect search behavior.

## 10. Verification requirements

The maintained tests must cover:

- pure planner behavior with database-free `PlacementView` and
  `RepairOracle` doubles;
- real `ImplantLayerChecker` overlay behavior through its helper-built input;
- clean, gap, and overlap coverage, including legal blocked/halo holes;
- single-, multi-, and adaptive three-swap repairs;
- baseline mismatch, multiset delta, protocol failure, and budget exhaustion;
- sparse filler density, opposite-direction adaptive growth, and empty
  compatible catalogs;
- exact `CellChangeRecord` mapping and append-only caller behavior;
- database, Grid, Network, and physical-record non-mutation on success and
  failure;
- deterministic output, normal build, ASan, and
  `-Wall -Wextra -Werror` compilation.

Helper dump replay is a test/debug path, not a runtime dependency. Dump v5
stores row base polarity and the serializable filler-setting projection.
`test_filler_repair -load <dump.gz>` rebuilds helper data and runs the planner
against the real checker without constructing UDM objects or mutating the
dumped placement.

## 11. Current limitations

- Repair remains swap-only; a valid solution requiring filler split/merge is
  outside this implementation.
- The search is bounded and can safely miss a solution outside its explored
  window or subset space.
- Runtime initialization still requires real UDM-backed physical handles and
  infrastructure master registration; portable planner/checker tests do not
  prove a destination's UDM wiring.
- The row/column frame checks intentionally reject designs whose checker and
  Grid frames do not coincide.
