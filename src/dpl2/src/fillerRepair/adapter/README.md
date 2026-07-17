# fillerRepair/adapter — unified infrastructure boundary

Updated: 2026-07-18.

`adapter::PlacementView` is the only production boundary between the pure
planner and dpl2 infrastructure. It implements the planner view, translates
and directly calls the final checker, and exposes `repair()`.

## Authoritative sources

| Data | Source |
|---|---|
| rows, legal spans, site geometry | `PhysDesMgr::getPhysRowIter()` |
| placed status, origin, orientation, physical master | `PhysDesMgr::getPhysCell()` |
| instance/master topology and IDs | `Network` |
| allowed filler masters | `fillerSetting::getFillerMasters()` |
| VT family and band polarity | `PhysLibCell` implant shapes + checker layers |
| DRC legality | final `ImplantLayerChecker` |

The adapter does not mutate the design. Commit belongs to infrastructure.

## Final checker alignment

- `InstanceId = Node::getId()`; `MasterId = Master::getId()`.
- One candidate is `ipl::FillerChanges`, a list of replacement
  `FillerCellRecord`s.
- Results are correlated by candidate input order.
- Checker computes the empty-overlay baseline and returns blocking findings.
- Relationship is `IntraRow` or `InterRow`.
- Adapter synthesizes the planner's internal requestId/status protocol.

## Construction

Use production `RepairInfrastructure` first:

```cpp
RepairInfrastructure infra;
infra.build(desMgr, leafCellIds, fillerSettings, targetNewMaster);
ImplantLayerChecker checker(infra.grid(), infra.network());
adapter::PlacementView view(desMgr,
                            infra.grid(),
                            infra.network(),
                            &checker,
                            &fillerSettings);
auto outcome = view.repair(targetCell, targetNewMaster);
```

`RepairInfrastructure` registers placed, target-new and configured candidate
masters before checker construction, including uninstantiated masters.

## Verified E2E

`src/dpl2/test/build_all.sh` and its CMake equivalent compile:

- real `Grid.cpp` and Network;
- real `RepairInfrastructure`;
- final checker;
- unified adapter;
- production planner.

Fake UDM is the only test-data provider. No Grid stub, RD Helper injection,
fake PlacementDRC, test DePlace/Network shim or `fillerRepair/fake` source is
linked. Normal and ASan executions pass with `-Wall -Wextra -Werror`.

## Lifetime/threading

The view is an immutable one-design snapshot; coverage uses `call_once`.
Concurrent callers use one engine per thread. Checker calls are serialized in
the adapter. Rebuild infrastructure/checker/view after commit.
