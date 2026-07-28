# Dense placement 下的 filler repair 风险与演进提议

状态: **设计讨论稿 / 历史记录**。本文的分析前提仍然成立，但其中列出的部分提议
此后已经落地，另有一部分接口在实现中被移除。以本文做设计参考时请对照下表:

| 本文提议 | 现状 |
|---|---|
| §4.3 #2 per-repair budget + truncated diagnostic | 已落地: `checkerCallBudgetPerRepair`(默认 2048) |
| §5 Window/Ranker 的有界扫描 | 已落地 |
| §4.3 #1 提到的全局 `precheckFillerRepair()` | **已移除**。engine 只保留区域 gate;全设计 placement 合法性归 infrastructure |
| `updateFillerRepair()` 快照刷新 | **已移除**。engine 改为懒初始化,快照随下一次懒建重建 |
| §5 的 254/254 CTest 与时间数据 | 已过期。当前为 220/220 本地 + 146/146 migration gate;最新性能数据见 `src/dpl2/src/fillerRepair/README.md` 的 "Search cost" |
| §4.1 influence closure / §4.2 Tier 2 span rewrite | **未实现**,仍是未来方向 |

原始更新: 2026-07-23。

## 1. 新前提

真实 layout 中 std cell 很密，filler 通常只占少数。原 E2E fixture 虽然把全设计
的 filler:std-cell 比例降到 20:80 和 10:90，但四个关键 repair window 的 filler
身份被锁定，局部候选空间仍接近 50:50。因此这些 case 证明了 checker 在不同全局
背景下结果稳定，却没有证明 planner 能处理局部 sparse-filler window。

新的 portable E2E matrix 改为控制 target-local density window:

- 每个 DRC 场景取目标左右共 20 个 site，并覆盖 `target row +/-1` 的有效行；
- intra-row width 位于边界行，窗口为 40 个 site；其余窗口为 60 个 site；
- 每个窗口精确覆盖 50:50、30:70、20:80、10:90、5:95；
- std/filler 身份变化只在同 implant layer 的 cell/filler master 间切换，不改变
  几何、row coverage、implant shape 或 DRC；
- 每个场景保留最小必要的 editable/bridge filler，并把 DRC 核心 footprint 内其余
  target/neighbor/context participant 锁定为 std cell，避免改变待测 candidate domain；
- intra/inter-row 的 min-width/min-spacing 都必须经真实 checker 完成 baseline、
  planner repair 和最终 overlay 验证。intra-row spacing 特别保留 anchor-adjacent
  bridge filler 和 gap filler，验证 planner 现有 L0 seed 语义；

这个定义测的是 target 邻域的可编辑资源密度。它仍不是任意真实 design 的统计
替代品，后续应补真实 DEF/LEF benchmark，但已经消除了原 fixture 最明显的偏差。

## 2. 已观察到的行为

旧的 whole-design density matrix 中，10:90 并不比 50:50 慢，反而略快。四类
planner-to-checker case 的代表时间如下：

| Case | 50:50 | 20:80 | 10:90 |
|---|---:|---:|---:|
| intra-row width | 0.916 s | 0.805 s | 0.611 s |
| inter-row width | 0.959 s | 0.971 s | 0.830 s |
| intra-row spacing | 11.791 s | 11.741 s | 11.551 s |
| inter-row spacing | 1.109 s | 1.048 s | 1.007 s |

这不能解释为 sparse filler 性能良好。关键窗口被锁定后，三档测试几乎拥有相同
的局部 candidate domain；时间主项是 intra-row spacing checker 路径，而不是
全局 filler 数量。新的 local-window matrix 才能观察局部比例变化。

新的 target-local matrix 在四类 DRC x 五档比例上全部通过。对应 planner repair
的单项 CTest 时间如下（2026-07-22，window/ranker 扫描收窄之后，见 §5）：

| Case | 50:50 | 30:70 | 20:80 | 10:90 | 5:95 |
|---|---:|---:|---:|---:|---:|
| intra-row width | 0.50 s | 0.49 s | 0.49 s | 0.50 s | 0.49 s |
| inter-row width | 0.87 s | 0.90 s | 0.59 s | 0.58 s | 0.51 s |
| intra-row spacing | 0.65 s | 0.67 s | 0.65 s | 0.67 s | 0.66 s |
| inter-row spacing | 1.09 s | 0.99 s | 0.99 s | 0.55 s | 0.55 s |

71 个 portable checker/precheck E2E 全部通过；包含 planner(82)、local
checker/engine regression(101)与 portable checker density(71)的完整 normal 与
ASan CTest 为 **254/254**，并在 `-Wall -Wextra -Werror` 下编译通过。

**关于计时的诚实说明**：这些 fixture 的行很短（每个窗口约 40–60 个 site），
单项时间由 checker DRC 路径与 per-test 进程启动主导，**不是** window 构造。因此
window/ranker 扫描收窄（§5）在这些数上看不出明显加速——它是一个 O(行长) →
O(log n + 窗口内 filler) 的**渐进复杂度**改进，收益出现在每行上千 instance 的真实
100% occupancy 设计上，而不是 60-site fixture 上；本轮用 254/254 保持全绿来证明
“选出的 filler 完全一致”（行为不变）。这里没有明显的低 filler 比例性能退化，但
fixture 有意保留了每类 DRC 的最小 editable/bridge support，因此不能外推到局部
完全没有 support 的真实 case。

fixture 验证过程还得到一个直接反例：intra-row spacing 中若把 anchor-adjacent
bridge filler 改成 std cell，同时保留后面的 gap filler，planner 返回
`NoEditableFiller`，checker requests/batches 都是 0。gap filler 不在 violation
participants 中，adaptive window 又不能跨过 std cell 到达它。这不是 checker
错误，而是当前 L0 seed 与 contiguous-filler expansion 的功能边界。

## 3. 当前算法在 dense placement 下的风险

### 3.1 Swap 支持可能根本不存在

当前操作只能把既有 filler instance 在原位置替换为同宽同高的另一 VT master。
当 violation 的有效影响区内没有 filler，或现有 filler span 无法提供所需 VT
边界时，swap-only 没有修复自由度。全局仍存在 filler 不能改变这个结论。

### 3.2 当前 adaptive window 不能跨 std cell 找 filler

`expandWindowAdaptive()` 只沿与当前 frontier 连续相接的 filler 扩展，遇到第一个
std cell、gap 或不连续 span 就停止。这个行为适合搜索 contiguous filler run，
但 dense std-cell row 中较远的 filler 通常被 std cell 隔开。盲目跨越 std cell
扫描所有远处 filler 也不正确：只有处于规则影响闭包内的 filler 才与本次 DRC
有因果关系。

### 3.3 低全局比例不是不可修复证明

以下条件都不能单独触发 hard failure：

- 全设计 filler 比例低；
- anchor 左右没有 filler；
- adaptive expansion 碰到 std cell；
- 所有 single-swap 都失败；
- checker call budget 用完。

Implant DRC 是非单调的，已有 two-swap 和 three-swap regression 证明单改无改善时
组合仍可能 clean。预算或成员上限耗尽只能返回 `SearchTruncated`，不能声称
`Unrepairable`。

### 3.4 预算是 per-window，不是 per-repair

当前默认每个窗口最多 512 个 checker request，并允许 32 次 adaptive level。
极端无解 case 的理论上限接近 `32 * 512`，不符合“不可能时快速返回”的目标。

### 3.5 独立 Move 不适合 100% occupancy

在完全占满的 row 中移动一个 filler，会在 source 留下 gap，而 destination 通常已
被 std cell 占用。如果不允许移动 std cell，合法操作不是单独 Move，而是对一个
完全由 filler 占有的连续 span 做等宽重铺。需要移动 std cell 才能获得空间时，
本模块应返回上游 placement repair，而不是在 filler repair 内伪造可行性。

## 4. 推荐 solution

### 4.1 先建立 sound influence closure

新增 `InfluenceAnalyzer`，输入 original violations、target/neighbor interval、涉及
rows 和所有相关规则的最大 reach，输出 conservative local influence closure：

1. std cell、macro 和 blockage 只作为 fixed context；
2. closure 内已有 filler span 是 editable support；
3. closure 外 filler 不进入 candidate domain；
4. 无法证明 closure 完备时，不允许输出 definitive no-solution。

这一步不做 DRC 判定。checker 仍是唯一 legality oracle；analyzer 只证明哪些对象
可能影响当前 violation。

### 4.2 分层 action model

| Tier | 操作 | 适用条件 | 当前模块复用 |
|---|---|---|---|
| 1 | Swap | 同位置同尺寸换 VT | 全部现有实现 |
| 2 | Span rewrite | 删除并重铺现有 filler-owned span，允许 merge/split/retile | 扩展 generator、rank 和 cache key |
| 3 | Placement repair | 需要移动 std cell、改变 row coverage 或没有 filler-owned span | 返回上游，不在本模块搜索 |

Tier 2 应按 span 建模，不引入通用 Move。一个 rewrite 必须满足 removed span union
与 added tile union 完全相等，保持 row coverage，无 gap/overlap。

### 4.3 快速失败路径

按以下顺序执行，只有带完备证明的路径才能声明不可修复：

1. precheck 失败：返回 `PrecheckFailed`，不进入搜索。已落地：`repair()` 在
   replacement master 注册前检查初始 target influence；adaptive candidate 若编辑
   更远行，则该 request 在进入 checker batch 前扩展并补查对应行。multi-row object
   按垂直重叠计入每一行。未触达区域的 gap/overlap 由 infrastructure 自己的全设计
   gate 负责（本文写作时设想的 public `precheckFillerRepair()` 已被移除）；
   placement/master commit 后由 infrastructure 同步 Grid/Network，engine 快照随
   下一次懒初始化重建；
2. influence closure 内无 editable filler：swap tier 返回
   `UnrepairableBySwap`；若不存在可重铺 filler span，则返回
   `NeedsPlacementRepair`；
3. candidate/domain 过滤后为空：对当前 action tier 返回 definitive no-solution；
4. 小窗口完整枚举：每个 filler 有 unchanged 加两个 replacement，共 `3^k - 1`
   个非空 overlay；`k=1..5` 分别为 2、8、26、80、242。包含 baseline 后仍可落在
   512 budget 内。全部 checker-rejected 后，才能证明该 closure/action tier 无解；
5. candidate space 超预算或任何 cap 生效：返回 `SearchTruncated`，changes 为空；
6. 增加 per-repair 全局 checker budget，避免每次扩窗重置 512。

建议明确区分结果：`Repaired`、`UnrepairableBySwap`、
`UnrepairableByRewrite`、`NeedsPlacementRepair`、`SearchTruncated`、
`PrecheckFailed`。现有 public result 暂时仍可通过 diagnostic code 表达，待 API
版本化时再提升为 enum。

### 4.4 Checker 与 infrastructure 的未来 contract

Span rewrite 需要版本化 checker wire：

- `remove(instanceId)`；
- `add(masterId, rowId, x, orientation, overlayId)`；
- checker 在 violation participant 中 echo request-scoped `overlayId`；
- checker 防御性验证 remove/add 的覆盖 union 完全一致；
- infrastructure 提供按 `(row, width, orientation)` 的 filler tiling candidate，并
  负责 commit remove/add；
- planner 负责选择 tiling，checker 只应用 overlay 并执行真实 DRC。

## 5. 现有代码复用评估

| 模块 | 处理方式 |
|---|---|
| `FillerRepairEngine` / `PlannerDataSource` | 保留 snapshot、ID、precheck、candidate 入口和 non-mutating contract；`instance()`/`masterInfo()` 已是 O(1) dense-table，`instancesInRow()` 返回缓存 bucket |
| `Signature` | 保留 violation normalization、signature 和 relatedness 基础 |
| `Window` | 改为 influence-bound window；不能只沿 contiguous filler run 扩展 |
| `Swap` | 保留为 Tier 1 generator |
| `Ranker` | 保留确定性排序，增加 action type、changed span 和 rule reach 特征 |
| `SubsetSearch` | 保留分批枚举，增加 span conflict 和 per-repair budget |
| `OracleGate` | 完整复用 baseline-delta、batch、cache、protocol gate 和 no-partial 语义 |
| cache key | swap 继续用 `(instanceId,newMasterId)`；rewrite 升级为 canonical primitive ops |
| diagnostics/log | 保留，并增加 closure、action tier、proof/truncation 原因 |

### 5.1 已落地的 sparse-filler 扫描收窄（2026-07-22）

前提：site 铺满、filler 通常只占 10–30%，所以一行有上千个 instance 而只有少数
是 target 邻域的可编辑 filler。窗口/排序阶段原本用**全行线性扫描**去找这些少数
filler，在稀疏场景下是纯浪费。本轮把热点扫描全部收窄为 x 区间二分（利用 planner
路径上 `instancesInRow` 按 x 排序且不重叠的不变量），成本从 O(行长) 降到
O(log n + 窗口内 filler)：

- 两个共享原语 `firstRightEdgeAfter` / `firstStartAtOrAfter` 提到
  `PlannerDataSource.h`（inline），供 `Window` 与 `Ranker` 复用；
- `Window`：`instancesInRing`、`buildWindow` 的 bridge 检测、`finalizeWindow` 的
  editable 收集（改为直接查成员再按 `(row,x,id)` 排序）、`expandWindowAdaptive`
  的左右 frontier 扩展，均不再扫全行；
- `Ranker`：`neighborMajorityVt`（每个待排序 filler 都会调用一次）只看紧邻/交叠
  邻居，收窄为对 inst.span 的二分，取代对 anchor 行与 ±1 行的整行扫描。

这是**行为不变**的复杂度改进：选出的 filler 与投票结果完全一致，因此不改搜索
语义、不动 engine 调用方式与任何接口数据结构，靠 254/254 normal/ASan CTest
验证等价。**故意保留** contiguous adaptive expansion；它能沿连续 filler run
渐进扩展，但不能跨越 std cell 或空隙触达非连续 filler。该功能边界仍需 §4.1 的
sound influence closure，或未来 replace/move/rewrite tier，才能根本解决。

## 6. 建议实施顺序

1. 先合入 local-window 50:50 到 5:95 E2E matrix，记录每档 checker request、batch、
   adaptive level 与时间；
2. 增加 per-repair budget 和明确的 truncated diagnostic，不改变候选语义；
3. 实现 influence closure 与 swap-only definitive fast return，并用反例测试保证不会
   把 multi-swap 解误判为无解；
4. 与 checker/infrastructure RD 定稿 remove/add wire 和 per-span tiling API；
5. 实现 Tier 2 span rewrite；需要 std-cell movement 的 case 统一返回 Tier 3。

进入 production 前至少需要真实 dense design 回归，覆盖局部 5% filler、零 filler、
filler 被 std cell 隔开、两/三 swap 非单调解、rewrite-only 解和必须 placement
repair 的无解场景。
