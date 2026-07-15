# fillerRepair infrastructure boundary

更新日期: 2026-07-15。

## 当前结构

真实集成只保留三个组件:

| 组件 | 职责 |
|---|---|
| `InfrastructurePlacementView` | 从 `Network/Node/Master` 建只读 placement 快照;从 `fillerSetting` 取得唯一候选 master 集;缓存同一快照的 full-utility precheck;用稳定 `LeafCellID`/`LibCellID` 索引与 checker 表交叉校验 |
| `CheckerOracleAdapter` | 在 planner wire types 与原生 `ImplantLayerChecker` overlay API 之间转换;不筛选、不去重、不判断 DRC |
| `FillerVtRepair` | 组装 view、oracle 和 engine;解析 target;取得初始 snapshot;把结果映回 `LeafCellID` 和 `PhysLibCell*` |

旧的 `UdmIdBridge`、checker-backed placement snapshot、独立 candidate provider 和独立 precheck cache 已删除。它们的职责全部收敛进同一个 immutable `InfrastructurePlacementView`,避免 placement、coverage、candidate 和 ID 来自不同快照。

## 调用顺序

```text
FillerVtRepair(desMgr, network, checker, fillerSetting)
  -> InfrastructurePlacementView
       -> rows: PhysDesMgr row geometry
       -> instances: Network::getNodes()
       -> masters: Network::getMasters() + fillerSetting
       -> VT metadata: checker.masters()
       -> stable id cross-check: LeafCellID / LibCellID
       -> cached checkSiteCoverage()
  -> CheckerOracleAdapter
  -> FillerRepairEngine(view, oracle)
       -> view.checkSiteCoverage()
       -> view.getUsableMasterCandidates()
       -> Swap generation / rank / subset / oracle gate
```

## 稳定 ID

- planner/checker `InstanceId` = `LeafCellID::getIndexValue()`。
- planner/checker `MasterId` = `LibCellID::getIndexValue()`。
- `ImplantLayerChecker::initFromUDM` 已改用这两个稳定索引;不再重放枚举顺序。
- view 构造时逐项比较 checker 与 infrastructure 的 master、instance、row、x、orientation 和 filler flag;不一致即 `isReady()==false`。

## Candidate 与 precheck

- candidate universe **只来自** `fillerSetting::getFillerMasters()`。
- `PlacementView` 的共享过滤负责:当前实例必须是 filler、候选必须是 filler、同宽同高、VT 已知且不同、排除当前 master、稳定排序。
- coverage 只遍历 infrastructure `Network::getNodes()`;包括 checker 没有 implant shape 的普通 core cell,不再合成负 ID coverage extras。
- multi-height node 会出现在其覆盖的每一行;每行使用自己的 row origin 计算相对 x。

## 仍需集成环境确认

1. dpl2 构建目标加入 `PlacementView.cpp`、`InfrastructurePlacementView.cpp`、其余 fillerRepair sources 与 checker adapter。
2. checker 必须建模 `fillerSetting` 中所有可能的 replacement master。当前 `initFromUDM` 仍从已放置实例收集 master;未实例化候选会触发 `CheckerMissingConfiguredMaster`,需要 RD 提供 master catalog 输入或扩展初始化入口。
3. row legal span 当前来自 row bbox;macro/blockage cutout 若不是由 Grid/row segmentation 提供,应在 infrastructure snapshot 中补齐。
4. 本轮按要求没有编译、没有运行 UDM-dependent tests;首次集成先检查 setup diagnostics,再运行真实 overlay E2E。
