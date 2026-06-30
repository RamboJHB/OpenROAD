# Filler VT Overlay Repair Spec

Status: proposed plan for `claude/filler-vt-overlay-repair-plan-2023`.
Base: `2023-base`.
Last updated: 2026-06-30.

## 1. Decision

Use a new Plan D: **checker-guided overlay search**.

Earlier Plan A/B/C are useful, but each has a mismatch with the newest goal:

- Plan A is fast but too local. It can miss cases where multiple nearby fillers must change together.
- Plan B/Plan C can optimize a site-grid model, but that duplicates DRC logic and risks disagreeing with the real checker.
- Plan sorting from `claude/filler-vt-weight-2023` gives a good candidate order, but its old rule `touches_cell -> weight 0` is wrong for this problem, because the DRC is caused by std-cell VT changes. A filler touching a changed/fixed std cell is often exactly the filler that must change.

So the chosen flow is:

1. Build a small repair window around each MW/MS violation cluster.
2. Generate legal same-size filler-type changes inside that window.
3. Sort candidates using the weight/tie-break idea from Plan sorting, modified so fixed cells vote for target VT instead of freezing adjacent fillers.
4. Try changes through `checkPlaceWithOverlay` without committing to DB.
5. Accept only an overlay that the checker says is clean for the target place and does not introduce new target MW/MS violations.
6. Return `FillerRepairResult` with the final list of filler master swaps.

This keeps the hard truth in the real checker while keeping the search space small and deterministic.

## 2. Scope And Invariants

Input design state:

- The design is fully filled: no legal empty sites in the repair region.
- DRC is caused by std-cell type / VT changes after ECO or optimization.
- Filler repair may only change filler type, implemented as replacing a filler instance's master with another legal same-geometry filler master.
- There are three VT / implant types.
- DRC rules are MW and MS, each split into intra-row and inter-row cases.

Hard invariants:

- Do not move cells.
- Do not move fillers.
- Do not change filler width, height, x, row, or orientation.
- Do not delete fillers or leave sites empty.
- Do not split or merge filler instances in this phase.
- Do not commit intermediate candidates to DB. All evaluation uses overlay checks.
- If no checker-clean overlay is found within the search budget, return `hasSolution=false` with diagnostics.

## 3. Required Data Model

A candidate filler must expose enough data to enumerate legal same-size replacements.
The current draft is close but should be expanded.

Current draft:

```cpp
struct EditableFiller
{
    InstanceId instanceId = 0;
    MasterId currentMasterId = 0;
    RowId rowId = 0;
    DbCoord x = 0;
    PhysOrientation orientation = PhysOrientation::R0;
};
```

Recommended addition:

```cpp
struct EditableFiller
{
    InstanceId instanceId = 0;
    MasterId currentMasterId = 0;
    FillerTypeId currentTypeId = 0;       // VT / implant abstraction
    RowId rowId = 0;
    DbCoord x = 0;
    DbCoord width = 0;
    DbCoord height = 0;
    PhysOrientation orientation = PhysOrientation::R0;
    std::vector<MasterId> legalReplacementMasters; // same w/h, orient-compatible
};
```

Why:

- `currentMasterId` alone is not enough unless the repair code can query master width, height, implant type, and same-size replacement masters.
- The search should not guess master compatibility.
- Three VT types are easier to reason about through `FillerTypeId`; final output can still use `newMasterId`.

## 4. Checker Interface Feedback

The proposed evaluation API is the right shape:

```cpp
struct FillerChange
{
    InstanceId instanceId = 0;
    MasterId newMasterId = 0;
};

struct CheckOverlay
{
    CheckRequest targetPlace;
    std::vector<FillerChange> fillerChanges;
};

CheckResult checkPlaceWithOverlay(const CheckOverlay& overlay) const;
```

Requests for the checker side:

- `CheckResult` should return a full violation list, not only pass/fail.
- Each `Violation` should include stable id if possible, rule type MW/MS, intra/inter relationship, row ids, x window, primary/secondary implant types, and participants.
- Participants should indicate instance id and whether the participant is a filler or fixed cell.
- `checkPlaceWithOverlay` must support multiple filler changes in one overlay.
- The checker should be deterministic for identical overlays.
- The checker should not mutate DB.
- The checker should report violations inside `targetPlace`; if it also reports nearby spillover violations, tag them separately as `insideTarget` / `spillover`.
- Add a batch API if possible:

```cpp
std::vector<CheckResult> checkPlaceWithOverlays(
    const std::vector<CheckOverlay>& overlays) const;
```

Batch evaluation will make beam search much cheaper.

## 5. Window Construction

The window must be large enough to cover the local implant interaction, but not so large that the search explodes.

### 5.1 Normalize Each Violation

For every input violation, derive:

- `type`: MW or MS.
- `relationship`: intra-row or inter-row.
- `rows`: involved row ids from participants; if missing, use the violation anchor row.
- `xRange`: union of violation `xWindow` and participant bboxes.
- `vtHint`: primary layer for MW; primary and secondary layers for MS.

### 5.2 Initial Windows

Use a ladder. Try smaller windows first, then expand only if no clean overlay exists.

- W0: anchor filler plus its 4-neighbor fillers. This preserves the useful Plan sorting neighborhood.
- W1: all fillers intersecting `xRange` expanded by one rule distance on involved rows.
- W2: snap W1 to whole filler instances and expand horizontally to the nearest fixed cell, blockage, or core boundary on the involved rows.
- W3: for inter-row violations, include the coupled adjacent rows and the same snapped x range.
- W4: merge all W2/W3 windows whose x ranges overlap or whose rows are adjacent and x ranges are within one rule distance.

Recommended default:

- Intra-row MW/MS starts at W1.
- Inter-row MW/MS starts at W3.
- If checker reports a residual violation just outside the window, expand one ladder step and retry.

### 5.3 Candidate Limit

If a merged window has too many candidate fillers, split by connected components of filler adjacency and violation membership.
If still too large, cap the search and return a diagnostic with the window size and candidate count.

Suggested initial cap:

- Greedy mode: up to 64 candidate fillers.
- Beam mode: up to 24 top-ranked candidate fillers per window.

## 6. Candidate Generation

For each editable filler in the window:

1. Enumerate legal replacement masters from `legalReplacementMasters`.
2. Drop the current master.
3. Keep only same width/height and row-orientation-compatible replacements.
4. Map each replacement to a `FillerTypeId` / VT.

A candidate move is:

```cpp
(instanceId, oldMasterId, newMasterId, oldType, newType)
```

Never generate moves for std cells or non-editable fillers.

## 7. Plan Sorting V2

Use Plan sorting as candidate order, with one important correction: fixed cells are constraints and votes, not a reason to give weight 0.

For every candidate filler and target VT, compute:

- `directParticipant`: high priority if the filler appears in the violation participants.
- `cellVote`: adjacency count to fixed std cells whose implant equals the target VT.
- `cellConflict`: adjacency count to fixed std cells whose implant differs from the target VT.
- `fillerVote`: adjacency count to neighboring fillers whose current or overlay VT equals target VT.
- `diffEdgesRemoved`: number of current different-VT adjacencies that would disappear.
- `sameVtAfter`: number of same-VT adjacencies after the change.
- `islandScore`: high if the current filler has no same-VT neighbor and is surrounded by another VT.
- `width`: narrower wins ties.
- `sameVtBefore`: smaller wins ties, inherited from Plan sorting.
- `position`: smaller col, then row, for deterministic final tie-break.

Candidate ordering:

1. Direct participant first.
2. Higher `cellVote + diffEdgesRemoved + islandScore`.
3. Lower `cellConflict`; a target with conflicting fixed cells may still be tried, but ranked late.
4. Narrower filler width.
5. Lower `sameVtBefore`.
6. Leftmost coordinate, then lower row.

Target VT ordering for one filler:

1. VT matching fixed-cell neighbors involved in the violation.
2. VT matching the majority neighboring filler region.
3. VT from the violation primary layer for MW.
4. VT that removes the most MS edges.
5. Stable type id order.

## 8. Checker-Guided Search

### 8.1 Score Function

Every overlay is evaluated by `checkPlaceWithOverlay`.

Define:

```text
score = 100000 * targetViolations
      +  20000 * newInsideTargetViolations
      +  50000 * spilloverViolations
      +    100 * changes.size()
      +      1 * lowPriorityPenalty
```

A clean solution has `targetViolations == 0` and no new inside-target or spillover MW/MS violations.

The exact constants can move, but the ordering must remain:

1. Clean beats everything.
2. Fewer target violations beats fewer changes.
3. No new violations beats smaller edit count.
4. Smaller change count breaks ties.

### 8.2 Greedy Prefix Search

Start with an empty overlay.

Loop:

1. Generate sorted candidate moves not already applied.
2. For each move, evaluate `overlay + move`.
3. Pick the best strict score improvement.
4. Append it to the overlay.
5. Stop when checker reports clean.

This handles simple MS and isolated MW cases cheaply.

### 8.3 Beam Search Escape

MW often requires two or more filler changes before any single change improves the checker result.
When greedy cannot improve:

1. Take the top N candidate moves by Plan sorting V2.
2. Run beam search to depth D.
3. Keep the best K partial overlays at each depth using checker score.
4. Stop early on a clean overlay.

Suggested defaults:

- N = 24 candidates.
- K = 8 beam width.
- D = 4 depth.
- Max checker calls per window = 512 initially.

If a clean overlay is found, return it.
If only an improvement is found, adopt the best improving overlay prefix and resume greedy.
If no improvement is found, expand the window.

### 8.4 Window Expansion And Failure

For each violation cluster:

1. Try the initial window.
2. Greedy search.
3. Beam search.
4. Expand the window one step.
5. Repeat until clean, candidate cap exceeded, or call budget exhausted.

If not clean:

- `hasSolution=false`.
- `changes` should normally be empty unless the caller explicitly accepts partial repair.
- `diagnostics` should include best score, residual violations, candidate count, checker-call count, and the final window bounds.

## 9. Cluster Processing

Do not solve overlapping violations independently.

Build a violation graph:

- Nodes are violations.
- Edge if windows overlap.
- Edge if rows are equal or adjacent and x ranges are within one rule distance.
- Edge if they share a candidate filler.

Solve each connected component as one cluster.

Process clusters in descending severity:

1. More violations first.
2. Inter-row before intra-row if tied.
3. Smaller window first if still tied.

The final result is the union of cluster overlays. Before returning success, run one final `checkPlaceWithOverlay` over the full `targetPlace` with all accumulated changes.

## 10. Why This Should Fix The Input Design

The likely failure mode is:

1. A std cell changes VT / implant type.
2. Nearby fillers still carry the old implant type.
3. The boundary creates MW/MS, either in-row or across adjacent rows.
4. Changing one or several same-geometry filler masters to match the new local implant context removes the checker violation.

The proposed search directly targets that cause:

- It starts at the violation and nearby fillers.
- It prioritizes fillers adjacent to fixed std cells and violation participants.
- It tests real checker results after each overlay.
- It expands only when the local window is insufficient.

## 11. Result Contract

Final output:

```cpp
struct FillerRepairResult
{
    bool hasSolution = false;
    std::vector<FillerChange> changes;
    std::vector<Diagnostic> diagnostics;
};
```

Recommended semantics:

- `hasSolution=true`: `changes` is checker-clean for the requested target place.
- `hasSolution=false`: `changes` is empty by default; diagnostics explain why no clean solution was found.
- A partial-change mode can be added later, but should be explicit because partial repair may hide or move violations.

## 12. Diagnostics

Diagnostics should include:

- branch/window id;
- rule type and intra/inter relationship;
- initial violation count;
- final violation count;
- candidate filler count;
- generated move count;
- checker call count;
- window expansion level;
- best overlay changes;
- residual violation ids and x windows;
- failure reason: no legal master, fixed-cell conflict, search budget, window cap, checker rejected all overlays.

## 13. Implementation Plan

1. Add a pure repair planner over the checker-facing structs.
2. Add conversion from filler masters to `FillerTypeId` and legal replacement lists.
3. Implement window construction and violation clustering.
4. Implement Plan sorting V2.
5. Implement greedy prefix search.
6. Add beam search fallback.
7. Add final full-target overlay validation.
8. Add unit tests with a fake checker:
   - intra-row MS fixed by one filler type change;
   - inter-row MS fixed by one filler type change;
   - MW fixed only by two simultaneous filler changes;
   - three-VT case where the majority neighbor is not the correct fixed-cell VT;
   - no same-size target master;
   - conflicting fixed std-cell VT constraints;
   - window expansion required;
   - beam budget exhausted.
9. Integrate with the real checker once `checkPlaceWithOverlay` stabilizes.

## 14. Open Questions For Checker Team

- Can `Violation` expose participants with `isFiller`, `instanceId`, row, x range, and implant type?
- Can `CheckResult` classify violations as original, fixed, new, and spillover?
- Can `CheckRequest targetPlace` represent a multi-row x-window, not only one placement point?
- Will `checkPlaceWithOverlay` accept an overlay with multiple filler changes?
- Is there a batch overlay API planned?
- Are replacement masters expected to be passed by us as `MasterId`, or should checker own a `FillerTypeId -> same-size master` lookup?
- Should the checker enforce same geometry, or should it trust repair to provide only legal replacements?
- How large can `targetPlace` be before checker runtime becomes an issue?
