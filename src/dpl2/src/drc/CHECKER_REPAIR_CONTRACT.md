# Checker 改动说明 — 供 filler-repair engine 对接(给 checker RD)

## 终版契约(2026-07-13,用户钉死;应用在 2026-07-13 的 RD 交付上)

**`checkPlaceWithOverlays` 只输出 violation list,不做任何过滤/查重**:

- 每个候选返回 **guard 裁剪后的全量违例列表**。原 blocking 过滤
  (`touchesInstance` ∪ 非 old,`containsViolation` 按 hash/包含判 old)已
  **整体删除**(连同这两个 helper)——它会吞掉"未修好但不触及 target"的
  bridge 残留,对 repair 流是 false accept;old/new 分类是 engine 的职责。
  baseline = 发一个空变更列表候选。
- **不做查重**:同一物理违例可出现多条(每 band/方向各一条)。重复必须
  **确定性**(同输入同 multiplicity);engine 的一对一 multiset 匹配容忍
  一致性重复。
- 其余保持 RD 交付形态:顺序关联(无 requestId/status)、invalid 候选 =
  diagnostics + isLegal=false 且逐候选隔离、`rowIds` 填充、签名 hash。
- 测试锁定:`src/dpl2/src/drc/test/`,9 个用例(-Werror + ASan),含
  `ListReportsResidualWithoutTarget`(bridge 残留必须可见 + 重复确定性)。
- 附带重新套用的三处 scan 正确性修正(guard 语义,见"改动 5"):committed
  上下文不标 candidate、同层接触 interval 不按 provenance 切分、target 不按
  candidate 归属过滤,外加快照按"guard + 最大规则半径"外扩纳入。
- `DPL2_FAKE_UDM` 编译边界重新加上(ctor 的 Session 路径与 `initFromUDM`
  的 UDM extraction 本体),standalone 测试用 `initialize(ImplantInput)` 注入。

以下历史改动(1–6:raw API 引入、rowIds 填充、编译修正、guard overlay 的
target/neighbor/merge 语义、padded-guard 快照、fake-UDM 边界)已全部并入上面的
终版契约或被其取代,细节见 git 历史;所有 checker 侧修改仍带
`[fillerRepair-fix]` 标记,`grep -n "fillerRepair-fix"` 可全部定位。

## 2026-07-15 增补:稳定 ID

`initFromUDM` 的 id 分配改为稳定索引(`InstanceId = LeafCellID::getIndexValue()`,
`MasterId = LibCellID::getIndexValue()`),不再顺次编号——engine 侧
`InfrastructurePlacementView` 依赖该索引与 infrastructure 直接互查,构造时逐项
交叉校验(master 宽高/isFiller、instance 位置/orientation)。**保持两点不变**:
`isFiller = isCoreFiller() || isPadFiller()`(engine 侧用同一谓词);内部查表走
`masterIdToIndex_`,不要引入 "id == vector 下标" 的假设。

## 待 RD 的事项

1. checker 需能建模 `fillerSetting` 中**未实例化**的候选 master(当前
   `initFromUDM` 只从已放置实例收集 master;缺失的候选会被 engine 以
   `CheckerMissingConfiguredMaster` Warning 剔除,修复后自动恢复)。
2. 真实 UDM 环境下跑 `src/dpl2/src/drc/test/` 等价场景验证 extraction。

## 验证(engine 侧持续维护)

- checker harness:`src/dpl2/src/drc/test/run_tests.sh`(-Werror + ASan)。
- planner:`src/dpl2/src/fillerRepair/test/run_tests.sh`(-Werror + ASan)。
