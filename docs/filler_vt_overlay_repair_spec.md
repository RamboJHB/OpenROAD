# Filler VT overlay repair specification

This document is the current behavioral contract for filler repair. Migration,
build, and validation instructions live only in `src/dpl2/HandOff.md`.

## 1. Purpose and scope

Opto proposes one standard-cell transaction. Deleting an existing cell releases
sites that filler repair must exactly fill. Adding a new buffer consumes sites
occupied by fillers, which filler repair removes and locally retiles. Replacing
an existing cell is limited to same-footprint master/orientation changes and
may require filler VT swaps. `ImplantLayerChecker` validates the complete target
plus filler transaction before the caller commits anything.

The current implementation supports:

- one target standard cell per request;
- one- or two-row Add/Delete targets and fixed-origin, same-footprint Replace;
- one- or two-row configured filler masters;
- filler `Replace`, `Delete`, and request-local `Add` operations;
- exact gap/overlap-free fill after Delete and room creation before Add;
- added-filler orientation selected from the Grid row/site authority;
- checker-verified minimum-width and minimum-spacing repair;
- no database, Grid, or Network mutation.

It does not move the target or unrelated standard cells, repair several target
cells in one request, support masters taller than two rows, or commit results.
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
  explicit FillerRepairEngine(const ipl::ImplantLayerChecker& checker);
  bool isReady() const;
  std::vector<ipl::Diagnostic> getInitDiagnostics() const;
  RepairOutcome repair(const ipl::CheckRequest& request) const;
  RepairOutcome repair(const CellChangeRecord& targetChange) const;
};

}  // namespace dpl2::fillerRepair
```

The opto-facing boundary is only `DePlace`:

```cpp
bool initializeFillerRepair(
    const std::vector<const PhysLibCell*>& targetMasters);
bool isLegal(LeafCellID cellId,
             LibCellID newMaster,
             std::vector<CellChangeRecord>& fillerChanges);
bool findLegal(CellChangeRecord& targetAdd,
               int diameter,
               std::vector<CellChangeRecord>& fillerChanges) const;
bool repairFillers(const CellChangeRecord& targetChange,
                   std::vector<CellChangeRecord>& fillerChanges) const;
```

DePlace constructs and owns one checker/engine chain for one immutable
placement revision after filler and target masters are known:

```cpp
if (!deplace.initializeFillerRepair(targetMasters)) {
  return false;
}
```

DePlace is the only infrastructure owner and initializes Design, Grid, Network,
`fillerSetting`, `PlacementDRC`, and the engine. `PlacementDRC` owns the checker;
DePlace retains a stable non-owning pointer to that checker. The checker borrows
the DePlace object set, and the engine borrows only the checker. Construction
eagerly builds the engine snapshot. DePlace binds the ready engine by the
checker's readiness-checked release/acquire publication before exposing either
object to workers. Repeated initialization is accepted only for the same frozen
master/configuration revision.
After any Grid, Network, UDM, filler-setting, instance, or master-registration
change, stop workers and create a new DePlace revision.

The complete std-cell master universe opto may propose is passed to
`DePlace::initializeFillerRepair(targetMasters)`. DePlace registers it together
with configured filler masters using its real edge table before construction.
A master or filler-setting change after publication is outside the immutable
revision and is rejected.

`ImplantLayerChecker::check(...)` remains the Node-facing entry. Its shared
boundary is
`check(cell, x, y, orient, cellChanges, overlayChanges)`: the second list is
the caller's read-only displaced-cell overlay and only successful filler
records are appended to the first list. With repair enabled and an initialized
engine bound, the checker calls `engine.repair(request)` when a same-footprint
Replace fails direct DRC. It always calls the engine for an uncommitted Add,
even when implant DRC is clean, because the Add must still remove/re-tile
covered fillers. The checker stores no request or repair result.

An uncommitted Node is distinguished from an existing Node by committed
database identity, not by numeric Node ID alone. This is required because a
new infrastructure Node defaults to ID 0, which may already be occupied.

For a fixed-origin master swap, opto calls
`DePlace::isLegal(cellId, newMaster, fillerChanges)`. It builds a request-local
candidate Node and never temporarily edits Network, Grid, or UDM. For a new
buffer, opto passes its Add record to
`DePlace::findLegal(targetAdd, diameter, fillerChanges)`; the method derives row
orientation, searches deterministically around the preferred origin, updates
the target Add pose on success, and returns the filler transaction. It examines
at most 65536 in-range sites and submits at most 4096 legal-orientation sites
to the checker, then fails safely with no changes. Explicit
Delete or already-positioned Add/Replace records use
`DePlace::repairFillers(targetChange, fillerChanges)`. `targetChange` is one
caller-owned standard-cell `Add`, `Delete`, or `Replace` record; the
checker delegates to `engine.repair(targetChange)` without changing the Node,
Network, Grid, or UDM. Success appends only the required filler records. The
caller commits its original target record and the returned filler transaction
atomically.

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

`Replace` and `Delete` identify an existing filler by `LeafCellID`. `Add`
identifies a request-local filler by a deterministic string and carries its
absolute x/y, new master, and Grid-derived orientation. The caller may resolve
that request-local name into its final database identity at commit. Failure
always returns an empty change list; partial repairs are never exposed.
The `Add` name extends dpl's coordinate-based filler naming with dimensions
and a request-local sequence:
`fillerSetting::getPrefix() + "_FR_" + row + "_" + startColumn + "_W" +
width + "_H" + height + "_" + addIndex`, with no spaces.
`width` and `height` are the added master's physical DBU dimensions.

The std-cell input overload accepts one transaction. Delete/Replace carry the
existing `LeafCellID`; Add carries a non-empty request-local name because the
new buffer has no Network/UDM instance yet. `orig_lib_cell_` must match the
snapshot for Delete/Replace. Every target master must be registered before
engine construction. Delete/Replace x/y must match the snapshot; Add x/y is
opto's selected legal site. Replace may change master and/or R0/R180/MX/MY
orientation only when its width and row height remain unchanged. Invalid or
stale input fails with diagnostics and empty changes.

ID authorities are fixed:

- planner/checker instance ID: `Node::getId()`;
- planner/checker master ID: `Master::getId()`;
- physical instance/master handles: `LeafCellID` and `LibCellID`;
- row and column: the same Grid frame used to create `ipl::CheckRequest`;
- x inside the planner: core-left-relative DBU.

## 4. Infrastructure authorities

DePlace initializes and retains the non-owning `Design*`, Grid, Network,
`fillerSetting`, PlacementDRC, and the repair engine; PlacementDRC owns the
checker. The checker borrows the infrastructure objects, and the engine obtains
them only from the checker. `Grid` supplies legal placement pixels. `Network`
supplies the placed Nodes, registered Masters, and a non-owning pointer to
DePlace's active `fillerSetting`. The engine performs no cross-object design
identity checks; that consistency is guaranteed by DePlace ownership. Global
Session state is not consulted by checker or engine.

Filler identity has two explicit authorities:

- a placed instance is editable only when `Node::isFiller()` is true;
- replacement masters come only from
  `fillerSetting::getFillerPhysCells()`.

UDM macro-type flags and master-name prefixes are not used by the engine.
Infrastructure must register configured filler masters and every candidate
std-cell target master with the real edge table before checker/engine
initialization. Checker master metadata is immutable after construction.
Neither checker nor engine adds or lazily rebuilds a master during a worker
request.

Network must contain every placed/fixed object that can cover the core,
including hard macros. Placement blockages remain Grid state and are not
represented as Nodes.

## 5. Initialization contract

Construction is eager and fail-closed. It reads infrastructure through the
checker and verifies row/column frames, master metadata, configured filler
mappings, and checker initialization diagnostics. It then freezes:

- the placement/master snapshot used by the planner;
- legal row segments derived from Grid pixels;
- a compatible replacement catalog for every placed filler master;
- the checker rule reach and initial snapshot halo.

Failure leaves `isReady()` false and retains every reason in
`getInitDiagnostics()`. The same diagnostics are printed as `[fr][engine] init
diagnostic` lines by default; `FR_VERBOSE=0` disables that debug transcript.
Callers must never bind an engine that is not ready.

A replacement enters a filler VT catalog entry only when it is configured,
registered, filler-classified, the same width and height, a different known VT,
and has compatible bottom-band polarity. The retiler separately records every
configured, registered, site-aligned filler footprint that is one or two rows
high. An empty VT catalog is allowed; a layout change still succeeds when an
exact filler tiling is checker-legal without additional VT replacements.

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

### 7.1 Request validation

The engine converts a valid std-cell `CellChangeRecord` to the same
`CheckRequest` used by the checker entry, then resolves that request against
its immutable snapshot. It
rejects an unknown/non-standard target, unsupported orientation, unregistered
or post-init master, non-site-aligned width/x/y, target outside legal Grid
pixels, or target height outside one or two rows. Delete/Replace also reject
x/y different from the snapshot; Replace rejects a footprint change. The
regional gap/overlap gate covers the affected target footprint.

### 7.2 Add/Delete layout transactions

Target Delete starts with the old std-cell footprint as released sites. Target
Add scans the new buffer rectangle, rejects any non-filler occupant, deletes
every covered filler, and forms released sites from those filler footprints
minus the new buffer. The engine then:

1. verifies the target rectangle and released sites against the Grid;
2. rejects any released site occupied by an unchanged instance;
3. deterministically enumerates exact covers using configured one/two-row
   filler footprints, largest footprint first;
4. chooses a registered master for every tile, preferring the target VT when
   available, and obtains orientation from
   `Grid::getSiteOrientation(col, row, siteName)`;
5. submits the target plus the complete filler `Delete`/`Add` transaction to
   the same checker.

Geometry enumeration is database-free and per-call. It returns at most 16
tilings and visits at most 100000 exact-cover states. Hitting either limit is a
safe failure. Synthetic negative planner IDs and Add names exist only inside
that request; they are never inserted into Network or UDM.

If an exact tiling is implant-legal, its fixed `Delete`/`Add` records are the
result. If it still has implant violations, the existing VT planner runs over
the immutable retiled view; its `Replace` records are merged into the same
checker-verified transaction. A VT replacement of a request-local Add updates
that Add record rather than creating a second record.

For Replace, the engine first checks the proposed same-footprint target
with empty filler changes. A legal snapshot returns success with no changes;
an illegal snapshot enters the VT planner directly.

### 7.3 Candidate generation and search

The planner is database-free and receives two seams from the engine:

- `PlacementView`: immutable geometry and compatible master queries;
- `RepairOracle`: ordered batch calls to the same checker.

The search pipeline is:

1. Normalize violations without geometrically deduplicating distinct rules,
   layers, bands, or participants.
2. Build L0 from participants, target-adjacent fillers, and bridge fillers.
3. Generate all legal same-footprint VT replacements in the current window.
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

### 7.4 Acceptance rule

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
own exact-cover search, retiled placement view, planner, cache, synthetic IDs,
and output. The checker loads its atomic enable state and published engine
pointer once per request. Engine repairs take shared state access, so worker
calls run concurrently; repository-local snapshot refresh and debug
reconfiguration take exclusive access. Concurrent calls may share an
initialized checker/engine pair only while Grid, Network, UDM, and
`fillerSetting` remain read-only. Initialization, first binding, teardown, and
database commit must not overlap worker calls.

## 9. Diagnostics and logging

Structural setup errors are fatal and prevent binding. Request-local invalid
inputs fail without changing registries or placement. Gap/overlap is reported
as warning diagnostics and blocks that repair. Checker errors, stale baselines,
mapping loss, and stale snapshots fail closed.

The deterministic `[fr][stage]` transcript is enabled by default and can be
disabled with `FR_VERBOSE=0`. It records request/frame data, candidate catalog
statistics, windows and guards, enumeration/batch counts, baseline decisions,
budgets, and the final outcome. Logging must not affect search behavior.

The transcript is structured for direct terminal use: initialization, target
snapshot, Add/Delete retiling, planning, each adaptive window, baseline, candidate
search, and final result have visible section boundaries. Single records use
aligned key/value blocks; repeated masters, rows, violations, swaps, rankings,
and subset counts use wrapped tables; diagnostics use lists or labeled blocks.
The formatter limits the payload of every physical line to 96 characters
(112 including the longest current `[fr][stage]` prefix) and emits each
multi-line record with one stdio write so concurrent calls are less likely to
interleave inside a record. Formatting changes neither the recorded values nor
the search.

## 10. Performance characteristics

The planner is intentionally bounded by checker requests rather than elapsed
time. Runtime cost therefore depends primarily on the number and size of
checker batches, not on the number of planner objects allocated. Keep one
initialized checker/engine pair for a design revision; constructing a pair per
proposal measures initialization, not normal repair latency. Disable the
diagnostic transcript with `FR_VERBOSE=0` for throughput runs.

The checker snapshot visits each Network instance at most once even when a
wide or multi-row cell occupies many Grid pixels. The engine accepts stable,
sparse Network IDs without rescanning the containers, and builds its immutable
slot tables once. `PlacementDRC` evaluates into a request-local trial vector
and publishes changes only after every checker succeeds, avoiding both partial
output and rollback work on failure.

The following regression baseline was measured on 2026-08-06 using an Apple
M4 (10 cores, 16 GB), macOS 26.2, Apple Clang 17, Release builds, and
`FR_VERBOSE=0`. GoogleTest repetitions include fixture construction, so these
numbers are conservative for a caller that reuses an initialized pair. They
are comparison data for future changes, not a destination SLA.

| Case | Repetitions / wall time | Average |
|---|---:|---:|
| planner ranked-pair search, synthetic oracle | 5000 / 0.31 s | 0.062 ms |
| planner adaptive-L1 search, synthetic oracle | 2000 / 0.07 s | 0.035 ms |
| clean target overlay | 3000 / 0.22 s | 0.073 ms |
| one checker-verified filler replacement | 2000 / 1.08 s | 0.54 ms |
| target Add/Delete filler retiling | 1000 / 0.07 s | 0.07 ms |
| two-row target/filler transaction | 1000 / 0.09 s | 0.09 ms |
| checker E2E at 5% filler density | 1000 / 0.87 s | 0.87 ms |
| adaptive three-swap stress case | 25 / 2.10 s | 84 ms |

The three-swap stress case is deliberately configured with a 4096-call window
budget and batch size 64. Its transcript reports 1147 checker requests in 18
batches: 1024 requests exhaustively disprove the initial ten-filler window,
then adaptive L1 finds the three-swap answer after adding two fillers. This is
an adversarial completeness test, not the default 512-call-window behavior.
It identifies the real optimization target: reduce checker questions before
trying to micro-optimize planner containers.

Eight concurrent one-swap repairs completed 4000 calls in 0.92 s (about 4350
calls/s), versus about 1850 calls/s in the serial fixture. The roughly 2.3x
throughput improvement is useful but not linear because each checker batch can
also use TBB internally; the destination must avoid oversubscribing outer opto
workers and inner checker workers.

The repository-local real-ODB smoke took 2.30 s for 50 fresh OpenROAD
processes, about 46 ms/process. That includes executable startup, LEF/DEF read,
ODB-to-test-UDM projection, DePlace/checker/engine initialization, and one
repair. It proves wiring and repeatability, but it is not a hot-path repair
measurement and the tiny design is not a scalability result.

Optimization order is therefore:

1. reuse the initialized checker/engine pair and run with `FR_VERBOSE=0`;
2. tune `batchSize` and checker-call budgets from real-design p95/p99 request
   counts while preserving fail-closed truncation;
3. bound outer worker concurrency against the checker's internal TBB width;
4. if the three-swap pattern is common, experiment with a staged search that
   tries capped low-order subsets and directional adaptive growth before
   returning to exhaustive proof of the smaller window;
5. only after profiling shows them material, consider per-guard checker
   baseline reuse or replacing the small request-ID `std::map` with contiguous
   batch correlation.

Step 4 must retain deterministic ordering and eventually revisit skipped
subsets when a definitive no-solution answer is required. Cross-repair caches
are intentionally not recommended: they complicate design-revision lifetime,
consume unbounded memory, and are unsafe across commits.

## 11. Verification requirements

The maintained tests must cover:

- pure planner behavior with database-free `PlacementView` and
  `RepairOracle` doubles;
- real `ImplantLayerChecker` overlay behavior through its helper-built input;
- clean, gap, and overlap coverage, including legal blocked/halo holes;
- single-, multi-, and adaptive three-swap repairs;
- baseline mismatch, multiset delta, protocol failure, and budget exhaustion;
- sparse filler density, opposite-direction adaptive growth, and empty
  compatible catalogs;
- exact-cover retiling at 10%, 40%, and 100% released-area utility;
- target Add room creation, target Delete refill, and one/two-row combinations;
- Add/Delete validation, Grid-derived orientation, and atomic checker entry;
- exact `CellChangeRecord` mapping, fixed-origin rejection, append-only caller
  behavior, and Add/Delete transactions through that API;
- database, Grid, Network, and physical-record non-mutation on success and
  failure;
- loaded-design `test_filler_repair` coverage for same-footprint master swap,
  explicit rotation, target Delete refill, target Add room creation, returned
  operation counts, and pre/post UDM/Network/Grid fingerprints;
- deterministic output, normal build, ASan, focused ThreadSanitizer coverage
  for shared checker/engine calls, and `-Wall -Wextra -Werror` compilation.

Helper dump replay is a test/debug path, not a runtime dependency. Dump v6
stores row base polarity, the serializable filler-setting projection, and
master site names needed to validate Add orientation; v1-v5 remain readable.
`test_filler_repair -load <dump.gz>` rebuilds helper data and runs the planner
against the real checker without constructing UDM objects or mutating the
dumped placement.

## 12. Current limitations

- Only site-aligned rectangular target and filler footprints one or two rows
  high are supported.
- Delete/Replace movement is unsupported. Add accepts opto's site-aligned
  selected location. Replace must keep the footprint unchanged.
- Exact-cover enumeration is bounded and can safely miss a later tiling after
  16 solutions or 100000 visited states.
- New-buffer placement search can safely miss a farther site after 65536
  examined Grid sites or 4096 checker candidates.
- The search is bounded and can safely miss a solution outside its explored
  window or subset space.
- Runtime initialization still requires real UDM-backed physical handles and
  infrastructure master registration; portable planner/checker tests do not
  prove a destination's UDM wiring.
- The row/column frame checks intentionally reject designs whose checker and
  Grid frames do not coincide.
