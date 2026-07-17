# fillerRepair/adapter — 统一 infrastructure 边界

更新:2026-07-15(FINAL checker 对齐,AGENTS D28)。

## 唯一组件

`adapter/PlacementView`(一个类,原 FillerVtRepair / InfrastructurePlacementView /
CheckerOracleAdapter 三件套已删除):

- **planner 视图**(`fillerRepair::PlacementView`):行/实例/master/候选查询,
  数据来自 **Grid + Network + PhysDesMgr**(与 checker 相同的依赖);
- **oracle**(`fillerRepair::ImplantOverlayChecker`):**直接调用**
  `ipl::ImplantLayerChecker::checkPlaceWithOverlays`,无中间对象;
- **repair 入口**:`repair(LeafCellID target, const PhysLibCell& newMaster)
  -> RepairOutcome{hasSolution, ipl::FillerChanges, diagnostics}`。

## 与 FINAL checker 的对齐点

| 项 | 约定 |
|---|---|
| id 空间 | `InstanceId = Node::getId()`,`MasterId = Master::getId()`(Network 索引) |
| wire | 候选 = `FillerChanges`(`FillerCellRecord{op=Replace, cell_id_, orig_lib_cell_, new_lib_cell_, origin}` 列表);结果按序关联 |
| 契约 | checker 内部自算空-overlay baseline 并过滤 old(blocking 形态);engine 的 baseline-delta gate 在其上仍一致(快照/baseline/候选同源) |
| Relationship | 只有 IntraRow / InterRow(planner 枚举已同步) |
| 元数据 | checker 只暴露 getNodes/getLayers/siteWidth/getDiags → VT/polarity 由 PhysLibCell implant shapes × `getLayers()` 名字匹配推导(与 checker 同一 parse) |
| init 诊断 | checker 把持久 init diagnostics 前缀进每个结果并计入 isLegal;边界层按 `getDiags().size()` 剥离前缀,只让请求级诊断决定候选状态 |
| 坐标 | RowId = PhysRow 迭代序(含 pad 行);x 相对行原点;colId = x/siteWidth;guard y = rowId*rowHeight 合成系(isInGuard 语义) |
| 线程 | 视图不可变 + call_once coverage;每线程一个 engine;checker 调用在本类内 mutex 串行(其 const 路径改 mutable 计数器) |

## tier-1 本地构建(fake UDM)

`src/dpl2/test/build_all.sh`:编译真实 infrastructure(network/Object/
architecture/Padding/fillerSetting)+ FINAL checker + RD Helper + planner +
本边界,链接 `test/support/grid_link_stubs.cpp`(Grid 行表功能性实现),跑
E2E smoke(`test/smoke_main.cpp`)。fake UDM 数据模型:
`src/drc/test/support/include/fake_udm.h`(DesignDb 可注入 tech/masters/
rows/cells,`activate()` 挂 Session)。

排除项(集成环境才编):`DePlace.cpp`(缺 PaddingChecker/EdgeSpacingChecker/
PlacementDRC 实现)、`Grid.cpp`(需 tbb + PhysNet visitor)。

## 集成环境待确认

1. 真实 `dpl2/PlacementDRC.h` 到位后删 fake(`src/drc/test/support/include/dpl2/`)。
2. `Session/Design/LibAcc` 等 fake 接口拼写与真 UDM 逐一核对(编译即验证)。
3. smoke 的行奇偶约定(偶数行 MX 翻转)与真实设计一致性。
4. macro/blockage 的 rowLegalSpan 多段化(仍未建模)。
