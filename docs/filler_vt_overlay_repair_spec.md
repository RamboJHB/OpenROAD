# 功能规格 — Filler VT Overlay 修复(checker-guided,只换 type)

状态:方案定稿提案。分支:`claude/filler-vt-overlay-repair-plan-2023`。
基线:`2023-base`。最后更新 2026-06-30。

**一句话**:design 已经全局铺满 filler,无空 site;std cell 改 VT/type 后产生 implant
MW/MS 违例。本模块**只替换 filler 的 VT/implant type**(同宽同高同位置换 master),
一边构造候选 overlay,一边调用上游 `checkPlaceWithOverlay` 判定,直到找到 checker-clean
的修改集合;修不了则返回 diagnostics,不动 DB。

---

## 1. 最终选择:Plan D(checker-guided overlay search)

之前的 Plan A/B/C 都有价值,但不完全匹配最新方向:

- **Plan A**:边界驱动,很快,但太局部;遇到 MW 需要多个 filler 同时改 type 时容易误判。
- **Plan B/C**:能在 site-grid 模型里做更优搜索,但会重复实现 DRC,且可能和真实 checker
  的 MW/MS/P/N/PRL 细节不一致。
- **Plan sorting**(`claude/filler-vt-weight-2023`):候选顺序很好,但旧规则
  `touches_cell -> weight 0` 不适用。本问题的 DRC 正是 std cell 改 VT 引起的,贴着
  fixed cell 的 filler 往往最该优先尝试。

**定稿建议**:用新的 **Plan D** 作为主路径:

1. 由输入 violation 建 repair window / violation cluster。
2. 在窗口内枚举合法的 same-size filler master 替换。
3. 用 Plan sorting 的权重和 tie-break 排序,但把 fixed cell 当作 target VT 的投票/约束。
4. 每一步通过 `checkPlaceWithOverlay` 评估 overlay,不 commit DB。
5. 找到 target place 内 MW/MS clean、且不引入 spillover 的 overlay 后,返回
   `FillerRepairResult.changes`。
6. 搜不到 checker-clean 解则 `hasSolution=false`,返回 diagnostics 给上游。

核心原则:**真实 DRC 判定交给 checker;我们负责开窗、候选生成、搜索顺序和结果收敛。**

---

## 2. 范围与不变量

### 输入背景

- design 已经 100% utility:每个合法 site 都被 cell 或 filler 占用。
- DRC 来源:opto/ECO 后 std cell 改 type/VT,改变了与周围 filler 的 implant 邻接。
- VT/implant type 有三种。
- DRC 规则两类:MW(min-width) 与 MS(min-spacing)。每类又分 intra-row / inter-row。
- 上游 checker 一次性给 target place、violation list、候选 editable filler。

### 关键事实:violation 与 filler 是多对多

这个问题不能建模成“一条 violation 对应一个 filler”。真实情况是:

- 一个 std cell 改 type 后,可能同时引起多个 MW/MS violation。
- 一个 filler cell 可能同时参与多个 violation,改它会同时影响多条约束。
- 两条 violation 可以发生在同一几何位置,但由不同 filler/不同 rule/不同 participant 组合触发。
- 某条 violation 的 checker participants 不一定包含所有必须一起改的 filler;有些需要改的是
  **bridge filler**(连接 std cell 与周围 implant 区的短 filler)。

因此 `Violation` 是 constraint,不是唯一 root cause。repair 的基本单位应是
**局部 violation cluster + 候选 filler 集合**,目标是让整个 cluster checker-clean。

### 只做

- 只替换 filler type:同一个 filler instance 换成同宽、同高、同 row/orient 可用的另一个
  filler master。
- 支持一次 overlay 包含多个 filler 替换。
- 中间候选只通过 checker overlay 验证。
- 最终输出 `FillerRepairResult`。

### 不做

- 不移动 std cell。
- 不移动 filler。
- 不改 filler width/height/x/row/orientation。
- 不删 filler,不留空 site。
- 不 split/merge filler instance。
- 不在 repair 内直接 commit DB。
- 不自己做最终 DRC 判定。

---

## 3. 接口草案与建议

当前 checker 侧草案:

```cpp
struct EditableFiller
{
    InstanceId instanceId = 0;
    MasterId currentMasterId = 0;
    RowId rowId = 0;
    DbCoord x = 0;
    PhysOrientation orientation = PhysOrientation::R0;
};

struct FillerChange
{
    InstanceId instanceId = 0;
    MasterId newMasterId = 0;
};

struct FillerRepairRequest
{
    CheckRequest targetPlace;
    std::vector<Violation> violations;
    std::vector<EditableFiller> candidateFillers;
};

struct FillerRepairResult
{
    bool hasSolution = false;
    std::vector<FillerChange> changes;
    std::vector<Diagnostic> diagnostics;
};

struct CheckOverlay
{
    CheckRequest targetPlace;
    std::vector<FillerChange> fillerChanges;
};

CheckResult checkPlaceWithOverlay(const CheckOverlay& overlay) const;
```

### 3.1 对 `EditableFiller` 的建议

建议把 filler 的几何、type、可替换 master 直接给 repair,避免 repair 侧猜 master 兼容性:

```cpp
struct EditableFiller
{
    InstanceId instanceId = 0;
    MasterId currentMasterId = 0;
    FillerTypeId currentTypeId = 0;  // VT / implant type 抽象
    RowId rowId = 0;
    DbCoord x = 0;
    DbCoord width = 0;
    DbCoord height = 0;
    PhysOrientation orientation = PhysOrientation::R0;
    std::vector<MasterId> legalReplacementMasters;  // 同 w/h 且 orient-compatible
};
```

原因:

- `currentMasterId` 不足以枚举三种 VT 的 same-size 替换。
- repair 侧必须知道 width/height 才能保证 occupancy-preserving。
- 如果 checker/DB 已经能判断 legal master,最好由它直接给 `legalReplacementMasters`。
- 最终输出仍可保持 `MasterId newMasterId`,不强制上游接受 `FillerTypeId`。

### 3.2 对 `Violation` / `CheckResult` 的建议

`CheckResult` 不应只给 pass/fail,需要完整 violation 列表。每个 violation 最好包含:

| 字段 | 用途 |
|---|---|
| stable id | 对比 original / fixed / residual |
| rule type(MW/MS) | 分流、score |
| relationship(intra/inter) | 决定开窗行数 |
| row ids | 定位 inter-row 耦合行 |
| xWindow | 横向开窗,但不能作为唯一 identity |
| primary/secondary implant type | 决定 target VT hint |
| participants | 判断相关 cell/filler |
| participant.role | 标出 fixed cell / filler / bridge / anchor |
| participant.instanceId | 找 candidate filler 或 std-cell anchor |
| participant.isFiller | 只改 filler |
| participant.xRange/row | 做 cluster 和 bridge window |
| insideTarget/spillover | 区分窗口内残留和窗口外新违例 |

注意:**不能按坐标去重 violation**。同一个 x/y 位置可能有多条不同 rule 或不同 participant 的
violation,必须都保留到 cluster score 里。

### 3.3 对 checker API 的建议

- `checkPlaceWithOverlay` 必须接受多个 `FillerChange`。
- 函数必须 non-mutating,不改 DB。
- 相同 overlay 返回 deterministic result。
- 如果可能,加 batch API,beam search 会大量受益:

```cpp
std::vector<CheckResult> checkPlaceWithOverlays(
    const std::vector<CheckOverlay>& overlays) const;
```

---

## 4. 开窗策略(重点)

目标:窗口足够覆盖 implant interaction,但不要大到搜索爆炸。

### 4.1 违例归一化

对每条 violation 先抽象成:

- `id`:checker stable id;没有 id 时用 `(rule,relationship,participants,xWindow)` 生成临时 id。
- `type`:MW 或 MS。
- `relationship`:intra-row 或 inter-row。
- `rows`:participant 涉及行;没有 participant 时用 violation anchor row。
- `xRange`:violation `xWindow` 与 participant bbox 的 union。
- `vtHint`:MW 用 primary layer;MS 用 primary/secondary layer。
- `cellAnchors`:参与 violation 或与 violation xRange 邻接的 fixed std cell。
- `fillerParticipants`:checker 明确列出的 filler。

### 4.2 窗口 ladder

按从小到大尝试:

| 窗口 | 内容 |
|---|---|
| W0 | anchor site 上的 filler + 上下左右 4-neighbor filler |
| W1 | 涉及行内与 `xRange` 相交的 filler,横向扩一个 rule distance |
| W2 | W1 snap 到完整 filler instance,再横向扩到最近 fixed cell/blockage/core 边界 |
| W3 | inter-row 用:包含耦合相邻行(±1) + 同一个 snapped x range |
| W4 | cell-centric:对每个 std-cell anchor,包含该 cell 左/右相邻 filler,以及上下相邻行中与 cell 边界/短 filler 对齐的 bridge filler |
| W5 | 合并所有 overlap/相邻且距离小于一个 rule distance 的 W2/W3/W4 |

默认入口:

- intra-row MW/MS 从 W1 开始。
- inter-row MW/MS 从 W3 开始。
- 若 violation 有 fixed std-cell participant,同时加入 W4。
- 如果 checker 返回 residual violation 刚好在窗口边界外,升一级窗口重试。

### 4.3 bridge filler 规则

std cell 改 VT 后,常见修法不是改大 filler,而是改夹在 std cell 和大区域之间的短 filler。
例如一个红色 std cell 旁边有两个蓝色 width-2 filler,上下又接红/蓝 width-6 区域;
同一位置可同时有 inter-MS 和 inter-row MW。此时把两个 width-2 蓝 filler 改成红,可能一次清掉
整个 cluster。

因此窗口必须主动包含:

- 与 std-cell anchor 左右接触的 filler。
- 与 std-cell anchor x-boundary 对齐的上下行 filler。
- 宽度小于等于 rule window 或明显短于相邻大 filler 的 bridge filler。
- 位于两个不同 VT 大区域之间的短 filler run。

这些 filler 即使没有出现在某条 violation participant list 中,也应进入候选集。

### 4.4 合并 violation cluster

不能把重叠违例独立修。构图:

- 节点 = violation。
- 两个 violation 的窗口重叠,连边。
- 行相同或相邻,且 xRange 距离小于一个 rule distance,连边。
- 共享 candidate filler,连边。
- 共享 fixed std-cell anchor,连边。
- 同一几何位置但 rule/participant 不同,也连边,**但不去重**。
- 一个 candidate filler 若出现在多条 violation 的窗口里,这些 violation 必须同 cluster 解。

每个 connected component 作为一个 cluster 一次求解。

处理顺序:

1. violation 多的 cluster 先处理。
2. inter-row 优先于 intra-row。
3. 含 std-cell anchor 的 cluster 优先。
4. 仍然相同则小窗口优先。

最终返回前,把所有 cluster 的 changes 合并成一个 full overlay,对整个 `targetPlace` 做一次最终
`checkPlaceWithOverlay`。

---

## 5. 候选生成

对窗口内每个 editable filler:

1. 从 `legalReplacementMasters` 枚举 target master。
2. 去掉 current master。
3. 只保留 same width / same height / row orientation compatible 的 master。
4. 将 master 映射到 `FillerTypeId` / VT。

候选 move:

```cpp
(instanceId, oldMasterId, newMasterId, oldType, newType)
```

绝不为 std cell、macro、non-editable filler 生成 move。

---

## 6. Plan sorting V2

沿用 `claude/filler-vt-weight-2023` 的排序思想,但修正 fixed-cell 处理:
**fixed cell 是约束和投票,不是禁止改动的理由。**

对一个候选 `(filler, targetVT)` 计算:

| 分量 | 含义 |
|---|---|
| `directParticipant` | filler 出现在 violation participants 中,高优先级 |
| `cellAnchorVote` | 与 std-cell anchor 接触,且 targetVT == cell VT |
| `bridgeScore` | 位于 std cell 和相邻大 implant region 之间的短 filler |
| `cellConflict` | 相邻 fixed cell 的 implant != targetVT 的边数 |
| `fillerVote` | 相邻 filler 当前/overlay VT == targetVT 的边数 |
| `diffEdgesRemoved` | 改完能消掉的异 VT 邻接边数 |
| `multiViolationTouch` | 该 filler 被多个 violation/window 关联 |
| `sameVtAfter` | 改完后的同 VT 邻接数 |
| `islandScore` | 当前 filler 没有同 VT 邻居、像孤岛时加权 |
| `width` | 平票时更窄优先 |
| `sameVtBefore` | 平票时 same-VT 邻居更少优先 |
| `position` | 最后按 col,row 保证 deterministic |

候选排序:

1. `directParticipant` 或 `bridgeScore` 高者优先。
2. `cellAnchorVote + diffEdgesRemoved + multiViolationTouch + islandScore` 高者优先。
3. `cellConflict` 低者优先。
4. 更窄 filler 优先。
5. `sameVtBefore` 更小优先。
6. col 更小优先,再 row 更小优先。

target VT 排序:

1. 匹配相关 fixed std-cell anchor 的 VT。
2. 匹配能同时减少最多 cluster violation 的 VT。
3. 匹配邻近 filler majority region 的 VT。
4. MW 使用 primary layer 对应 VT。
5. MS 使用能消掉最多异 VT 邻接的 VT。
6. 最后按稳定 type id 顺序。

---

## 7. checker-guided 搜索

### 7.1 score

每个 overlay 都通过 `checkPlaceWithOverlay` 得到真实 checker result。

建议 score:

```text
score = 100000 * targetViolations
      +  50000 * spilloverViolations
      +  20000 * newInsideTargetViolations
      +    100 * changes.size()
      +      1 * lowPriorityPenalty
```

排序含义:

1. clean 解绝对优先。
2. target violation 更少优先。
3. 不引入新 violation 优先。
4. change 数更少优先。
5. Plan sorting penalty 只做最后 tie-break。

clean 解定义:

- targetPlace 内目标 MW/MS 全清。
- 没有新增 inside-target MW/MS。
- 没有不可接受的 spillover MW/MS。

### 7.2 greedy prefix search

从空 overlay 开始:

1. 生成尚未应用的 sorted moves。
2. 逐个评估 `overlay + move`。
3. 选择 score strict improvement 最大的 move。
4. 加入 overlay。
5. checker clean 则返回。

适合简单 MS、孤岛 MW、单 filler 修复。

### 7.3 beam search 兜底

MW 常见情况:单独改一个 filler 不改善,必须两个或多个一起改。greedy 卡住时进入 beam。
同一位置多 violation 的情况也必须允许 beam 同时选择多个 bridge filler,不能因为第一步不 clean
就停止。

- 取 Plan sorting V2 排名前 N 的 move。
- 搜索深度 D。
- 每层保留 K 个最佳 partial overlay。
- 每个 partial overlay 都由 checker 真实评分。
- 一旦 clean,立刻返回。

建议初值:

| 参数 | 值 |
|---|---|
| N | 24 |
| K | 8 |
| D | 4 |
| 每窗口 checker call 上限 | 512 |

若 beam 找到改善但未 clean,采用 best improving overlay 继续 greedy。
若没有改善,扩窗。

### 7.4 失败策略

对每个 cluster:

1. 当前窗口 greedy。
2. 当前窗口 beam。
3. 扩窗一级。
4. 重复,直到 clean / candidate cap / call budget / window cap。

失败时:

- 默认 `hasSolution=false`。
- 默认 `changes` 为空,避免 partial repair 把违例挪走但未清干净。
- diagnostics 带 best overlay、残留 violation、窗口范围、候选数、checker call 数、失败原因。

可选:以后加 explicit partial mode,但不能默认开启。

---

## 8. 为什么这个方法适合当前 design

当前问题链路是:

1. std cell 改 VT/type。
2. 周围 filler 还保留旧 implant type。
3. cell/filler 或 filler/filler 边界产生 intra/inter MW/MS。
4. 把局部 filler 换成匹配新 implant context 的 same-size master,即可消掉真实 checker 看到的违例。

Plan D 正好针对这个链路:

- 从 violation/participant 定位局部窗口。
- 把同位置、同 std-cell anchor、共享 filler 的多条 violation 合成 cluster。
- 优先尝试贴 fixed cell、参与 violation、异 VT 边多、桥接 std cell 的短 filler。
- 每个候选都交给真实 checker 判断。
- MW 需要多 filler 联动时用 beam search,不是只看单步改善。
- 窗口不够时自动扩到相邻耦合行/固定边界。

典型例子:一个红色 std cell 改 type 后,其右侧两个 width-2 蓝 filler 同时连接上下不同
implant 区域;同一 x 位置可能既有 std cell 与右上红色 width-6 filler 的 inter-MS,
又有左上蓝色 width-6 filler 与 std cell 旁边蓝色 width-2 filler 的 inter-row MW。
正确修法可能是把两个 width-2 蓝 filler 都换成红色。这个例子要求 solver 同时满足:

- 不按 x 位置去重 violation。
- 不只改 checker 直接列出的某一个 filler。
- 把两个 width-2 bridge filler 放进同一个 cluster。
- 允许 beam 一次评估两个 change 的 overlay。

---

## 9. 输出语义

```cpp
struct FillerRepairResult
{
    bool hasSolution = false;
    std::vector<FillerChange> changes;
    std::vector<Diagnostic> diagnostics;
};
```

约定:

- `hasSolution=true`: `changes` 经最终 full-target overlay check 为 clean。
- `hasSolution=false`:默认 `changes` 为空,diagnostics 说明为什么未找到 clean 解。
- partial repair 以后可加,但必须显式请求。

---

## 10. diagnostics

建议至少记录:

- cluster/window id;
- rule type(MW/MS) 与 intra/inter;
- 初始 violation 数;
- 最终 residual violation 数;
- candidate filler 数;
- generated move 数;
- checker call 数;
- window expansion level;
- best overlay changes;
- residual violation id/xWindow/row/participants;
- std-cell anchor id;
- bridge filler ids;
- 失败原因:无合法 master、fixed-cell 冲突、搜索预算耗尽、窗口过大、checker 拒绝所有 overlay。

---

## 11. 实现 TODO

1. 定义 checker-facing 的 pure repair planner,输入 `FillerRepairRequest`,输出
   `FillerRepairResult`。
2. 建立 master -> `FillerTypeId` / same-size replacement table。
3. 实现 violation 归一化、开窗和 cluster 合并。
4. 实现 cell-centric / bridge filler candidate 扩展。
5. 实现 Plan sorting V2。
6. 实现 greedy prefix search。
7. 实现 beam search fallback。
8. 实现最终 full-target overlay check。
9. 加 fake checker 单测:
   - intra-row MS:一个 filler type change 修好;
   - inter-row MS:一个 filler type change 修好;
   - MW:必须两个 filler 同时改才修好;
   - 同一位置两条 violation,不同 participant,不能去重;
   - 一个 std cell anchor 引发多个 violation;
   - 一个 filler 同时关联多个 violation;
   - 三 VT:邻居 majority 不是正确 fixed-cell VT;
   - 缺 same-size target master;
   - fixed cell 约束冲突;
   - 必须扩窗才修好;
   - beam budget exhausted。
10. 等 `checkPlaceWithOverlay` 接口稳定后接真实 checker。

---

## 12. 需要 checker team 确认/修改

1. `Violation` 能否暴露 participant 的 `isFiller / instanceId / row / xRange / implant type`?
2. `Violation` 能否保留 stable id,避免同一坐标不同 violation 被误合并?
3. `CheckResult` 能否区分 original / fixed / residual / new / spillover violation?
4. `CheckRequest targetPlace` 能否表达多行 x-window,而不只是一个 placement point?
5. `checkPlaceWithOverlay` 是否支持一个 overlay 内多个 filler changes?
6. 是否可以提供 batch API `checkPlaceWithOverlays`?
7. candidateFillers 是 checker 给全窗口 editable filler,还是仅 participant filler?建议给全窗口。
8. replacement master 是 repair 侧传 `MasterId`,还是 checker/DB 提供
   `FillerTypeId -> same-size master` 查询?
9. same width/height/orient-compatible 由 checker 强校验,还是 repair 侧保证即可?
10. `targetPlace` 多大时 checker runtime 会不可接受?需要给 repair 一个默认 call/window budget。
