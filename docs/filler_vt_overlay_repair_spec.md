# 功能规格 — Filler VT Overlay 修复 V2.1(checker-guided,本阶段 swap-only)

状态:**V2.1 定稿**。分支:`claude/wizardly-carson-secahu`。基线:`2023-base`。
最后更新 2026-07-23。对齐 `src/dpl2/src/fillerRepair` 实现与 `src/dpl2/src/drc`
checker 源码。

V2.1 相对 V2 是一次 reviewer 驱动的修订,聚焦三处高价值改动:**OracleGate 正确性、
窗口模型简化、搜索域建模**。完整修订记录见 [§0](#0-v21-修订记录);V2 相对 V1 的
差异清单见 [§12](#12-与-v1-的差异清单)。V1 存档文件(`spec_v1` / `addendum_v1`)
已作为冗余文档移除,其内容被 §12 差异表完整覆盖。

**一句话**:opto 在任何 cell mutation 前主动调用
`ImplantLayerChecker::precheckFillerRepair()`;
该接口只扫描 placement gap/overlap,失败时以 `isLegal=false` 明确阻断 opto。
`repair()` 在入口重复同一 precheck,失败时返回 warning 和空 changes,作为调用方
漏调或并发变化的安全门。之后 opto/ECO 对一个 std cell 提议 VT/type overlay;
repair 在局部窗口内生成 **swap move**
(同位置同尺寸换 filler master),按启发式排序后**枚举 move 子集并
成批交给 checker overlay API 验证**,用同一 `guardRegion` 下的 baseline-delta clean 作为
唯一 accept 标准。找到 clean 解返回 `ipl::FillerChanges`,由 opto/infrastructure
commit;修不了则返回 diagnostics。`precheck()` 与 `repair()` 都不修改 DB,且
修复成功 commit 后可通过 `update()` 刷新同一实例集合的 runtime engine snapshot。

---

## 0. V2.1 修订记录

V2 定稿并完成最初的 planner 实现后,拿到真实 checker 源码,做了一次 reviewer
复审。以下 12 项修订都在 **filler repair feature 范围内**
(checker 实现不属本 feature 责任);优先级排序为 **OracleGate 正确性 > 窗口简化
> 搜索域建模**。

| # | 类别 | 问题 | 处置 | 落点 |
|---|---|---|---|---|
| 1 | 正确性 | `classify()` 未拒绝 `Checked && !isLegal && violations 空`,可能把未解释的非法结果判 clean | result 自洽性双向判定:`isLegal` 与 `violations 空否` 必须一致 | §5.2、§6.8 |
| 2 | 正确性 | baseline 只查 `status==Checked`,不验证 original 是否复现,可能把"没观察到"误当"已修好" | 搜索前增加 `BaselineMismatch` 门:baseline 必须复现全部 original | §6.8 |
| 3 | 正确性 | delta 匹配非一对一,两条同 signature 的新违例可被一条 baseline finding 一起吸收 | baseline/candidate 作 multiset,消耗式一对一匹配 | §6.8 |
| 4 | 正确性 | pre-existing 的窗口内/相关 violation 被无条件 `continue` 忽略,比 spec 宽松 | 并入 #2 baseline 一致性门(而非在 classify 里对 pre-existing 重判 relatedness) | §6.8 |
| 5 | 正确性 | halo relatedness 的 `ruleDistance` 只取 original 最大 requiredValue,新违例规则更大时被误判 unrelated 放行 | 判新违例时取 `max(原始最大, 该违例 requiredValue)` | §6.2、§6.8 |
| 10 | 正确性 | `anyDefinitive` 用 OR 累积,L0 完备 + L1 截断仍会宣称 definitive | definitive 只按**最后实际搜索窗口**的完备性断言 | §6.7、§6.9 |
| 6 | 简化 | `UnfixableByTypeSwap` 用无 oracle 的 ring 论证做 hard abort,与 checker-as-oracle 有张力,收益极小 | 降级为 Warning 提示,不提前终止,仍走正常搜索(2026-07-15 瘦身:提示整体移除——不影响任何决策路径,无 filler 时搜索本就零 checker call 返回 NoEditableFiller) | §6.2 |
| 7 | 简化 | 单 cluster 下 L2 恒等于 L1、ExpansionCutoff 必触发,名义三级实际两级 | 删 L2,窗口模型改为 L0 + adaptive-L1 | §3.2、§6.3、§7 |
| 8 | 简化 | L1 一次扩到 fixed/core 边界可吞整条 filler run,`3^k>预算` 立即退化为截断枚举 | 改渐进扩窗:每步向 blocking 侧扩 K≈2 个 filler,尽量维持完备枚举 | §6.3 |
| 11 | 简化 | final full-overlay check 用相同 cacheKey,必然 cache 命中,零验证增益 | 删除该步;clean 一经 §6.8 判定即返回 | §6.4、§6.9 |
| 9 | 建模 | 成员上限作用在扁平 swap 列表上,高排名 filler 占满前缀,关键 filler / 第三 VT 可能整体出局 | 先 rank filler、每 filler 保留全部 master domain,再枚举 per-filler 赋值;上限按 filler 数 | §6.6、§6.7 |
| 12 | 建模 | placement gate 与 repair 混在一起,调用责任不清 | 独立公开 `precheck()`:只检查 gap/overlap;opto 在 mutation 前调用并根据 `isLegal` 阻断;repair 入口重复该 gate 作为 fail-closed safety net | §5.4、§6.1 |

落地分三批(实施顺序,详见 `src/dpl2/HandOff.md`):
**批 1 正确性** #1/#2/#3/#4/#5/#10 —— 都是小改动、现有 harness + ScriptedChecker
可造回归;**批 2 简化** #6/#7/#8/#11 —— 减代码,改动集中在窗口相关 case;
**批 3 建模** #9/#12 —— #9 动 Ranker/SubsetSearcher 接口,#12 随 runtime engine 对接。

保留不采纳原文的一处:#4 的原始处方(对 related pre-existing 也否决)会让那条
连 baseline 都有的违例在每个 candidate 里都出现、导致 repair 恒失败;正确修法是把
它并入 #2 的 baseline 一致性门——按 §2.3 假设,窗口内/相关的 pre-existing violation
本就不该存在,发现即报输入异常。

---

## 1. 背景

### 1.1 问题链路与调用流程

1. opto 在任何 cell mutation 前调用 `precheck()`;repair 入口也会重复检查;gap/overlap 时停止后续操作。
2. opto/ECO **一次提议一个 std cell** 的 VT/type overlay(master 替换,位置不变)。
3. 该 cell 周围的 filler 仍保留旧 implant type,在 cell/filler 或 filler/filler 边界上
   产生 implant 层 DRC 违例。
4. checker 对 changed cell 做 target-local 检查,把完整 violation snapshot 交给
   runtime `FillerRepairEngine`。
5. runtime engine 调用 pure planner 搜索 snapshot 中所有 violation 的修复,
   返回 `RepairOutcome`;
   solution wire 是 final checker 的 `ipl::FillerChanges`。
6. opto/infrastructure 负责 commit。runtime engine 与 planner 全程不改 DB。

DRC 规则两类:**MW(min-width)与 MS(min-spacing)**,各分 intra-row / inter-row。
MS 按 P 区 / N 区(band polarity)分别检查;MW 的完整语义由 checker 负责,repair
planner 不依赖其细节(见 §3.1 职责边界)。示例工艺下 MW/MS 的 rule 尺度约为 1 个
site,因此单次修复问题天然**很局部、很小**——这是 V2 搜索策略选型的关键前提。
VT/implant type 有多种(示例 2 种,实际 3 种);算法不依赖具体数目。

### 1.2 一个具体例子

四行 std-cell row,绿=VT1,橙=VT2,filler 宽度 4/3/2/1。`j`(row3)与 `4`(row1)
是 std cell,其余是 filler:

- opto 把 `4` 从绿改橙后,产生 `MS(4,5)`(与 row2 橙 filler `5` 的 inter-row 间距)
  和 `MW(i,d)`(row1 绿 `d` 与 row2 绿 `i` 合并形状在边界处宽度不足)。
- opto 把 `j` 改 VT 后,产生 `MS(f,j)` 和 `MW(2,3)`(row2 橙 `2` 与 row3 橙 `3`)。
- 正确修法通常是把 anchor 周围的**短 filler(bridge filler)整组换 VT**,例如把
  夹在 std cell 与大 implant 区之间的两个 width-2 filler 一起改色——单改任何一个
  都不 clean,必须组合改。这决定了搜索不能假设单调改善。
- 同一 x 间隙可能同时产生 N-band 和 P-band 两条 MS 违例:**violation 不能按几何
  位置去重**。

### 1.3 关键事实

**violation 与 filler 是多对多**。一个 std cell 改 type 可同时引起多条 MW/MS;一个
filler 可同时参与多条 violation;两条 violation 可发生在同一几何位置但由不同
rule/participant 触发;必须一起改的 filler 不一定出现在 participant list 里(bridge
filler)。因此 `Violation` 是 constraint 不是唯一 root cause,repair 的基本单位是
**anchor 邻域的 violation cluster + 候选 filler 集合**,目标是让整个 cluster 在
checker result 中 clean。

**`targetPlace` 是 std-cell anchor,不是 repair window**。它指向被 opto 改动的
std cell 的 candidate placement(instanceId/masterId/rowId/x/orientation),供 checker
重建局部 check context。repair 的"窗口"由 pure planner 生成(§6.3),每个
checker request 额外携带由窗口外扩得到的 `guardRegion`。

**opto 单 cell 内环调用**决定了:一次 repair 的所有 violation 都在同一个 anchor
邻域内,V2 第一版按单 cluster 求解(§6.4);checker call 的总开销 = 每次修复开销 ×
opto 改动次数,所以"好排序让首批候选命中"比"搜索策略高级"更能省时间。

---

## 2. 范围

### 2.1 第一版实现范围(只换 type)

- 只替换 filler master:同一 filler instance 换成同宽、同高、同 row/orientation
  兼容的另一个 filler master(`Swap`,见 §4)。
- 一个 solution 可包含多个 filler 替换。
- 中间候选只通过 checker overlay API 验证,不自己做 DRC 判定。
- 最终输出 `FillerRepairResult`。

### 2.2 不做

- 不移动 std cell,不移动 filler。
- 不改 filler width/height/x/row/orientation(第一版;merge/split 见 §8)。
- 不删 filler、不留空 site、不 split/merge filler instance(第一版)。
- 不在 repair 内 commit DB。
- 不自己做最终 DRC 判定。

### 2.3 假设

- opto 在 mutation 前调用公开 precheck;repair 也强制执行;`isLegal=false` 时不得继续(§6.1)。
- 初始 snapshot 中的 violation 全部视为本次待修对象;不考虑"邻域内存在改动前遗留
  violation"的场景(如未来需要"顺手修历史违例",在 score 中加次级最大化项即可,
  是纯增量,不影响本 spec 结构)。
- checker snapshot 是一次性交付,repair 不要求 checker 维护 violation 历史状态。

### 2.4 Roadmap(影响设计但不在第一版实现)

- **merge/split 修复**:把相邻 filler 合并或把宽 filler 切开来消 violation。
- **multi-height**:跨多行的 cell/filler。
- **batch opto**:一次多个 changed cell。

这三项不进入本阶段代码;演进设计集中在 §4.3、§5.2 决策记录与 §8。

---

## 3. 总体架构

### 3.1 职责边界

- **checker RD**:只做 non-mutating overlay DRC verify。输入一批
  `OracleRequest`(每个是一个 atomic overlay 方案),输出对应的真实 violation
  snapshot。checker 是唯一的 DRC oracle——MW/MS/P/N/PRL 的全部规则细节封装在
  checker 内,pure planner 不依赖这些规则细节。
- **infrastructure RD**:只做 filler 替换候选查询(第一版 per-instance,演进为
  per-span tiling 查询,§5.3)。commit 由 infrastructure 执行,不在 repair 内。
- **checker-owned engine (`FillerRepairEngine`)**:借用 Grid/Network,负责
  precheck、请求校验、private oracle snapshot、update 与 diagnostics;
  `ImplantLayerChecker::check()` 用原始 `CheckRequest` 调用它。它不实现搜索算法,
  也不修改 DB。
- **pure planner (`internal::FillerRepairPlanner`)**:负责 violation 归一化、开窗、
  swap 生成、排序、子集搜索、baseline-delta 判定及搜索结果。它只依赖
  `PlannerDataSource`/`PlannerOracle`,保持 deterministic 且不接触 UDM 生命周期。
  planner unit tests 与其不构造 UDM object 的 doubles 均随 `fillerRepair/test` 迁移;
  portable E2E 使用 final checker/helper 验证 runtime 边界。

第一版代码位置:`src/dpl2/src/fillerRepair/`。

### 3.2 五层管线

pure planner 内部是五层管线,每层单独可测、单独可替换:

```text
┌─ 窗口控制外环(L0 → adaptive-L1,预算尽/近边界则渐进扩窗,§6.3)──┐
│                                                                  │
│  ① Swap   窗口内生成候选 swap                           │
│                    本阶段:仅 swap;未来:+RewriteGenerator       │
│  ② Ranker          按 filler 排序(启发式,只影响评估顺序,不判   │
│                    合法性,§6.6);每 filler 保留全部候选 domain   │
│  ③ SubsetSearcher  按 filler rank 序枚举 filler 子集 × 各自       │
│                    domain 赋值组合,分批产出(§6.7)              │
│  ④ OracleGate      canonical cache + batch checker call +        │
│                    baseline 一致性门 + baseline-delta clean(§6.8) │
│  ⑤ Result/Diag     首个 clean 即返回;预算尽/近边界渐进扩窗;     │
│                    仍无解则 diagnostics(§6.9)                    │
└──────────────────────────────────────────────────────────────────┘
```

选型理由(简单/高效/稳定/可扩展四维度):

- greedy 是 size-1 枚举、group seed 是人工挑选的 size-2/3 枚举、beam 是大子集空间的
  启发式路径——三者统一成"排序枚举"一条代码路径,**没有 partial overlay 打分、没有
  survivor 配额等 magic knob**,只有 clean/not-clean 二值判定,天然免疫 MW 非单调
  陷阱(双 filler 联动在枚举里只是一个普通的 size-2 子集)。
- 批间无串行依赖,batch checker API 可并行;好排序下答案通常在首批 16-32 个候选里。
- 未来加入 rewrite 时,主要扩展①②层;cache key 届时从 instanceId 锚升级为
  span 锚(§4.3),③④⑤层的结构与语义不变。
- beam 保留为 escape hatch(§8.3),第一版不实现。

### 3.3 ImplantLayerChecker、engine 与内部 planner

调用方只接触现有 `ImplantLayerChecker`,不单独构造 engine、PlannerDataSource 或 adapter:

```text
opto: ImplantLayerChecker checker(deplace->getGrid(), deplace->getNetwork())
opto: checker.initFillerRepair(deplace->getDesMgr(), fillerSetting)
  → checker owns engine; engine reuses infrastructure + private oracle/planner snapshot

opto: checker.precheckFillerRepair()         [mutation 前;只查 gap/overlap]
  isLegal=false → opto 阻断
  isLegal=true  → opto 可继续 proposed target overlay

opto: checker.check(node, x, y, orient)      [pre-commit]
  → pass the exact ipl::CheckRequest to engine
  → internal precheck
     → isLegal=false: Warning(PrecheckFailed) + empty changes
  → validate target/type/size
  → final checker: target overlay + empty filler changes
  → 无 violation: success + empty FillerChanges
  → 有 violation: internal::FillerRepairPlanner
       → internal PlannerDataSource/PlannerOracle protocol
       → final checker ordered CheckResult batches
  → bool + checker.getFillerChanges()/getFillerRepairDiagnostics()
opto/infrastructure: commit target + complete FillerChanges
```

checker request 的 master 来自 `Node::getMaster()->getId()`,因此已在 Network 中。
engine 先验证 std-cell/type/width/height;若 DePlace 在 init 后才注册该 master,
则重建 private checker/planner snapshot。direct UDM-handle overload 只供 focused
engine tests,保留 validate-then-lazy-register 行为。snapshot/oracle 全部在
`FillerRepairEngine::Impl` 内。
planner-only `OracleRequest`/`OracleStatus`/`requestId`
abstraction 位于 `OracleGate.h`,只供 `internal::FillerRepairPlanner` 与 unit-test
fake 使用；其 change payload 与 public result 都直接使用 final checker 的
`ipl::FillerChanges`；其中的 `dpl2::FillerCellRecord` 由
`infrastructure/Objects.h` 统一拥有，不存在第二套 change 类型或转换层。
`Types.h` 有意保持为独立叶子，只放 geometry、planner model 与终版 checker wire
helper；record 本身不在 `Types.h`。它不并入 Engine、Planner 或 OracleGate，
避免任一上层反向成为共享依赖。
runtime 签名只使用 final checker 的 `ipl::CheckResult`、`ipl::Diagnostic`、
`ipl::FillerChanges`。`init()` 成功前,
`precheck()`/`repair()` 一律 fail-closed。

checker overlay API 与 runtime engine API 都不修改 UDM/placement。direct test
overload 的 target master 首次注册只扩展既有 Network 的 in-memory master registry,
不修改 DB。repair 拒绝同实例重入;
checker 调用在 runtime engine `Impl` 内串行化。一个 runtime engine 私有拥有一套
checker/planner snapshot、借用一套 Grid/Network,对应一个 design revision。位置/
master commit 且实例集合、rows、blockages 不变时,先由 infrastructure 将 UDM
同步到 Network Nodes,再调用 checker 的 `updateFillerRepair()` 原子替换 private
snapshot。repair engine 不更新 Node。实例增删或 Grid topology 变化时先由
infrastructure 重建 Grid/Network。`update()` 返回 false 后 runtime engine 保持
fail-closed,必须在 infrastructure 同步后成功 update 才能继续查询。

### 3.4 Infrastructure alignment (2026-07-18)

- 移植目的地已经提供 final checker;integration 只包含
  `src/dpl2/src/fillerRepair/` 与 checker `check()` 预留点接线。
  checker-owned engine 借用 supplied Grid/Network、私有持有 checker/planner
  snapshot;UDM 到 Network 的同步由目的地 infrastructure 负责。本项目不修改其
  DRC 算法,也不要求其提供本仓库的 CMake。
- `ImplantLayerChecker(Grid*, Network*)` 是唯一 caller repair 边界;通常直接传
  `DePlace::getGrid()` / `getNetwork()`，再调用 `initFillerRepair()`。
- 单次 `initFillerRepair(PhysDesMgr*, fillerSetting)` 绑定现有 infrastructure,注册 configured
  filler masters 并建立 final-checker/planner snapshot;不再接收 leafCells 或 targetNewMaster。
  row/site/status/origin/orientation 的 authority 是 `PhysDesMgr`。
- `precheckFillerRepair()` 以 supplied `Grid` 的合法 site segments 为 coverage domain,再从
  `PhysDesMgr` placement 与 `Network` cell universe 扫描 gap/overlap。Grid 中
  `Pixel::is_valid && padding_reserved_by == nullptr` 的最大连续区间才要求覆盖;
  hard blockage、fragmented-row hole、halo/padding 与其他合法空白不算 Gap;
  soft blockage 保持 Grid 原有的可放置语义。std cell、filler 与 hard macro 必须
  作为 Network Node 导入并提供 coverage;placement blockage 属于 Grid,不作为 Node。
  它不检查 target、master size、candidate、ID mapping 或 implant DRC。
- candidate universe 只来自 `fillerSetting::getFillerPhysCells()`;repair 内部过滤
  同宽同高、异 VT、filler-only 与相同 bottom-band polarity layout。
- placed instance 是否为 filler 只采用 `Node::isFiller()`；configured replacement
  master 是否进入 planner candidate universe 只采用 fillerSetting allow-list。
  engine init 不再用 UDM macro-type filler flag 交叉否决这两项。非 pad PhysRow
  可以有不同 site height，但每个 height 必须是最小 base height 的整数倍。
- checker/planner instance/master ID 固定为 `Node::getId()` / `Master::getId()`;
  `LeafCellID` / `LibCellID` 是 runtime `FillerCellRecord` handle。
- placed masters 来自既有 Network;configured filler masters 在 init 时注册;
  checker request master 若在 init 后才加入 Network,repair 在验证后触发
  checker/snapshot 重建。infrastructure 先同步现有 Node 的物理状态,
  `updateFillerRepair()` 只重建两套 checker snapshot;repair 不负责更新 Node,
  也不负责发现新增/删除的 UDM instance。
- 82 个可移植 planner unit tests 与 database-free doubles 位于
  `test/` 根目录并与 E2E test 同级;不 include local fake UDM tree/provider。
- 71 个可移植 E2E 位于 `test/FillerRepairCheckerE2ETest.cpp`;通过 final checker 的
  `ImplantLayerCheckerHelper` 直接构造 8-row dense input,不读 DEF/LEF,不需要
  目的地实现 UDM fixture/provider。
- 101 个 checker/engine cases、fake UDM include
  tree/provider/runner 全部位于交付目录外的 `src/dpl2/test/local`;
- runtime 与 portable test 编译清单唯一定义在
  `src/dpl2/src/fillerRepair/sources.cmake`
  (`DPL2_FILLER_REPAIR_SOURCES`);test harness 与移植目的地共用,
  不允许手抄文件列表。


## 4. 核心操作:Swap(本阶段唯一操作)

### 4.1 本阶段需求钉死

本 feature 的操作演进是两步:**第一步 swap(本阶段),第二步 rewrite(未来,
§8)**。没有、也不引入论文式的通用"Move"抽象层——那是文献里的概念,不是本
feature 的需要。代码中不出现 Move/FillerRewrite/adapter 转换,也没有 merge/split
机器(无 span-rewrite 结构、无 tiling 校验)。

planner 的原子操作就是 `Swap`。它在搜索中保留紧凑的 instance/master id 与
排序/几何元数据；发给 checker 或返回调用方时，由 `PlannerDataSource` 生成完整
`dpl2::FillerCellRecord`:

```cpp
struct Swap
{
    InstanceId instanceId = 0;
    MasterId oldMasterId = 0;
    MasterId newMasterId = 0;  // same-width / same-height filler master
    RowId rowId = 0;
    XInterval span;            // 该 filler 的占位区间
    VtId oldVt = kUnknownVt;
    VtId newVt = kUnknownVt;
};
```

`rowId`/`span` 不是 merge/split 预留:开窗(§6.3)、bridge filler 识别(§6.5)、
delta 相关性判定(§6.2)、swap-unfixable 快速判定(§6.2)都是几何计算,是 V1
自身的需要。不存在 `Swap.change()`、独立 adapter 或第二套 change struct。

### 4.2 overlay cache key

- 一个 **overlay 候选** = 一组 Swap 的集合,原子应用。
- cache key = `sorted_unique((instanceId, newMasterId))`,order-independent。
  checker call 前必须用它去重并查 cache;cache value 至少含 `OracleResult`、
  score 摘要和 checker call index。同一 key 的 overlay 在一次 repair 内只调用
  一次 checker。
- **不需要冲突判定机制**:③层枚举按 filler 分组(每个 filler 至多选一个目标
  VT),"同一 instance 出现两次"按构造不可能发生。

### 4.3 未来演进(不在本阶段)

第二步 rewrite(merge/split)需要"span rewrite(区段重铺)"抽象与
primitive-op 形态的 v2 checker API;设计、职责归属与升级时机见 §5.2 决策记录和
§8.1,multi-height 见 §8.2。本阶段代码中不出现这些概念。

---

## 5. 接口

checker API 与 infrastructure API 仍可协商修改;本节是推荐形态。凡与 V1 一致的
协议约定原样保留。

### 5.1 共享基础类型

```cpp
struct TargetPlace   // 实现中可继续命名为 CheckRequest,语义必须是 std-cell anchor
{
    InstanceId instanceId = 0;  // opto changed std cell
    MasterId masterId = 0;      // new/candidate std-cell master
    RowId rowId = 0;
    DbCoord x = 0;
    PhysOrientation orientation = PhysOrientationE::R0;
};
```

`Violation` 结构与 V1 相同(推荐字段:`ruleId`,`kind(MinWidth/MinSpacing)`,
`relation(IntraRow/InterRow)`,`primaryLayer/secondaryLayer`,
sorted-unique `rowIds`,`xWindow`,`measuredValue/requiredValue`,`participants`)。
checker 侧必须保证:

- `rowIds` 提供且 sorted unique;inter-row violation 包含所有涉及行。
- `ViolationParticipant` 含 `instanceId/masterId/rowId/xRange/isFiller/isTarget`,
  其中 `rowId` 用于区分同 x 位置、不同 row/participant 的 violation。
- violation 不需要 stable id;repair 侧自己生成临时 signature(§6.2)。

### 5.2 Planner 内部 checker oracle API

```cpp
using OracleRequestId = int;

struct OracleRequest
{
    OracleRequestId requestId = -1;  // repair 生成,batch 内唯一
    TargetPlace targetPlace;
    Rect guardRegion;                 // repair window 外扩 two-cell guard halo
    ipl::FillerChanges fillerChanges;  // 一个 atomic overlay 候选
};

enum class OracleStatus { Checked, InvalidOverlay, CheckerError };

struct OracleResult
{
    OracleRequestId requestId = -1;  // 必须 echo 请求的 requestId
    OracleStatus status = OracleStatus::CheckerError;
    bool isLegal = false;             // 仅 status == Checked 时有意义
    std::vector<Violation> violations;
    std::vector<Diagnostic> diagnostics;
};

class PlannerOracle
{
 public:
  OracleResult checkPlaceWithOverlay(const OracleRequest& request);
  std::vector<OracleResult> checkPlaceWithOverlays(
      const std::vector<OracleRequest>& requests);
};
```

planner 的 `Swap` 是搜索元数据；`PlannerDataSource::fillerCellRecord()` 在创建
oracle request 时一次性生成终版 checker wire。`OracleRequest`、planner
result、checker batch 和 `RepairOutcome` 全程复用同一个
infrastructure-owned `FillerCellRecord`。

#### 5.2.1 checker 实际交付形态(终版契约,2026-07-18)

checker 已交付真实 overlay 实现,UDM extraction 内联。**对接以实物为准**,
上面的接口只描述 runtime engine 内部的 planner-oracle adapter 抽象。终版 checker 对每个 batch 自算空 overlay
baseline,再返回每个候选的 **blocking violation list**。

```cpp
// infrastructure/Objects.h (namespace dpl2)
enum class OpType : uint8_t {
  Replace = 0,
  Delete = 1,
  Add = 2,
};

struct FillerCellRecord {
  OpType op_;                       // 本阶段为 Replace
  eUNL::LeafCellID cell_id_;
  eUTL::UvDist origin_x_;
  eUTL::UvDist origin_y_;
  eLIB::LibCellID orig_lib_cell_;
  eLIB::LibCellID new_lib_cell_;
};

// drc/ImplantLayerChecker.h (namespace dpl2::ipl)
using FillerChanges = std::vector<dpl2::FillerCellRecord>;

// batch = 单 target + 单 guardRegion + N 个候选;结果按输入顺序一一对应
// (没有 requestId:顺序即关联)。CheckRequest 用 colId(site 单位)。
std::vector<CheckResult> ImplantLayerChecker::checkPlaceWithOverlays(
    const CheckRequest& request,
    const Rect& guardRegion,          // eUTL::Rect(UDM 类型)
    const std::vector<FillerChanges>& fillerChanges) const;

struct CheckResult {                  // 没有 status 枚举
  bool isLegal;                       // = blocking 为空 && diagnostics 干净
  std::vector<Violation> violations;  // guard 内 blocking list
  std::vector<Diagnostic> diagnostics;  // invalid 候选 → 诊断 + isLegal=false
};
```

终版契约要点:

- **blocking**:checker 先用同一 target/guard 计算空-overlay `oldViolations`,
  再保留触及 target 或不被 old baseline 包含的候选 violation。planner 的
  `OracleGate` 仍保留 baseline-delta gate;original snapshot、baseline 与 candidates
  必须都经同一 runtime engine/checker 路径,保证分类一致。
- **requestId/status 取消**:关联按顺序(结果数==输入数、序一致);
  invalid 候选以 diagnostics 表达(dup/非 filler/尺寸不符等),逐候选隔离。
  runtime engine private boundary 按 index 合成 planner id/status。
- **batch 形态**:单 target/guard + N 变更列表,与 planner 每窗口的 batch 用法一致。
- `Violation` 带 `rowIds` 与 hash;`Relationship` 只有 IntraRow/InterRow。
- checker ID 空间 = `Node::getId()` / `Master::getId()`;physical wire =
  `LeafCellID` / `LibCellID`。
- 快照纳入按 guard 外扩最大规则半径(结果仍按精确 guard 裁剪),避免跨 guard
  边界的 run 被截断产生伪违例(细节见
  `src/dpl2/src/drc/CHECKER_REPAIR_CONTRACT.md`)。

**future work(merge/split 前置条件)与职责归属(决策记录)**:届时 API
需要一次版本化升级。"在 overlay context 里删除/实例化 filler"拆成两半,归属
不同:

- **语义半 = planner 做**:把一个 span rewrite 翻译成"删哪些 instance、
  在哪些 x 放哪些 master"。tiling 规划知识(库宽度、切法、每段位置)全在
  planner,①生成器本来就要算出精确位置;让 checker 解释 tiling 意图
  会把规划知识泄漏进 oracle。
- **机械半 = checker 做**:把删/加操作应用到 checker 内部 candidate context
  (candidate intervals、merged shapes)并跑规则——这些索引是 checker 内部的,
  只能它做;且 V1 的换 master 内部本来就是 remove + add,这是已有机制的直接
  推广,不是新语义。

由此推论,**v2 wire format 推荐 primitive-op 形态而非 span 形态**:repair 发
`remove(instanceId)` 若干 + `add(masterId, rowId, x, orientation, overlayId)`
若干;`overlayId` 是请求方分配的 request-scoped 合成 id,checker 在
`ViolationParticipant` 中 echo 它——这直接消解了"overlay 新建 filler 没有
instanceId、participant 无法引用"的协议难题(id 分配权归请求方)。checker 侧
保留防御性校验(removed span 的并集必须等于 added tiles 铺出的并集,否则
`InvalidOverlay`),但不解释 tiling 意图。op 形态还与 commit 路径同构:
infrastructure 的 commit 原语同样是删/建 instance,solution 可直通 commit。
span rewrite(`FillerRewrite`)届时作为 planner 内部抽象一并引入,对外始终翻译
成 op 列表;本阶段(swap-only)代码中不存在该结构。

**为什么不现在就升级(决策记录)**:V1 所有 move 都是同尺寸换 master,
`FillerCellRecord` 对 swap 零歧义、零新增工作量;multi-height 的行对齐语义、commit 侧
id 分配流程等契约细节仍依赖未定稿的设计,现在拍板是投机性 API。merge/split
落地时 checker 反正要做机械半的推广实现,API 升级与其同批完成,不产生额外
成本。现在只要求 checker RD 两点:① 接口版本化预留(升级时新增 v2 接口而非
原地改语义);② 不把"overlay 不会创建/销毁 instance"的假设固化到 checker
内部深处。

协议约定:

- non-mutating,不 commit DB。
- 一个 `FillerChanges` 是 atomic candidate,必须一起 overlay 后检查;batch API
  是单 target + 单 guard + 有序 candidate vector。
- 每个 batch 必须携带 `guardRegion`;checker 至少在该区域内 collect violations。
  `guardRegion` 只扩大 checking/diagnostics 范围,**不允许** repair 修改 guard-only
  区域内的 filler。
- `CheckResult` 按输入顺序关联;结果数不等于候选数即 protocol error。
  runtime engine private boundary 按 index 合成 planner request ID/status。
- 单个 invalid candidate 只影响自己的 `CheckResult`,并以 diagnostics 表达。
- planner 内部结果仍做双向自洽校验;fatal/request diagnostics 使该候选不可用。
- checker 不需要维护 violation 历史,也不做 original/residual/new/spillover 分类;
  分类由 repair 侧用同一 `guardRegion` 的 baseline/overlay delta 完成(§6.8)。
- `checkPlaceWithOverlay[s]` 是**纯查询**(const、不 mutate DB),禁止在内部
  触发 repair——只有顶层 `ImplantLayerChecker::check()` 可进入 engine;engine
  的 private oracle 只调用 overlay API,避免递归 callback(依赖拓扑见 §3.3)。

### 5.3 Infrastructure 候选 API

第一版(per-instance,与 V1 相同):

```cpp
struct MasterCandidateRequest { InstanceId fillerInstanceId = 0; };
struct MasterCandidate       { MasterId masterId = 0; };

struct MasterCandidateResult
{
    std::vector<MasterCandidate> candidates;
    std::vector<Diagnostic> diagnostics;
};

class PlannerDataSource
{
 public:
  virtual const std::vector<MasterId>& fillerMasterIds() const = 0;
  virtual MasterCandidateResult getUsableMasterCandidates(
      const MasterCandidateRequest& request) const;
};
```

语义(与 V1 相同):输入必须是 filler instance;candidates 只含可直接替换的
filler master(不含当前 master),保证 same width / same height / same site /
orientation 兼容;没有可用替换时返回 empty candidates + diagnostics,不是 error。
当前工艺下每个 filler 恒有 2 个同尺寸候选(3 VT − 当前,附录 A);
empty candidates 保留为防御性路径,不是常态。

runtime engine 的候选 universe 必须由
`fillerSetting::getFillerPhysCells()` 取得,再解析为 `Network::Master::getId()`;
不得从 placed-instance 枚举猜测,也不得解析 master 名。configured master 即使
尚未实例化也必须在 checker 构造前注册进 Network。

**band polarity layout 约束(2026-07-15 落地)**:候选还必须与当前 master 的
**R0 系 bottom-band polarity** 一致(`MasterInfo.bottomBandPolarity`,来源 =
checker `rebuildMasterShapes` 的锚定规则:最底 shape 所在 `Layer::Polar`;
band 沿行向上 N/P 交替;`Layer::Vt` 每 master 唯一,checker
`master_implant_family_mismatch` 保证)。理由:swap 保持位置**和 orientation**,
layout 相反的候选会把每个 band 落到相反 track 上,checker 必然以 polarity
mismatch 拒绝——提供它只烧 checker call。合法性终判仍在 checker(oracle 不变)。

**演进方向**(merge/split 需要,提前告知 infrastructure RD):查询按几何键而非
instance 键——"在 `(rowId, width, orientation)` 下有哪些可用 filler master(或
master 序列)可铺满该宽度"。第一版实现建议内部就按宽度建表,per-instance API 做成
它的 wrapper,升级时零迁移。

### 5.4 Checker entry 与 placement precheck

```cpp
ImplantLayerChecker(Grid* grid, Network* network);
bool initFillerRepair(PhysDesMgr* desMgr,
                      const fillerSetting& fillerSetting);
void setFillerRepairDebugLogging(bool enabled);
ipl::CheckResult precheckFillerRepair() const;
bool check(const Node* node, GridX x, GridY y,
           const PhysOrientation& orient) const;
const ipl::FillerChanges& getFillerChanges() const;
const ipl::DiagVec& getFillerRepairDiagnostics() const;
bool updateFillerRepair(PhysDesMgr* desMgr,
                        const fillerSetting& fillerSetting);
```

checker constructor 直接借用 dpl2/DePlace 已初始化的 Grid/Network;
`initFillerRepair()` 验证它们与
PhysDesMgr/fillerSetting/active UDM design 一致,注册 configured filler masters,
然后构造 final checker 和 runtime snapshot。调用方不提供 hierarchy leaf list,
也不构造 repair-specific infrastructure/checker。`check()` 直接传入 Network 已有的
request master;repair 先验证 target/type/width/height,若 master 是 init 后加入则重建
private checker/snapshot。实例集合不变的 placement/master
commit 后,先由 infrastructure 同步 Grid/Network,再调用
`updateFillerRepair()` 刷新两套 checker snapshot;repair 不更新 Network Nodes。
新增/删除 instance 或 row/blockage 改变时必须先重建 Grid/Network,而不是复用旧
snapshot。

final checker 的 persistent init diagnostics 在接受 oracle 前分类：
`skipped_phys_status`，以及 Network 中所有 master 都未使用的 implant layer 的
`missing_rule_parameter`/`skipped_missing_rule_parameter` 是非阻断提示；其余 init
diagnostics（包括实际使用层缺规则、shape/band/placement 结构问题）使 `init()`
fail-closed。初始化成功后，runtime engine 才从每个 overlay result 的前缀中移除这一整段已批准
persistent diagnostics，request-specific diagnostics 不移除。

`precheckFillerRepair()` 的 contract:

- `isLegal=true`:合法 row segments 内没有 gap/overlap;
- `isLegal=false`:至少一个 gap 或 overlap,diagnostics status 为 `Gap`/`Overlap`,
  message 是 warning 并明确 opto 必须阻断 mutation;
- 不检查 target、master size、candidate、ID mapping、site alignment、越界分类或
  implant DRC;
- 不修改数据库。opto 负责在任何 cell mutation 前主动调用并解释返回值。

`check()` 把自己构造的 `CheckRequest` 原样交给 engine;engine 在注册
replacement master 前先对初始 target influence 重复 precheck。adaptive candidate
若编辑初始范围之外的行，则该 request 在进入 checker batch 前扩展并补查对应行；
非法 request 不会连带拒绝同 batch 的合法 request。通过后以 request master 作为 target overlay,第一次
implant DRC 使用空 `FillerChanges`;无 violation 返回 success + empty changes,
有 violation 才进入内部 planner。有解返回 `ipl::FillerChanges`,无解返回失败;
commit 始终属于 opto/infrastructure。runtime diagnostics 和 wire types沿用
final checker,内部 planner request/status 不能越过 runtime engine boundary。

---

## 6. 修复流程

### 6.1 Placement precheck(opto-owned gate)

`precheckFillerRepair()` 只回答一件事:每个需要 placement coverage 的合法 row segment 是否
恰好覆盖一次。coverage domain 由 supplied Grid 定义:逐行扫描 site pixel,
只把 `is_valid && padding_reserved_by == nullptr` 的最大连续区间纳入检查。
因此 hard blockage、fragmented-row hole、instance/master/global halo/padding 以及
infrastructure 标记为不可放置的合法空白被排除,而不是按完整 row bbox 误报 Gap。

在每个合法 segment 内,UDM placed/fixed cell span 被独立裁剪并做 coverage sweep;
零覆盖区间报告 `Gap`,多重覆盖区间报告 `Overlap`;两者均使
`ipl::CheckResult::isLegal=false`。diagnostics 是 warning 级信息,但 bool contract
是 hard gate,opto 必须停止后续 mutation。

precheck 不解析 target/new master,不枚举 candidate,不检查 Node/Master ID mapping,
不调用 ImplantLayerChecker,也不把 off-grid/越界/其他 placement legality 单独分类。
它是 non-mutating query。opto 应在 mutation 前主动调用以尽早阻断;`repair()`
按初始 target influence 与实际 adaptive filler-change rows 执行局部 safety gate。
multi-row object 对每个垂直重叠 row 提供 coverage。失败时
`RepairOutcome.hasSolution=false`,changes
为空,保留 `Gap`/`Overlap` diagnostics 并追加 warning `PrecheckFailed`,不会进入
target master 注册、checker 或 planner。

### 6.2 violation 归一化与 signature

对每条 violation 抽取:`type`(MW/MS)、`relation`(intra/inter-row)、`rowIds`
(为空时退回 `targetPlace.rowId` 并在 diagnostics 标记)、`xRange`(xWindow 与
participant bbox 的 union)、`vtHint`(MW 用 primary layer,MS 用 primary/secondary
layer,**按 band-slot / P-N 区分别记**)、`cellAnchors`(至少含
`targetPlace.instanceId`,再加参与或邻接的 fixed std cell)、`fillerParticipants`。

**signature 匹配键(钉死,delta 分类依赖它)**:

```text
signature = (ruleId, kind, relation, primaryLayer, secondaryLayer,
             sorted(rowIds), xWindow 重叠 ≥ 阈值)
```

两条 violation 跨 baseline/overlay result 匹配,当且仅当所有 id/enum/layer 字段
相等且 xWindow 重叠超过阈值(建议:重叠长度 ≥ min(两者长度) 的一半,或距离 ≤
1 site)。

**为什么必须带 layer**:MS 分 P 区 / N 区,同一个 x 间隙可能同时产生 N-band 和
P-band 两条 MS,它们的 ruleId/kind/relation/rowIds/xWindow 可能完全相同,只有
implant layer 不同。若签名不含 layer,这两条会被误当成一条,直接违反"不按几何位置
去重"(§1.2/§1.3)。因此 `primaryLayer`(MW)与 `primaryLayer/secondaryLayer`
(MS)必须进签名;checker 未填 layer 时字段默认 0/空,对 layer-agnostic checker 是
no-op,不影响行为。

**"与本次改动相关"的定义(钉死)**:violation 的 participants 触及任一 changed
span,或其 xWindow 距任一 changed span ≤ 1 个 rule distance。以 span 几何为锚而非
instanceId,保证 merge/split 时代 instance 被销毁后判定依然成立。判定**新** violation
的 relatedness 时,rule distance 取 `max(原始违例最大 requiredValue, 该 violation
自身 requiredValue)`——新违例可能来自 requiredValue 更大的规则,只用原始值会把它
误判成 unrelated 而放行(V2.1 修订 #5)。

**swap-unfixable 判定(已移除)**:V1 曾借鉴 DAC'23 [Zou et al.] 用 "violation
外扩 two-cell ring 内无 editable filler" 做 hard abort;V2.1 修订 #6 因其无 oracle
佐证降级为 Warning 提示;2026-07-15 瘦身把提示也移除了——它不影响任何决策路径
(无 filler 时搜索本就 `NoEditableFiller`、零 checker call 返回无解),属于最低
spec 要求之外的功能。不可修的终判只有一个来源:④OracleGate 的正常搜索结果。
这类 case 通常意味着需要 merge/split(§8.1)或上游回退该 opto 改动。

### 6.3 开窗:L0 → adaptive-L1 与 guardRegion

窗口用于圈定 candidate filler、限制搜索、和 delta 分类;它不是 `targetPlace`。
两级模型(V2.1 修订 #7,删去了 V2 的 L2):

| 级别 | 内容 |
|---|---|
| L0 | violation participants ∪ anchor std cell 左右相邻 filler ∪ bridge filler(见下)——最小 participant/bridge 窗口 |
| adaptive-L1 | 受控渐进扩窗:每步优先向"当前 best 非-clean candidate 的 blocking violation 所在侧"扩入固定 K 个相邻 filler(K≈2,纵向含耦合相邻行 ±1);若该单侧无法加入 filler,确定性尝试反方向;重算完备枚举判定并循环到扩窗截止 |

- intra-row violation 从 L0 入,inter-row 从含 ±1 行的 L0 入(bridge 集合天然覆盖)。
- 扩窗触发:result 非 clean 且 remaining violation 的 xWindow/participants 靠近当前
  窗口边界;或当前窗口枚举/预算耗尽仍无 clean——每步优先向 blocking 侧扩 K 个
  filler;该单侧被阻塞时再尝试反方向。
- **为什么渐进扩窗而非一次吞整段 filler run(V2.1 修订 #8)**:早期 L1 会横向 snap
  到最近 fixed cell/blockage/core 边界,可能一次把整条 filler run 拉进窗口——editable
  filler 一多,`3^k > 预算`,完备枚举立刻退化成 size≤4 的截断枚举,"窗口更大"反而
  "更难找到解"。按 best-residual 的 blocking 侧逐步扩,让窗口只在确需的方向增长,
  尽量维持完备枚举区间。
- **扩窗截止**:主方向与必要的反方向 fallback 均未引入任何新的 editable
  filler/move 时停止扩窗并进入失败路径。remaining violation 集合在完备枚举后
  不变**不是**安全的截止证明:implant 合法性是非单调的,更远 filler 仍可能参与
  三个及以上的 atomic swap 解。
- **层数安全阀**:`RepairConfig::maxAdaptiveLevels`(默认 32)限制无解场景的
  扩窗层数上限——没有它,长 filler run 上的无解 case 会一直扩到 filler 耗尽
  (层数 ~ filler 数/(2K),每层最多一个 window 预算的 checker 调用)。触发上限
  按既有 **truncated** 语义收尾(不得声明 definitive),仍然不返回 partial。
- multi-height 预留规则:窗口按行扩展时,跨行 instance 把它占用的所有行拉进同一窗口。

**bridge filler 默认必选**(不是兜底):与 anchor std cell 左右接触的 filler、上下行
中与 anchor x-boundary 对齐的 filler、宽度 ≤ rule window 或明显短于相邻大 filler 的
短 filler、夹在两个不同 VT 大区之间的短 filler run——即使不在任何 participant list
中也进入候选集。§1.2 的例子里正解就是这类 filler。

每次确定 repair window 后计算:

```text
guardRegion = expandByCellRing(repairWindow, 2)
```

即 two-cell guard halo:横向包含窗口左右各两圈相邻 placed instance,纵向包含上下各
两条相邻 row 中与窗口 xRange 相交或贴近的 instance。只能用几何距离近似时,必须取
不小于 two-cell ring 的 conservative expansion,并在 diagnostics 打印实际
`repairWindow`、`guardRegion` 与 halo 来源。同一窗口生成的所有 `OracleRequest`
携带同一个 `guardRegion`;guard-only 区域的 filler 只参与 checking/diagnostics,
不得出现在 `fillerChanges` 中(违反判 invalid request)。

### 6.4 cluster:第一版单 cluster

opto 一次只改一个 cell,本次 snapshot 的所有 violation 都在同一 anchor 邻域内。
第一版直接把它们并成**一个 cluster、一个(逐级放大的)窗口**一次求解,等价于 V1
从合并窗口起步,但删去了"cluster 独立求解 → final merge → conflict-graph retry"
整套机制。该机制在 batch opto(多 anchor)时代再引入(§8.4)。

**V2.1 修订 #11:删除单独的 final full-overlay check**。单 cluster、单 guardRegion
下,该检查与搜索中判定为 clean 的那一次 overlay 用完全相同的 cacheKey,必然 cache
命中、classify 输出相同,零验证增益。因此获胜 overlay 一经 §6.8 的 baseline-delta
门判为 clean 即作为解返回,不再重复一次。真正有增益的"扩大 guard 的独立认证"需要
为新 guard 重跑 baseline(每次 +2 call),留到 batch opto / 多窗口合并(§8.4)真正
引入多 guard 时,按实测预算决定是否加回。

### 6.5 Swap 生成器(本阶段唯一的 move 生成器)

生成器是纯枚举,**只产出原子 swap move,不产出任何组合/种子/hint**:

1. 对窗口内每个 editable filler 调 `getUsableMasterCandidates`;为空则该 filler
   不生成 swap,diagnostics 记 `NoUsableMaster`(正常结果,不是 error)。
2. 对每个 candidate master 生成一个 `Swap`(§4.1),master 到 VT 的映射由
   runtime engine 的 private planner snapshot 提供,供排序使用;通不过构造校验的候选记 diagnostics 后跳过
   (defense in depth)。
3. 绝不为 std cell、macro、guard-only filler、non-editable filler 生成 swap。
4. 输出定序:窗口 editable 顺序(row, x),同 filler 内按 master id 升序。

**组合不在生成器做**:多 filler 联动方案由③层 SubsetSearcher 枚举 size-2/3 子集
自然产生(§6.7);anchor-follow 方向(把 anchor 相邻/bridge filler 换成 anchor
新 VT)由②层 Ranker 的排序实现(§6.6)——排序把该方向的 swap 排最前,首批
size-1/2 子集就等价于 anchor-follow 组合,不需要独立的种子注入机制。

### 6.6 排序(Ranker)

排序只影响 checker call 次数,不影响正确性。第一版 5 特征,lexicographic 比较:

| 优先级 | 特征 | 含义 |
|---|---|---|
| 1 | `directParticipant` | filler 出现在 violation participants 中 |
| 2 | `bridgeScore` | 位于 anchor 与相邻大 implant 区之间的短 filler |
| 3 | `cellAnchorVote` | targetVT == anchor std cell 的新 VT(本问题最常见正解方向) |
| 4 | `width` | 更窄优先 |
| 5 | `position` | x 更小优先,再 row 更小,保证 deterministic |

**排序作用在 filler 上,不是扁平的 swap 上(V2.1 修订 #9)**:先按上表对窗口内
editable filler 排序,得到 filler 优先序;每个入选 filler **保留其全部候选 master
作为 domain**(当前库恒 2 个,附录 A)。③层枚举的是"filler 子集 × 各自 domain 的
赋值",成员上限(§6.7)因此作用在 filler 数而非候选数上——不会再出现"高排名
filler 的两个 master 占满 rank 前缀,导致某个关键 filler 或它的第三 VT 完全不进入
组合"的问题。

target VT 顺序(决定各 filler domain 内的排序):① anchor 的新 VT;② 邻接 majority
VT(**按 band-slot 分别计数**,不能按 cell 整体数);③ 稳定 type id。所有顺序都只在
candidate provider 实际返回的 master 中取值(防御库变化;当前库各宽度 VT 齐全,
附录 A)。

per-band 计数的落地形态(2026-07-15,依据 checker 实测模型):master 的 VT
family 在 band 间**恒一致**(checker `master_implant_family_mismatch` 保证),
band 间的自由度只有 polarity——因此 per-band 计数体现为**权重**:同行贴邻邻居
与 filler 在上下两个 band 都相邻,记 **2 票**;±1 行邻居只经跨行边界的一对
facing band 交互(checker `activeKindByBoundary`),记 **1 票**。tie 仍取
较小 VT id(确定性)。

**第三 VT 强降权(V2.1 修订 #9:改为 domain 内排序)**:三档 VT 下,每个 filler 的
两个候选中总有一个"既非 anchor 新 VT、也非邻接 majority"的第三色,几乎不可能是解
的一部分。它不再作用在扁平的全局 swap 列表上,而是**在该 filler 自己的候选 domain
内排到最后**(降权,**不剔除**,三 VT 相邻的犄角场景仍可达)。有效分支因子由此从
2 降到 ~1;配合 §6.7 的完备枚举,正确性完全不依赖该启发。

fixed cell 是投票和约束,不是禁改理由(贴着 fixed cell 的 filler 往往最该先试)。
V1 的其余特征(`fillerVote`/`diffEdgesRemoved`/`multiViolationTouch`/`islandScore`/
`sameVtBefore`/`cellConflict`)列为 backlog,fake-checker 测试显示排序命中率不足时
再逐个引入;merge/split 时代加"操作类型偏好(swap 优先于 rewrite)、changed span 面积
小者优先"两项。backlog 还包括 **model-guided proposal**(DAC'23 的 inference /
forced-assignment 思想:用近似局部规则模型推导"该 filler 必须是某色否则必然
违例"的强制赋值,用于排序与剪枝)——anchor-follow 种子与第三 VT 降权正是它的
弱化版;模型不准只多花 checker call,正确性始终由 ④OracleGate 保证。

### 6.7 子集搜索(SubsetSearcher)

从排序后的 move 列表 `m1..mM` 枚举 overlay 候选:

- **枚举顺序** = 按 `(子集 size, 成员 rank 的字典序)`。Ranker 把 anchor-follow
  方向的 move 排最前(§6.6),因此首批 size-1/2 子集天然覆盖 anchor-follow
  组合——没有独立的种子/hint 注入机制(§6.5)。
- **小窗口完备枚举(优先规则)**:窗口内 editable filler 数为 k 时,完整子集空间
  = 3^k 个着色方案(每个 filler 恒 2 候选,附录 A)。当 `3^k ≤ 剩余预算` 时
  (k ≤ 5 对应 243 ≤ 512),**完整枚举全部非冲突子集**,仍按 rank 序分批、首
  clean 早停。此时"窗口内无解"是**确定性结论**——扩窗触发精确、不可能漏解,
  排序只影响速度不影响完备性,`hasSolution=false` 的诊断含义从"预算耗尽"升级为
  "窗口内确定无解"。**但完备只对当前窗口成立(V2.1 修订 #10)**:L0 完备只证明
  L0 无解,不能证明后续被截断的扩窗窗口无解;因此"definitive no solution"必须以
  **最后一个实际搜索过的窗口**是否完备为准,而不是"任一较小窗口曾完备"(见 §6.9)。
- **成员限制(仅大窗口,V2.1 修订 #9)**:`3^k > 剩余预算` 时启用截断——size 1
  允许全部 filler;size ≥ 2 限制在 rank 前 `N_s` 个 **filler**(不是 swap)中组合
  (建议 `N_2 = 24`,`N_3 = 12`,`N_4 = 8`;size > 4 不枚举,渐进扩窗);每个入选
  filler 仍展开其完整 domain。
- 跳过含冲突 move 的子集;canonical key 去重、查 cache(§4.2)。
- **分批验证**:每批 16-32 个候选发 batch checker;批内按枚举序取第一个
  delta-clean 作为解(保证确定性),命中立即停止。
- **预算**:每窗口 checker call 上限 512(含 baseline)。预算耗尽或枚举完仍无
  clean → 渐进扩窗(L0 → adaptive-L1,§6.3);扩窗截止 → 失败路径(§6.9)。

MW"必须多 filler 联动、单改不改善甚至更差"的非单调 case,在这里只是一个普通的
size-2/3 子集,不需要任何特殊机制;好排序下通常出现在首批。

### 6.8 accept gate:baseline-delta clean(唯一 accept 标准)

对每个窗口/guardRegion,先发一次 **baseline request**(同 `targetPlace`、同
`guardRegion`、空 `fillerChanges`)。

**baseline 一致性门(V2.1 修订 #2+#4,搜索前必过)**:baseline 必须能复现输入
snapshot——每条 original violation 都应在 baseline result 中按 signature 出现;且
baseline 的 repairWindow 内 / 与 anchor 相关的 violation 不能多于这些 original
(§2.3 假设:输入快照除待修 original 外是干净的)。任一条不满足 → 输入 snapshot
已过期 / guard 不一致 / checker 字段漂移,repair 以 `BaselineMismatch` fatal 中止
并请上游重出快照,而不是把"没观察到 original"误当成"已修好"。halo 内 baseline
已有且与本次改动无关的历史违例不触发此门(它们本就允许存在)。

过门后,candidate 判 **delta-clean** 当且仅当:

1. `status == OracleStatus::Checked`、无 checker fatal/protocol diagnostics,且结果
   自洽——`isLegal` 与 `violations 是否为空` 必须一致(V2.1 修订 #1:`isLegal &&
   violations 非空` 与 `!isLegal && violations 空` 两种矛盾形态都判不 clean);
2. 初始 snapshot 的 original violations 在 overlay result 中全部消失
   (signature 匹配,§6.2);
3. `repairWindow` 内没有新增 violation(overlay 有、baseline 无);
4. guard halo 内没有"与本次改动相关"(§6.2 定义,rule distance 取 per-violation
   max)的新增或迁移 violation;
5. guard halo 内 baseline 已有且与本次改动无关的 violation **不导致失败**,只进
   diagnostics。

**"新增" = baseline↔candidate 一对一 multiset delta(V2.1 修订 #3)**:判断
candidate 里哪些 violation 是新增时,把 baseline 与 candidate 的 violation 各视为
multiset,按 signature 做**消耗式一对一匹配**——一条 baseline finding 只能抵消一条
candidate violation。否则两条同 signature 的 candidate violation 会被同一条 baseline
finding 一起"吸收",其中真正新增的那条被漏计(signature 带一个 site 的容差,同
signature 多条是现实场景)。

辅助规则:`status != Checked` 的 request 只保留 diagnostics,不参与成功候选。

失败时为 diagnostics 挑选 best overlay 用的比较序(lexicographic,不用加权
magic number):

```text
deltaClean(true 绝对优先) > checkerError=false > checkerIllegal=false
> 相关 violation 数少 > changed span 总面积小 > 枚举序靠前
```

### 6.9 final check、输出与失败路径

- `hasSolution=true`:`changes` 是搜索中首个通过 §6.8 baseline-delta clean 门的
  overlay(不再有单独的 final full-overlay check,V2.1 修订 #11)。
- `hasSolution=false`:`changes` 为空(不返回 partial repair,避免把违例挪走但未清
  干净;explicit partial mode 未来可加,不默认开启),diagnostics 说明失败原因:
  无合法 master、fixed-cell 冲突、枚举/call 预算耗尽、窗口到顶(含扩窗截止,§6.3)、
  所有 overlay 均未 delta-clean;附 best overlay、remaining violations、窗口与预算
  统计;若命中 swap-unfixable 提示(§6.2,已降级为 Warning)一并附上。**definitive
  语义(V2.1 修订 #10)**:仅当**最后一个实际搜索过的窗口**完成了完备枚举(未被
  size 上限或预算截断)才可标注"window space exhausted definitively";任一较早窗口
  曾完备不足以据此断言。
- placement precheck 失败既可由 opto 提前阻断,也会由 repair 返回 warning
  `PrecheckFailed`;两条路径都不产生 changes。

---

## 7. 预算与参数(初值)

| 参数 | 值 | 说明 |
|---|---|---|
| 每窗口 checker call 上限 | 512 | 含 baseline;与 V1 持平 |
| batch 大小 | 16-32 | 批内枚举序定序 |
| 完备枚举阈值 | 3^k ≤ 剩余预算(k ≤ 5) | 满足则完整枚举,无解结论对**当前窗口**确定(§6.7/§6.9 #10) |
| size-2/3/4 成员上限 N_s(按 filler 计) | 24 / 12 / 8 | 仅大窗口截断时启用,作用在 filler 数上(V2.1 #9);超出则渐进扩窗 |
| 最大子集 size | 4 | 更大组合交给渐进扩窗 |
| 渐进扩窗步长 K | 2 | adaptive-L1 每步在相关行/侧扩入的连续 filler 数;耦合 blocking rows ±1(§6.3) |
| 窗口模型 | L0 + adaptive-L1 | L2 已删(V2.1 #7);扩窗截止即失败路径 |

所有参数进 config,diagnostics 打印实际取值。

---

## 8. 扩展性设计

### 8.1 merge/split

- **①层增量**:新增 RewriteGenerator(merge/split),只提议少数高价值
  tiling(两个 bridge filler 合一、在 cell 边界切开宽 filler、短 run 重铺),控制
  分支爆炸的位置在生成器,不在搜索。split 的价值在于比 swap 更细的
  VT 粒度(例如 4 → 2+2 允许 span 的半段换 VT、半段保持,是 swap-only 覆盖不了的
  解形态);tiling 每段宽度必须取自库中实际存在的宽度集合(附录 A,当前为
  {2,3,4,8})。tiling 生成器可参考 DAC'23 的 DP row-optimal insertion(状态 =
  位置 × VT-interval 长度 × filler-interval 长度 × label,配 inter-row cost
  table 可线性化):在 span 上求近似违例最少的 tiling 作为 proposal,checker 仍作
  终判;MF(min filler width)约束由库宽度集合自然满足。
- **②层增量**:排序加操作类型偏好(swap 优先于 rewrite)与 span 面积项(§6.6)。
- **③④⑤层零改动**:冲突判定(span 相交)、canonical key、cache、delta 分类
  (span 几何锚)、gate、diagnostics 全部按 §4/§6.8 的定义直接适用。
- **接口(前置条件)**:checker API 从 swap-only `FillerCellRecord` 版本化升级为
  primitive-op 形态(remove/add + overlayId echo,职责归属与协议见 §5.2 决策
  记录);infrastructure 候选查询切换到 per-span tiling 键(§5.3 演进方向)。
- **不变量**:重铺精确覆盖 span、宽度和相等,gap/overlap-free 继续按构造保持。

### 8.2 multi-height

- `FillerRewrite.rowIds` 多行;窗口按"跨行 instance 拉入其全部行"规则扩展(§6.3);
- placement precheck 对 multi-row instance 按行分别记覆盖;
- 候选查询增加 row parity / orientation 约束(P/N band 翻转行),由 infrastructure
  候选 API 吸收;
- checker 已有 band-slot / inter-row 模型,oracle 边界不变;
- 搜索核不变。

### 8.3 beam escape hatch

当窗口内可行操作数大到排序枚举预算不够(预计出现在 merge/split 时代的大窗口),
在③层后插入 beam searcher 作为替代子集生成器。它复用②的排序与④的 gate,不引入
新的 accept 语义。第一版不实现,只保留此接口位。

### 8.4 batch opto / 多 cluster

一次多个 changed cell 时,再引入 V1 的 cluster 划分(violation 构图连通分量)与
"独立求解 → final merge → conflict merge retry"机制;单 anchor 场景不需要。

---

## 9. diagnostics

precheck diagnostics 至少记录 Gap/Overlap、row 与 x range。repair diagnostics 记录:anchor
(`targetPlace` 五元组);窗口级别与实际 `repairWindow`/`guardRegion`/halo 来源;
初始 violation 数与归一化 signature;候选 filler 数、生成 swap 数;
枚举子集数、canonical cache 命中数、checker call 数(batch 次数与 batch size);
baseline result 摘要;final result 是否 checked/legal/delta-clean 与 returned
violation 数;best overlay 及其分类(residual original / new inside-window /
related-in-halo / unrelated-in-halo 统计);bridge filler ids;失败原因枚举。

`setDebugLogging(true)` 额外输出 deterministic `[fr][stage]` transcript:
`planner` 记录 request/config/final decision,`normalize` 记录 signature 输入,
`window` 记录 L0/adaptive-L1 方向与增量,`swapgen`/`rank`/`enumerate` 记录候选空间,
`gate` 记录 baseline、batch、cache、budget 与 best candidate。默认关闭;开启只增加
可观测性,不改变排序、预算或 accept 结果。engine init 另记录每个 implant layer
的 raw WIDTH/SPACING 与 `defaultHaloX` 的胜出来源；snapshot 前记录 request、
engine snapshot、live Network、PhysDesMgr、master bottom-band polarity 以及同时
覆盖 physical/request Y 的全部 PhysRow iteration records。

初始化 Fatal diagnostics 不依赖 `setDebugLogging()`。row/site 类失败必须输出
reference/observed row 的 site name、pad flag、site width/height/count、origin 与
bbox，并同时给出 Grid/checker/engine 的 site-width frame。mixed site heights
以最小 non-pad height 为 base；非整数倍才是 `IncompatibleRowHeight` Fatal。
Node filler flag 与 configured allow-list 是 engine authority，UDM macro-type flag
不再产生 engine init Fatal。checker 源码和 checker 自身的 overlay validation
保持不变。

---

## 10. 测试集

当前 82 个 planner cases 是独立 GoogleTests,位于交付目录中的
`fillerRepair/test/`,并与 71 个 portable E2E cases 同级;E2E cases 集中在
`fillerRepair/test/FillerRepairCheckerE2ETest.cpp`:26 个 final-checker fixture/overlay
case + 33 个 `FillerRepairPlanner`→final checker repair/failure case + 12 个
internal exact-coverage precheck case。fixture 通过
`ImplantLayerCheckerHelper` 构造 8 行 × 200 sites 的 Grid/Network/checker input;
不读 DEF/LEF,不需要 `E2ETestProvider` 或 real-UDM design builder。
四类 width/spacing checker 与 repair path 都在 target 左右 20 columns、有效
`target row +/-1` 的局部窗口内运行 50:50、30:70、20:80、10:90、5:95 精确
filler:std-cell 比例。dense placement 风险、快速失败与 span-rewrite 演进提议见
`docs/filler_repair_dense_placement_analysis.md`;它是 future design note,不改变本阶段
swap-only normative contract。

101 个 checker/engine fake-UDM cases、provider 与完整 local fake regression
copy 均位于 `src/dpl2/test/local`,不进入迁移目录。

前置与协议:

- runtime clean/gap/overlap 三种 precheck;gap/overlap 返回 `isLegal=false` 与
  Warning diagnostics,clean 返回 `isLegal=true`。另覆盖 hard blockage 内空白、
  instance halo 内空白不报 Gap,以及同一 blocked-row 的合法 segment 内真实 Gap
  仍然阻断。
- internal coverage sweep 覆盖 clean、前/中/后 gap、相邻 overlap 合并、legal
  segment 间 whitespace 排除、placed span clipping、多 row 排序、空/零宽 span、
  empty placement、unordered input、triple coverage、touching legal spans 与
  gap+overlap 确定性顺序。
- external public runtime engine API 在 3 种 layout 上覆盖 repeated precheck、opto gate、
  repair internal precheck、two-row hard macro upper-row coverage、hard/soft blockage、
  invalid replacement/precheck 无 registry 副作用与 update refresh。portable checker 另覆盖 changed neighbor
  位于 guard 外仍能报告 target violation。
- 验证 precheck 与 repair 前后 UDM physical records 完全相同。
- guard-only filler 出现在 `fillerChanges` 中,判 invalid request。
- 非 filler instance 出现在 `FillerCellRecord` 中,判 invalid request。
- (future)rewrite 不变量(span 精确覆盖完整 instance、宽度和相等)单测,
  随第二步 rewrite 一起引入,本阶段无此代码。
- planner 内部抽象仍校验 requestId/status;runtime engine private boundary 以 checker 结果顺序合成
  requestId/status。checker result 数与 candidate 数不相等判 protocol error。
- batch 中单个 invalid overlay 不影响其他 result,以 diagnostics 表达。
- canonical 相同 overlay 只调一次 checker(cache 命中)。

gate 语义:

- `status != Checked` 拒绝并记录 diagnostics。
- `Checked && isLegal && violations 非空` 拒绝。
- `Checked && !isLegal && violations 为空` 拒绝并记录 diagnostics。
- overlay 修掉 original 但 repairWindow 内新增 violation,拒绝。
- overlay 修掉 original 但 guard halo 出现相关新增/迁移 violation,拒绝。
- guard halo 中 baseline 已有且无关的 violation,不导致 candidate 失败。
- **baseline 一致性门**:baseline 复现不出某条 original,或 baseline 窗口内/相关
  violation 多于 original,判 `BaselineMismatch` fatal,不进入搜索(V2.1 #2+#4)。
- **multiset 一对一匹配**:两条同 signature 的 candidate violation,只有一条能被
  同 signature 的 baseline finding 抵消,另一条计为新增导致拒绝(V2.1 #3)。
- **新违例 rule distance**:candidate 新增违例来自 requiredValue 大于原始最大值的
  规则时,按 per-violation max 判为 related-in-halo 而拒绝,不被误放行(V2.1 #5)。

搜索行为:

- intra-row MS:单 filler swap 修好(size-1 首批命中)。
- inter-row MS:单 filler swap 修好。
- MW:必须两个 filler 同时改才 clean;单改不改善——size-2 子集命中。
- 同一位置两条 violation(不同 participant / 不同 P-N band),不去重,一起解。
- 一个 filler 关联多条 violation;一个 anchor 引发多条 violation。
- 三 VT:邻居 majority 不是正确的 anchor VT(验证 target VT 顺序)。
- 缺 same-size replacement master(fake provider 构造;当前真实库 VT 齐全,
  此 case 为防御性,附录 A),返回 no usable master diagnostics;fixed cell 约束冲突。
- 必须扩窗(L0 不够,渐进扩窗后修好);枚举预算耗尽触发扩窗;扩窗到顶返回 no
  solution 且 diagnostics 带 best overlay 与 remaining violations。
- 无 editable filler:纯 std cell 行内的 violation,引擎零 checker call 返回
  无解(`NoEditableFiller` 路径;unfixable 提示已随瘦身移除,见 §6.2)。
- 扩窗截止:扩窗后无新增 editable filler/move 时停止扩窗,不烧剩余预算。
- anchor-follow 首发:典型单/双 filler case 在首个 batch 内 clean。
- 小窗口完备枚举:窗口内确无解时,枚举完 3^k 空间后**确定性**扩窗;definitive 只
  按最后搜索窗口的完备性断言(L0 完备 + L1 截断 ⇒ 非 definitive,V2.1 #10)。
- 第三 VT 降权但可达:正解需要第三色的犄角 case 仍能被找到。
- 确定性:同输入两次运行,产出完全相同的 changes/diagnostics/call 序列。
- portable real-checker matrix 另锁定 clean empty repair、batch size 1/多 batch
  一致性、empty candidate、one-call budget、fabricated/duplicate original baseline
  gate、same-size filler-only changes、known new-violation site avoidance 与必须两个
  atomic swaps 的 MW 修复。
- 三 swap adaptive-direction regression:portable case 先证明人工三 swap overlay
  checker-legal;当 best residual 把主扩窗方向指向被 fixed cell 阻塞的一侧时,
  planner 必须尝试反方向、纳入第三个 non-L0 filler、返回完整三 swap 并由 final
  checker 验证 clean。任何失败路径仍返回空 changes,不允许 partial repair。

---

## 11. 实现状态

全部功能已实现并验证(2026-07-21):

- pure planner 完整落地:`Swap`/cache key、violation 归一化 + signature、
  L0 + adaptive-L1 window + guardRegion、swap 生成器、Ranker(5 特征,
  per band-slot 计数:同行 2 票/跨行 1 票)、SubsetSearcher、OracleGate
  (batch、canonical cache、baseline-delta、best-overlay 记录)、
  last-window definitive 语义。
- checker-owned `FillerRepairEngine` 借用 supplied Grid/Network,私有拥有
  oracle checker/snapshot;configured filler init-time 注册、late request master
  validate-then-rebuild;repair internal precheck 与原子 snapshot update;
  portable final-checker GoogleTest E2E、pure precheck sweep 与 CMake/CTest 接入;
  编译清单唯一定义在
  `src/dpl2/src/fillerRepair/sources.cmake`。
- 82 个 planner unit tests、71 个 portable checker/planner/precheck cases 与 101 个
  fake-UDM checker/engine tests 全为 GoogleTest;
  82 个 planner tests 与 database-free doubles 已移入 `fillerRepair/test/` 根目录,
  和 helper-built portable E2E 一起迁移;fake UDM checker/engine suite 留在 local;
  precheck/repair 均 non-mutating;
  runtime integration 使用 checker `check()` 预留点与 fillerRepair;
  Network Node 同步由 infrastructure 独立负责,checker DRC 算法未修改。
  2026-07-24 normal 与 ASan CTest 均为 254/254；完整 source list 在
  `-Wall -Wextra -Werror` 下编译通过。
  详见 `src/dpl2/HandOff.md` 与 `src/dpl2/src/fillerRepair/test/TestPlan.md`。

---

## 12. 与 V1 的差异清单

| # | 项目 | V1 | V2 定稿 | 理由 |
|---|---|---|---|---|
| 1 | 原子操作 | swap change | 内外统一 `FillerCellRecord`;内部 `Swap` 只附加 row/span/ranking 元数据;无通用 Move 抽象层 | 操作演进钉死为两步:swap → rewrite;merge/split 的抽象与 API 升级同批设计,避免投机 |
| 2 | 搜索内核 | greedy prefix + beam(N/K/D、survivor 配额、partial 打分) | 排序枚举 move 子集 + batch 验证,首 clean 即停 | 一条代码路径;无 partial 打分噪声与 magic knob;MW 非单调 case 是普通 size-2 子集;批并行友好 |
| 3 | 窗口 | W0-W5 六级,按类型选入口 | L0 + adaptive-L1(V2.1;V2 曾为 L0/L1/L2) | 规则尺度 ~1 site,多级状态过多;渐进扩窗避免一次吞整段 run 后退化为截断枚举(V2.1 #7/#8) |
| 4 | cluster | 划分 + 独立求解 + final merge + conflict-graph retry | 单 cluster(opto 单 cell),复用搜索中的首个 delta-clean 结果,无冗余 final check | 单 anchor 下所有 violation 同邻域;merge retry 机制移到 batch opto 时代 |
| 5 | 排序 | 12 特征 + 6 级 tie-break | 5 特征起步,其余 backlog;per band-slot 计数 | 排序只影响 call 数;先测命中率再加特征 |
| 6 | signature | "至少 ruleId+kind+relation+rowIds,尽量加…" | 匹配键与"相关性"定义钉死(§6.2) | delta 分类是最脆一环,不能留自由度 |
| 7 | 既有 violation | baseline-delta 允许 halo 无关违例 | 同 V1,并明确"忽略邻域遗留违例场景"为假设;顺手修历史违例列为未来 score 增量 | 按最新需求收敛 |
| 8 | beam | 主路径兜底 | escape hatch 接口位,第一版不实现 | 枚举预算不够时才需要 |
| 9 | 候选 API | per-instance | per-instance(wrapper)+ per-span tiling 演进方向 | merge/split 需要按几何键查询 |
| 10 | 扩展性 | 未系统化 | §8 专章:merge/split、multi-height、batch opto 的分层扩展路径 | 新增维度 |

保留不变的 V1 决策:checker-as-oracle(不复刻 DRC)、只在 checker 结果上 accept、
baseline-delta gate 与 guard halo 语义、two-cell guardRegion、canonical cache、batch
协议(planner 内部 requestId/status 由 runtime engine private boundary 合成)、pure planner、
失败不返回 partial、diagnostics 要求。终版 checker 现按 candidate 顺序关联结果,
runtime engine private boundary 合成 planner 内部 requestId/status;placement gate 是独立
public precheck,不进入 repair pipeline。

---

## 附录 A. 可用 filler master 库(示例工艺备注)

filler master 命名 `F_FILL{宽度}_63S6T9{VT}_1`,同一 site/track 族、同一高度;
宽度 ∈ {8, 4, 3, 2},VT 后缀 ∈ {R, L, UL}。**每个宽度三种 VT 齐全**,共 12 个:

```text
F_FILL8_63S6T9R_1   F_FILL8_63S6T9L_1   F_FILL8_63S6T9UL_1
F_FILL4_63S6T9R_1   F_FILL4_63S6T9L_1   F_FILL4_63S6T9UL_1
F_FILL3_63S6T9R_1   F_FILL3_63S6T9L_1   F_FILL3_63S6T9UL_1
F_FILL2_63S6T9R_1   F_FILL2_63S6T9L_1   F_FILL2_63S6T9UL_1
```

VT 后缀含义(按业界常规命名推断,待 library 团队确认):R = RVT(regular,
标准阈值)、L = LVT(low-VT,低阈值,更快/更漏电)、UL = ULVT(ultra-low-VT,
超低阈值)。三者即本 feature 的三种 implant type;与 checker header 中
`Layer::Vt` 的映射由 runtime engine private snapshot 从 master 的 implant 层导出;
shape 与 checker layer 通过 `Layer::TechLayerId` 关联,不依赖 master 或 layer 名字符串解析,
本附录命名仅供人读。

对算法的推论(备注性质,算法不 hard-code 这张表,一切以
`PlannerDataSource::getUsableMasterCandidates` 运行时返回为准):

1. **枚举预算充裕**:每个 filler 恒有 2 个同尺寸替换候选(3 VT − 当前),
   分支因子小且均匀,窗口内 move 总数很小,§6.7 的排序枚举远够用——这张表是
   该选型的直接佐证。
2. **无 VT 覆盖缺口**:任意宽度都可换到任意 VT。因此 swap-only 下
   `hasSolution=false` 只会来自 DRC 不可满足(搜索无 clean 解),不会来自缺
   master;"no usable master"路径保留为防御性处理(库变化时行为可控),
   不是常态路径。
3. **tiling 宽度约束**(future work):库中宽度集合为 {2,3,4,8},没有宽
   1/5/6/7 的 master。任何 span-rewrite tiling 的每一段宽度必须取自该集合,
   不得留宽 1 残段;宽 5/6/7 的 span 无法 merge 成单个 master,只能多 master
   重铺——这也支持 §4 采用一般的 span-rewrite 形式而非严格"merge 成一个"。
   以库实际为准。
4. **split 的价值定位**:库 VT 齐全时,split 的意义不是补库的缺口,而是提供比
   整个 filler 换 VT 更细的粒度(例如 4 → 2+2 允许半段换 VT、半段保持),
   这是 swap-only 覆盖不了的解形态(§8.1)。
5. **齐全库解锁的三项搜索精化**(已并入正文):anchor-follow 方向恒可构造,
   由 Ranker 排序实现(§6.6);第三 VT 强降权,有效分支因子 ~1(§6.6);
   小窗口 3^k 完备枚举,"窗口内无解"成为确定性结论(§6.7)。

---

## 附录 B. 相关工作对照 — DAC'23 filler insertion(Zou et al.)

《Toward Optimal Filler Cell Insertion with Complex Implant Layer Constraints》
(DAC 2023,Fudan)解决的是 **filler insertion** 问题:layout 存在空 site,
选择各空位的 filler VT,全局最小化 intra/inter-row MW/MS 与 MF(min filler
width)违例;三档 VT,不移动 cell。三段式:inference-driven violation
identification(局部 pattern 规则集 + 级联推理,预先识别 placed cell 锁死的
unsolvable violation)→ DP-based row-optimal insertion(线性化)→
contour-driven refinement(逐级扩圈重指派,带"违例未减少即终止"准则)。

与本 feature 的关系:

- **问题互补**:该工作是 insertion 阶段的全局求解器,正是本 spec §6.1 precheck
  失败时应先运行的那类上游工具;本 feature 是 insertion 之后、opto 内环的增量
  repair。规则形式化(intra/inter MW/MS、三 VT)一致。
- **架构差异**:该工作自建完整规则引擎(它没有 checker-in-the-loop);本 feature
  保持 checker-as-oracle,不复刻 DRC——但它证明了近似规则模型作为 **proposal
  生成器**的有效性,模型不准只多花 checker call,不影响正确性。
- **已采纳**(V1):swap-unfixable 快速判定(§6.2,源自其 unsolvable violation
  分类);扩窗截止准则(§6.3,源自其 contour 终止准则)。
- **已列入 future**:DP row-optimal tiling 作为 split/tiling 生成器参考
  (§8.1,tiling 语义由 repair 侧翻译为 primitive op,§5.2);model-guided
  proposal 作为 Ranker backlog(§6.6)。
- **反向验证**:其 inference 规则生成的强制赋值,与本 spec 的 anchor-follow
  排序方向、第三 VT 降权一致;其"部分违例在 filler 阶段无解"的分类,
  佐证 `hasSolution=false` + 明确失败码的输出语义。
