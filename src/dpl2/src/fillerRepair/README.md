# fillerRepair — filler VT overlay repair

Updated: 2026-08-02.

`ImplantLayerChecker::check()` is the caller-facing entry. Opto owns the
`CellChangeRecord` vector; the checker appends checker-verified repair swaps
into that reference and keeps **no** filler-change member state. The
`FillerRepairEngine` is created **lazily** on the first DRC-illegal check —
a run whose checks all pass never pays engine initialization.

The shared wire is
`CellChangeRecord{Replace, CellData{LeafCellID}, origin_x_, origin_y_,
orig_lib_cell_, new_lib_cell_, orientation_}`. The generic `CellData` protocol
also reserves a string name for future add operations; this swap-only engine
emits and accepts only the existing-cell `LeafCellID` alternative.

```cpp
// production (set_filler_option ran; DePlace registered the setting provider)
std::vector<CellChangeRecord> fcRecord;
bool legal = deplace->isLegal(cellId, lcId, fcRecord);   // -> checker.check(...)
if (legal && !fcRecord.empty()) {
  commitFillerSwaps(fcRecord);   // commit stays with opto/infrastructure
}

// harnesses without a DePlace owner preset the lazy context instead:
checker.setFillerRepairContext(desMgr, &fillerSetting);
```

**One rule-reach authority.** The checker's `getMaxRuleValue()` is literally
the radius (in sites) of the neighbourhood its `getSnapshot` scans, so the
engine sizes its guard from that number alone and never re-derives reach from
raw TechLayer width/spacing. A guard narrower than the reach truncates the
checker's snapshot and can fabricate a min-width violation at the guard edge;
two independent derivations of the same quantity is how that happened once
already.

**Bounded search.** `checkerCallBudgetPerWindow` (512) bounds one window;
`checkerCallBudgetPerRepair` (2048) bounds the whole `repair()` across every
adaptive level, so the worst case is not `maxAdaptiveLevels` full windows.
Reaching either ends the search with the existing *truncated* semantics --
never a wrong answer, only a bounded give-up.

**The guard is quantized so the answer cache spans adaptive levels.** The
guard region *is* the checker's question, so a guard that tracked the window
exactly made every level re-ask what the previous level already answered —
measured on the no-solution path, **90% of all checker calls were an overlay
already asked under a slightly different guard**. Each side is now snapped
outward to a power-of-two distance from the anchor (the one point that cannot
move during a repair), so the guard changes O(log) times over the whole
escalation instead of once per level. Two properties carry the argument, and
`guard_quantization_contains_window_and_is_stable` asserts both: the guard
always **contains** the window and never crosses the core-left edge
(enlarging a guard is sound — a wider snapshot can only remove truncation
artifacts, never add them), and it takes far fewer distinct values than there
are levels.

A side effect worth knowing: a repeated guard makes that level's baseline a
cache hit, so `checkerCallBudgetPerWindow` now buys candidate evaluations
instead of re-buying a baseline already held
(`CachedBaselineFreesWindowBudget`).

**A repeated guard also makes the enumeration incremental.** Under an
unchanged guard, a candidate that was not clean at level L cannot become
clean at L+1: the window only grows, so a halo finding can migrate into it
(still blocking) but no blocker can disappear, and a clean candidate would
have ended the search at L. So level L+1 enumerates only the combinations
that contain a filler it just gained — the rest are questions the previous
level already answered. This is not a heuristic prune: the skipped
candidates still count as covered, so a level that skips them is still
reported *complete*. When the quantized guard does move, every candidate is a
new checker question again and the filter switches off for that level.

**Overlay identity is a value, not a string.** Two candidates are the same
checker question when they propose the same `(instance -> new master)` set
under the same guard; `OverlayKey` is that identity, and `OracleGate::resolve`
hands its answers straight back to the search so one lookup serves both. The
cache is a `std::unordered_map`, chosen node-based on purpose: the gate holds
a pointer to the baseline result inside it while hundreds of later answers are
inserted around it (`gate_baseline_survives_cache_growth` covers that
invariant). It is never iterated -- only `find`/`emplace`/`size` -- so bucket
order cannot reach search order.

The engine's only placement gate is **regional**: repair refuses to run on a
gap/overlap inside the rows it can edit (legal spans derived from Grid pixels
lazily, per row). Whole-design placement legality is infrastructure's own
gate — the engine has no global precheck.

Infrastructure data is **trusted as-is**: RowId is the Grid row, x is
core-left-relative (the same frame the checker's `CheckRequest` uses), and the
engine performs no Network↔UDM cross-validation — with lazy init it typically
runs mid-check, while the candidate Node already carries its proposed master
ahead of the pending UDM commit.

`Grid` is the single design authority for checker construction. It retains the
`PhysDesMgr` used by `initGrid()`, and
`ImplantLayerChecker(Grid*, Network*)` reads that manager through
`Grid::getDesMgr()`. The engine rejects initialization unless its `PhysDesMgr`
is that same pointer, then creates its private checker with only `Grid` and
`Network`. There is no explicit-design checker constructor and no global
design lookup.

**One filler authority.** `fillerSetting::isFillerCell(LibCellID)` answers whether
a master belongs to the configured `core_` list. Infrastructure stores that
answer on `Master`; `Node` inherits it whenever it is added, updated or
updated. `set_filler_option` runs before filler placement, checker
initialization and repair initialization. `Node::isFiller()` /
`Master::isFiller()` are the only downstream queries, so no production path
re-derives filler identity from UDM macro flags. The same core list is the
replacement candidate allow-list.

**Includes** use angle brackets throughout, resolved from the `src/` root
(`<fillerRepair/RepairPlanner.h>`, `<infrastructure/Grid.h>`), matching the
delivered infrastructure/checker sources.

## Porting: what needs a decision

Everything a destination has to decide is tagged in the source. One grep is
the whole list, and the legend sits at the top of `RepairTypes.h`:

```sh
grep -rn "\[PORT-" <srcroot>/fillerRepair
```

- **`[PORT-ADAPT]`** — will not compile, or will be quietly wrong, until you
  change it. Eleven of them, and **five compile cleanly while being wrong**:
  the coordinate frame, the init-diagnostic strip, the overlay batch
  semantics, `getMaxRuleValue()`, and the guard's vertical reach. Those five
  first. **Not optional.**
- **`[PORT-DROP]`** — you do not need it. Each one says what it costs to keep
  and what breaks if you delete it, which in production is nothing.
- **`[PORT-TUNE]`** — four numbers whose answer lives on your hardware, not
  here: the oracle these were measured against answers in ~0 ns, so the cost
  of a real DRC call — the thing that decides them — is exactly what this
  repository does not have. Each says what to measure. Safe as shipped; a
  budget can only end a search early, never give a wrong answer.

`src/dpl2/HandOff.md` §8 lists all of them with the trade-off for each.

## Main files

The module is one runtime layer over one pure search pipeline, joined by two
explicit seams. The engine implements both seams; the planner depends on
nothing else, which is what keeps it database-free and portable.

```
                 FillerRepairEngine          (runtime: UDM/Grid/Network + checker)
                   implements v   ^ implements
        PlacementView (data in)   |   RepairOracle (legality out)
                              \   |   /
                            RepairPlanner     (pure deterministic search)
                                   |
                    RepairTypes + Debug       (leaf data model, logging)
```

| Path | Purpose |
|---|---|
| `FillerRepairEngine.h/.cpp` | runtime entry: snapshot over Grid/Network, regional coverage gate, owns the private checker and planner, implements both seams |
| `PlacementView.h` | **seam 1** — read-only placement view the planner queries (`MasterInfo`, `PlacedInstance`, candidate query, span/binary-search helpers) |
| `RepairOracle.h` | **seam 2** — legality oracle protocol (`OracleRequest/Result/Status`); not a second DRC checker |
| `RepairPlanner.h/.cpp` | the search pipeline in flow order: swap model → violation signatures → L0/adaptive window → ranking → subset enumeration → oracle gate → driver, plus `RepairConfig` |
| `RepairTypes.h` | leaf data model: ids, geometry, violations, diagnostics, entry/exit records. Depends on nothing in the module |
| `Debug.h` | `[fr][stage]` transcript (`cat`, `show`, `DebugLog`) |
| `CMakeLists.txt` | the module's own targets — `dpl2::fillerRepair` (payload, C++20) and `dpl2::fillerRepairPlanner` (pure pipeline, C++17); a destination adds the directory and links a target rather than listing sources |
| `test/CMakeLists.txt` | the portable tests, added when `DPL2_FILLER_REPAIR_BUILD_TESTS=ON` |
| `test/RepairPlannerTest.cpp` | 91 portable database-free planner cases; the two seam doubles and the synthetic master catalog are folded into this one file |
| `test/FillerRepairCheckerE2ETest.cpp` | 77 portable real-checker, repair-window and planner-to-checker cases (see the fixture model below) |

## Debug transcript

The deterministic `[fr][stage]` transcript is **on by default**, so a
production run leaves a diagnosable trail without a rebuild or a rerun. Set
`FR_VERBOSE=0` to silence it (any other value, or unset, keeps it on); the
same variable governs the planner tests. Logging never changes search order
or acceptance.

`log.msg(stage, text)` evaluates its argument at the call site, so inside a
loop use the deferred form -- `log.msg(stage, [&] { return cat(...); })` or a
surrounding `if (log.enabled())` block -- and a silenced transcript costs
nothing there.

Lines are written with normal stdio buffering and **no per-line flush**: line
buffered on a terminal (interactive debugging still sees each line as it
happens), block buffered when redirected. On the worst-case measurement below
the flush alone was half the transcript's cost.

## Portable fixture model

The real-checker fixture is a 7-row x 200-site design that is **legal as
built**: columns march in same-VT pairs (std cell, then filler, cycling the VT
families), so every implant run is exactly min width and the next run of that
family starts four sites later. Nothing is planted.

Every case then does the one thing opto does — retarget a single std cell to a
different VT — and lets the checker say what that costs. Giving the cell the VT
of the pair on its right isolates it: its own run collapses to one site (min
width) and it lands one site from the run it was meant to join (min spacing),
on the new family's N band and its P partner, intra-row and across both row
boundaries. Ten violations, all caused, none authored. The repair is the filler
between the two runs; recolouring it merges them. Targets sit on interior rows
so each case carries real context above and below — enough for the guard
(window +/- two rows) and one adaptive step before it clamps.

Because the layout is legal to begin with, a case that passes is evidence about
the code rather than about the fixture, and the window assertions can pin exact
sizes: five sites, three rows, nine editable fillers, and a guard quantized to
a power-of-two number of sites either side of the anchor.

The fixtures encode five invariants of the current checker; breaking any of
them silently changes what the cases test:

- **Rule and layer ids ARE indices** into `ImplantInput::rules` / `layers`.
  The checker resolves them as `rules_[id]` / `layers_[id]` (its own builders
  assign the container size), so semantic numbering indexes out of bounds.
- **Band polarity**: each VT family has an N layer (bottom band in R0) and a
  P partner (top band), `basePolar = N`, and odd rows are placed MX. Inter-row
  expectations pick the N or P rule by boundary parity (`interRule`).
- **Rule reach sizes the snapshot**: `getSnapshot` spans
  `colId +/- maxRuleValue_` sites, where
  `maxRuleValue_ = ceil(max rule minValue / siteWidth)`. A neighbour further
  out is never in `shapes`, so the spacing scenarios keep their neighbour run
  starting within that window, with a single editable bridge filler forming
  the sub-minimum gap.
- **`xWindow` means something different per rule kind.** Width with no
  neighbour: the run itself. Width with a neighbour: the union (intra-row) or
  the intersection (inter-row). **Spacing: the GAP** — an interval lying
  *between* the participants that overlaps neither of them. The planner seeds
  its window from `xWindow` united with the participants' spans, so the gap
  form is handled, but relatedness of a halo finding is measured from
  `xWindow` alone. An earlier checker left spacing violations with a
  default-constructed `[0,0)`, which parked every one of them at the core's
  left edge; `SpacingViolationXWindowIsTheGap` fails if that returns.
- **Min width still applies** to every run, so a scenario's runs must stay at
  or above `MIN_RULE` while the gap between them stays below it.

## Build

`CMakeLists.txt` owns the targets, so a consumer adds the directory and links
one — it never lists our files:

```cmake
add_subdirectory(<srcroot>/fillerRepair fillerRepair)
target_link_libraries(<owning-target> PRIVATE dpl2::fillerRepair)
```

| Target | Contents | Standard |
|---|---|---|
| `dpl2::fillerRepair` | complete payload | C++20 |
| `dpl2::fillerRepairPlanner` | pure search pipeline, no database access | C++17 |

Everything external — UDM, dpl2 infrastructure, the implant checker, global
flags such as sanitizers — arrives through one interface target,
`dpl2_filler_repair_deps`. Define it before `add_subdirectory` to point the
payload at your headers and libraries; without it the module falls back to the
in-tree layout plus the `DPL2_UDM_INCLUDE_DIRS` / `DPL2_UDM_LIBRARIES` cache
variables, which is what lets it configure, build and test standalone.

Full migration instructions, including the destination checklist, are in
`src/dpl2/HandOff.md`; the portable tests are described in `test/README.md`.

## Verification

- portable planner: 91 cases; portable checker E2E: 79 cases (both compile,
  link and run in fake-UDM AND real-UDM harness modes — the migration gate).
- repository-local fake-UDM engine regression: 106 cases under
  `src/dpl2/test/local/`.
- 2026-08-03 full local suite: 276/276 normal and ASan; migration gate
  170/170 normal and ASan; standalone module build 170/170.

### Search cost

Worst case measured on the no-solution path (120 editable fillers, every
adaptive level searched to the caps, per-repair budget disabled), gcc 13
`-O2`. Planner time excludes the oracle; **checker calls are the number that
matters in production**, where each one is real DRC work:

| | checker calls | batches | planner ms/repair |
|---|---|---|---|
| baseline | 15 633 | 523 | 63.5 |
| value-typed key, no per-call allocation | 15 633 | 523 | 22.5 |
| quantized guard | 2 972 | 189 | 9.8 |
| incremental enumeration + allocation-free classify | **4 230** | **151** | **4.5** |

(call and batch counts are per repair; the ms column is over 20 repeats)

The first step removed overhead only — identical call count. The second was
algorithmic on the **oracle** side: the same ~1 525 distinct overlays are
still explored, they are simply no longer re-asked once per adaptive level
(33 distinct guards became 5).

The third is algorithmic on the **planner** side, and it is the one that
explains the odd-looking call count. Guard quantization stopped the repeats
from reaching the checker, but the search still *generated* them: 15 660
candidate resolutions of which 12 688 (81%) were cache hits, each one paying
for an overlay, a key, a hash lookup and a full delta classification. From
step 3 of the escalation onward every level enumerated its whole 512-candidate
budget and was ~18% productive — **the budget was being spent re-asking rather
than exploring.** Enumerating only combinations that touch a newly editable
filler removes them at the source (cache hits: 12 688 -> 28), so the same
budget reaches deeper into the space. That is why calls go *up* while time
goes down: the search now explores 42% more distinct overlays for less work.

At the shipped `checkerCallBudgetPerRepair` (2048) the comparison is direct —
both stop at the same call ceiling:

| | checker calls | batches | planner ms/repair |
|---|---|---|---|
| quantized guard | 2 048 | 127 | 2.7 |
| + incremental enumeration | 2 048 | **66** | **1.5** |

Same DRC work, **1.8x less planner time and half the batches** (fuller
batches, so fewer round trips), and the calls buy distinct questions.

The common case (a solution a few fillers away) is unaffected: it never
escalates far enough for any of this to apply.

Supporting constant-factor work, all on paths that run once per candidate:
`OverlayKey` carries its swap list inline up to `kInlineSwaps` (spilling to
the heap beyond that, so it is an optimization and not a capacity limit); the
delta classifier reuses its scratch buffers instead of allocating two vectors
per candidate, and compares a packed `SignatureClass` before calling
`sameSignature`, which turns the O(originals x findings) scan into integer
compares for every pair that cannot match. Allocation was ~46% of all
retired instructions before this work.

**Evaluated and not done:** coalescing uncached candidates across chunks into
full batches. `checkPlaceWithOverlays` has a per-batch fixed cost — one
empty-overlay region scan — with the candidates themselves run through
`parallelFor`. Batches are already fuller now that the search stops
generating cache hits (mean 28 rather than 16 per batch). Whether refilling
them further wins depends on the production thread count and on how much a
speculatively-sent candidate costs when an earlier one turns out clean; that
needs measurement on real hardware, not a guess here.
