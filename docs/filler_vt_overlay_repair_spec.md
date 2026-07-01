# 功能规格 — Filler VT Overlay 修复(checker-guided,只换 type)

状态:方案定稿提案。分支:`claude/filler-vt-overlay-repair-plan-2023`。
基线:`2023-base`。最后更新 2026-07-01。

**一句话**:design 已经全局铺满 filler,无空 site;std cell 改 VT/type 后产生 implant
MW/MS 违例。checker 把 violation list 交给 filler engine;filler engine 先确认 design 是
100% utility,否则直接报错。通过 preflight 后,filler engine **只替换 filler 的 VT/implant
type**(同宽同高同位置换 master),枚举/搜索若干 overlay 方案,并对每个方案调用 DP checking
API 在局部 window/cluster 上重查。第一版 accept 标准收敛为 checker-clean:
`CheckResult.isLegal == true && CheckResult.violations.empty()`。residual/new/spillover
分类只做 best-effort diagnostics、排序提示和未来增强,不作为第一版正确性依赖。找到 clean 的
`FillerChange` 集合后返回;修不了则返回 diagnostics,不动 DB。

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

1. checker 初次检查 target std cell,把 violation list 交给 filler engine。
2. filler engine 做 100% utility preflight:每个合法 site 必须已被 std cell 或 filler
   覆盖。如果发现空 site/gap,立即返回 fatal diagnostic,不进入 repair search。
3. filler engine 以 `targetPlace` 指向的 std-cell anchor 为根,把 violations 合成局部
   repair window / violation cluster。
4. filler engine 在窗口内枚举合法的 same-size filler master 替换,形成若干
   `FillerChange` overlay 方案。
5. filler engine 用 Plan sorting 的权重和 tie-break 排序,但把 fixed cell 当作 target VT
   的投票/约束。
6. 每个 overlay 方案都调用 DP checking API,让 checker 在该局部 window/cluster 上重新检查,
   不 commit DB。
7. filler engine 先用 checker-clean 判定 accept/reject:`isLegal == true` 且本次
   `violations` 为空才接受。original/residual/new/spillover 分类在第一版只用于 debug、
   排序提示和扩窗解释。
8. 找到 clean overlay 后返回 `FillerRepairResult.changes`。
9. 搜不到 DRC-clean 解则 `hasSolution=false`,返回 diagnostics 给上游。

核心原则:**checker 负责产生 violation snapshot 和真实 DRC 判定;filler engine 负责开窗、
候选生成、overlay 搜索、checker-clean 收敛和可选 result 分类。**

---

## 2. 范围与不变量

### 输入背景

- design 已经 100% utility:每个合法 site 都被 cell 或 filler 占用。
- DRC 来源:opto/ECO 后 std cell 改 type/VT,改变了与周围 filler 的 implant 邻接。
- VT/implant type 有三种。
- DRC 规则两类:MW(min-width) 与 MS(min-spacing)。每类又分 intra-row / inter-row。
- `targetPlace` 是上游 opto/ECO 改动的 std cell placement,也是这批 violation 的
  root anchor;它不是 repair 侧任意选择的几何窗口。
- checker 给 filler engine 一次 violation snapshot 作为原始待修 violation list;repair
  侧不会要求 checker 长期维护 violation 状态。
- filler engine 后续每个 overlay 方案都会调用 DP checking API,要求 checker 在指定局部
  window/cluster 上重新检查并返回新的 violation snapshot。

### 硬前置条件:100% utility

filler engine 开始 repair 前必须检查 occupancy:

- 每个合法 site 必须被 std cell 或 filler 覆盖。
- 不允许存在空 site/gap。
- 不允许把"补洞"作为 repair 的隐含行为。
- 如果实现顺手能发现 overlap/非法占用,也应报 placement precondition error。

失败语义:

- 不生成 candidate filler。
- 不调用 `checkPlaceWithOverlay`。
- 不返回 partial changes。
- `FillerRepairResult.hasSolution=false`。
- `FillerRepairResult.changes` 为空。
- `diagnostics` 带 fatal/error 级别的 `NonFullUtility` 或等价 code,并报告至少一个 gap 的
  row/col range/site count。

原因:本方案只替换 filler master,保持同宽同高同位置。如果 design 没有全局铺满 filler,
正确动作应该是先跑 filler insertion / placement repair,不是让 VT repair 搜索承担补洞职责。

### 关键事实:violation 与 filler 是多对多

这个问题不能建模成“一条 violation 对应一个 filler”。真实情况是:

- 一个 std cell 改 type 后,可能同时引起多个 MW/MS violation。
- 一个 filler cell 可能同时参与多个 violation,改它会同时影响多条约束。
- 两条 violation 可以发生在同一几何位置,但由不同 filler/不同 rule/不同 participant 组合触发。
- 某条 violation 的 checker participants 不一定包含所有必须一起改的 filler;有些需要改的是
  **bridge filler**(连接 std cell 与周围 implant 区的短 filler)。

因此 `Violation` 是 constraint,不是唯一 root cause。repair 的基本单位应是
**局部 violation cluster + 候选 filler 集合**,目标是让整个 cluster 在 checker result 中 clean。

### 关键事实:`targetPlace` 是 std-cell anchor,不是 repair window

checker 更新后的 `CheckRequest` 语义如下:

```cpp
struct CheckRequest
{
    InstanceId instanceId = 0;  // upstream opto changed std cell
    GridY rowId{0};
    GridX colId{0};
    PhysOrientation orientation = PhysOrientationE::R0;  // exact enum spelling follows checker
};
```

这会改变本 spec 的几个结论:

- `targetPlace.instanceId` 是被 opto 改 VT/type 的 std cell,也是 violation 的根因 anchor。
- `rowId/colId/orientation` 描述该 std cell 的 placement,供 checker 重建局部 implant
  check context。
- repair 的 W0-W5 "窗口"是 filler engine 的**候选 filler 选择、搜索预算和 batch 分组**
  概念;当前已同意的 DP checking API 不显式接收这个窗口,不能复用 `targetPlace` 表达。
- 每次调用 `checkPlaceWithOverlay` 时,`targetPlace` 保持同一个 std-cell anchor;
  `fillerChanges` 表示候选方案;checker 按自己的 target-local 规则重查。
- 如果 repair 扩大候选窗口,它既允许更多 filler 进入 `fillerChanges`,也扩大本次 overlay
  搜索和诊断上下文;checker 的实际检查范围仍由 checker 内部规则决定。
- 因为 checker 不是全局 DRC,当 overlay 改到 target std-cell 局部作用域边界附近时,
  checker 最好返回 edge/spillover violation,或者提供一个可选 guard/check region。

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
struct CheckRequest
{
    InstanceId instanceId = 0;  // upstream opto changed std cell
    GridY rowId{0};
    GridX colId{0};
    PhysOrientation orientation = PhysOrientationE::R0;
};

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
    CheckRequest targetPlace;              // changed std-cell anchor
    std::vector<Violation> violations;     // original snapshot from checker
    std::vector<EditableFiller> candidateFillers;  // optional; filler engine can also collect by region
};

struct FillerRepairResult
{
    bool hasSolution = false;
    std::vector<FillerChange> changes;
    std::vector<Diagnostic> diagnostics;
};

struct CheckOverlay
{
    CheckRequest targetPlace;  // fixed std-cell anchor, not a repair window
    std::vector<FillerChange> fillerChanges;
};

CheckResult checkPlaceWithOverlay(const CheckOverlay& overlay) const;

std::vector<CheckResult> checkPlaceWithOverlays(
    const CheckRequest& targetPlace,
    const std::vector<std::vector<FillerChange>>& candidateOverlays) const;

struct CheckResult
{
    bool isLegal = true;
    std::vector<Violation> violations;
    std::vector<Diagnostic> diagnostics;
};
```

完整调用方向:

1. checker / DP 发现 implant MW/MS violations,构造 `FillerRepairRequest`。
2. filler engine 从 `request.violations` 建 cluster/window,用于选择 candidate filler 和生成
   overlay。
3. filler engine 枚举一组 `FillerChange` 作为一个 overlay。
4. filler engine 对单个 overlay 可调用 `checkPlaceWithOverlay`;对一批 overlay 优先调用
   已同意的 `checkPlaceWithOverlays(targetPlace, candidateOverlays)`。
5. checker 围绕同一个 `targetPlace` 做局部重查,对每个 overlay 返回一个 `CheckResult`。
6. `CheckResult` 只表达这次检查事实:`isLegal`、raw violations、diagnostics;不要求
   checker 直接标注 original/fixed/residual/new/spillover。
7. filler engine 用 checker-clean predicate 决定是否 accept;original/result matching 只用于
   diagnostics、排序提示、扩窗和未来增强。

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

`CheckResult` 当前已定为 raw snapshot:

```cpp
struct CheckResult
{
    bool isLegal = true;
    std::vector<Violation> violations;
    std::vector<Diagnostic> diagnostics;
};
```

`isLegal` 是 checker 对该 overlay 的直接合法性判断;`violations` 是本次 overlay 下重新检查
得到的 violation snapshot。checker 不需要把 violation 存成长期 member/state;repair 只消费
当前 overlay 下的临时结果。

第一版 filler engine 的 correctness gate 必须简单且保守:

```cpp
bool isCheckerClean(const CheckResult& result)
{
    return result.isLegal && result.violations.empty();
}
```

也就是说,如果 checker result 中还有任何 violation,无论 repair 侧能否判断它是 original、
residual、new 还是 spillover,该 overlay 都不能作为最终解接受。这个取舍避免把工程推进卡在
violation identity 不稳定或字段不足的问题上。

filler engine 仍会把 `FillerRepairRequest.violations` 作为 original snapshot,把每次
`CheckResult.violations` 作为 checked snapshot,做 best-effort 分类。但这些分类只用于
diagnostics、非 clean overlay 的排序提示、扩窗解释和未来增强:

| 分类 | 定义 | 第一版用途 |
|---|---|---|
| fixed original | original set 中有、checked set 中没有 | debug/统计,不是 accept 条件 |
| residual original | original set 中仍然存在 | debug/失败解释,不是唯一 reject 原因 |
| new inside-window | checked set 中新增,且落在 filler engine 当前 window 内 | 如果能可靠识别则用于诊断 |
| spillover | checked set 中新增或残留在 filler engine window 边界/guard 区 | 如果能可靠识别则提示扩窗 |

checker 侧更新: `Violation` 不保留 stable id,但会提供 `rowIDs` 和 `type`:

```cpp
struct Violation
{
    ViolationType type;          // MW / MS
    std::vector<RowId> rowIDs;   // rows touched by this violation
    // other geometry / participant fields are optional but useful for matching and diagnostics
};
```

因为没有 stable id,filler engine 必须生成**临时 violation signature**,只用于本次 repair
request 内的 original/checked set 匹配,不跨调用保存。signature 优先使用:

1. `type`;
2. sorted `rowIDs`;
3. violation x/bbox/window,如果 checker 暴露;
4. participant instance ids / roles,如果 checker 暴露;
5. implant type hint,如果 checker 暴露。

若 checker 只提供 `type + rowIDs`,匹配会比较粗,diagnostics 应说明 classification 是
coarse matching。此时不要因为无法区分 residual/new/spillover 阻塞实现;accept 仍只看
`isCheckerClean(result)`。无论如何,仍然**不能只按坐标去重**。

每个 violation 最好包含:

| 字段 | 用途 |
|---|---|
| type(MW/MS) | 必需;分流、score、signature |
| relationship(intra/inter) | 决定开窗行数 |
| rowIDs | 必需;定位 inter-row 耦合行,参与 signature |
| xWindow | 横向开窗,但不能作为唯一 identity |
| primary/secondary implant type | 决定 target VT hint |
| participants | 判断相关 cell/filler |
| participant.role | 标出 fixed cell / filler / bridge / anchor |
| participant.instanceId | 找 candidate filler 或 std-cell anchor |
| participant.isFiller | 只改 filler |
| participant.xRange/row | 做 cluster 和 bridge window |
| insideRegion/spillover | 可选;区分窗口内残留和窗口外/边界违例 |

注意:**不能按坐标去重 violation**。同一个 x/y 位置可能有多条不同 rule 或不同 participant 的
violation,必须都保留到 cluster score 里。

### 3.3 对 checker API 的建议

- `checkPlaceWithOverlay` 必须接受多个 `FillerChange`。
- 已同意 batch API,beam search 应优先使用:

```cpp
std::vector<CheckResult> checkPlaceWithOverlays(
    const CheckRequest& targetPlace,
    const std::vector<std::vector<FillerChange>>& candidateOverlays) const;
```

- 函数必须 non-mutating,不改 DB。
- 相同 overlay 返回 deterministic result。
- `targetPlace` 必须被解释为 changed std-cell anchor,不是可变 check window。
- batch API 返回顺序必须与 `candidateOverlays` 输入顺序一一对应;第一版不需要
  `CheckResult` 保存 request 指针。如果后续为了 debug 需要显式关联,建议加 `requestIndex`
  而不是裸指针。
- 单个非法 overlay 只影响对应 `CheckResult`,不影响同 batch 其他 overlay。
- 一个 overlay 内必须支持多个 `FillerChange`,因为 MW/bridge case 经常需要 group move。
- `CheckResult.isLegal=false` 时,checker 仍应尽量返回 violations 或 diagnostics 说明原因。
- 当前同意的 batch API 不显式传 `checkRegion`;filler engine 的 W0-W5 window/cluster
  只用于候选生成和搜索分组,checker 的实际局部重查范围由 `targetPlace` 和 checker 内部
  region 规则决定。
- `CheckResult.violations` 是本次 overlay 的 snapshot,checker 不需要 maintain 历史
  violation。
- 如果未来 checker 需要显式边界控制,建议加 optional guard/collect region,但不要复用
  `targetPlace` 表达窗口。第一版 checker 可以先忽略该字段,但接口层预留能避免后续破坏式
  改 API:

```cpp
struct CheckOverlay
{
    CheckRequest targetPlace;
    std::vector<FillerChange> fillerChanges;
    std::optional<Rect> guardRegion;  // optional, for spillover collection only
};
```

guard/check region 的推荐语义:

- `repairWindow`:filler engine 允许收集 candidate filler、生成 overlay、输出 changes 的区域。
- `checkWindow` / `guardRegion`:checker 额外 collect violations 的区域,用于发现边界副作用。
- guard-only 区域里的 filler 只能参与 checking 和 diagnostics,不能被当前 overlay 修改。
- 如果 checker 尚不能返回 inside/boundary 信息,第一版仍可运行,只是 diagnostics 需要标明
  spillover classification unavailable。

---

## 4. 开窗策略(重点)

目标:窗口足够覆盖 implant interaction,但不要大到搜索爆炸。

注意:这里的"窗口"不是 `CheckRequest targetPlace`。它由 filler engine 生成,用于选
candidate filler、限制搜索、batch 分组和诊断 residual/new/spillover violation。当前已同意的
DP checking API 不显式接收 `checkRegion`;checker 根据 `targetPlace` 和内部局部规则重查。
`targetPlace` 仍然只是 changed std-cell anchor。

### 4.1 违例归一化

对每条 violation 先抽象成:

- `signature`:filler engine 生成的临时 key;至少使用 `type + sorted(rowIDs)`,并尽量加入
  xWindow/participants/implant hint。
- `type`:MW 或 MS。
- `relationship`:intra-row 或 inter-row。
- `rowIDs`:checker 提供的行集合;为空时退回 `targetPlace.rowId` 并在 diagnostics 中标记。
- `xRange`:violation `xWindow` 与 participant bbox 的 union。
- `vtHint`:MW 用 primary layer;MS 用 primary/secondary layer。
- `cellAnchors`:至少包含 `targetPlace.instanceId`;再加入参与 violation 或与 violation
  xRange 邻接的 fixed std cell。
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
- 始终围绕 `targetPlace.instanceId` 加入 W4。
- 若 violation 还有其他 fixed std-cell participant,也加入对应 W4。
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

最终返回前,把所有 cluster 的 changes 合并成一个 full overlay,用同一个
`targetPlace` 做一次最终 `checkPlaceWithOverlay` 或单元素 batch check。clean 判定仍然是
`isLegal == true` 且 `violations.empty()`。若未来 checker 支持 `guardRegion`,最终 check
可使用覆盖所有 changed filler 及其相邻 rule distance 的 guard region。

如果各 cluster 单独 checker-clean,但合并后的 full overlay 不 clean,不要立刻放弃。建议用失败
result 中的 returned violations、changed filler 邻接关系和窗口重叠关系建立 conflict graph,
合并相关 cluster,扩大窗口后重试。超过 call/window budget 后再返回 no solution diagnostics。

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

每个 overlay 都通过 `checkPlaceWithOverlay` 或 batch API 得到真实 checker result。第一版
filler engine 不依赖 checker 或自身把 result violation 标成 original/residual/new/spillover。
accept/reject 先看 checker-clean predicate:

```cpp
bool isCheckerClean(const CheckResult& result)
{
    return result.isLegal && result.violations.empty();
}
```

clean 解定义:

- 当前 cluster overlay check 满足 `isCheckerClean(result)`。
- 合并所有 cluster 后的 final full overlay check 仍满足 `isCheckerClean(result)`。
- 若 checker 支持 `guardRegion`,final check 应使用同样或更大的 guard/check window;若不支持,
  diagnostics 记录本次 clean 是 target-local checker clean。

非 clean overlay 的排序建议用 lexicographic tuple,不要依赖 magic number 权重:

```cpp
struct OverlayScore
{
    bool checkerClean = false;      // true always wins
    bool checkerIllegal = false;    // result.isLegal == false
    int violationCount = 0;         // result.violations.size()
    int coarseResidualOriginal = 0; // optional best-effort estimate
    int guardOrBoundaryRisk = 0;    // optional if checker exposes region/boundary
    int changes = 0;
    int sortingPenalty = 0;         // Plan sorting V2 tie-break
};
```

比较顺序:

1. `checkerClean=true` 绝对优先。
2. 非 clean 中,`checkerIllegal=false` 优先。
3. `violationCount` 少者优先。
4. 如果 signature 字段足够可靠,`coarseResidualOriginal` 少者优先。
5. 如果 checker 暴露 guard/boundary 信息,`guardOrBoundaryRisk` 少者优先。
6. `changes` 少者优先。
7. `sortingPenalty` 小者优先。

如果 checker 暂时只返回 `type + rowIDs`,第 4/5 项可以关闭或仅打印 diagnostics。只要
result 非 clean,即使分类显示 original 好像被修掉,该 overlay 也不能作为最终解返回。

### 7.2 greedy prefix search

从空 overlay 开始:

1. 生成尚未应用的 sorted moves。
2. 逐个评估 `overlay + move`。
3. 选择 score strict improvement 最大的 move。
4. 加入 overlay。
5. checker result 满足 `isCheckerClean(result)` 则返回。

适合简单 MS、孤岛 MW、单 filler 修复。

### 7.3 beam search 兜底

MW 常见情况:单独改一个 filler 不改善,必须两个或多个一起改。greedy 卡住时进入 beam。
同一位置多 violation 的情况也必须允许 beam 同时选择多个 bridge filler,不能因为第一步不 clean
就停止。

- beam 的输入不应只有 atomic move,也要主动生成 group seed:
  - std-cell anchor 左右 bridge filler pair;
  - 上下相邻行中与 cell 边界对齐的 bridge filler pair;
  - 同一短 filler run 整段同改;
  - 与同一个 anchor 接触的一组 fillers;
  - 同一 violation cluster 共享的一组 candidate fillers。
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
- diagnostics 带 best overlay、checker 返回的 remaining violations、窗口范围、候选数、
  checker call 数、失败原因。若 best-effort 分类可用,再附 residual/new/spillover 统计。

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

- `hasSolution=true`: `changes` 经最终 full overlay 检查满足
  `result.isLegal && result.violations.empty()`。
- `hasSolution=false`:默认 `changes` 为空,diagnostics 说明为什么未找到 clean 解。
- 100% utility preflight 失败时,`hasSolution=false`,changes 为空,diagnostics 必须是
  fatal/error,原因是 placement precondition 不满足,不是搜索无解。
- partial repair 以后可加,但必须显式请求。

---

## 10. diagnostics

建议至少记录:

- 100% utility preflight 状态;若失败,记录 gap row/col range/site count;
- target std-cell anchor:`targetPlace.instanceId/rowId/colId/orientation`;
- cluster/window id;
- rule type(MW/MS) 与 intra/inter;
- 初始 violation 数;
- final checker result 是否 clean;
- final checker result returned violation 数;
- candidate filler 数;
- generated move 数;
- checker call 数;
- batch checker call 数与 batch size;
- window expansion level;
- best overlay changes;
- remaining violation signature/type/rowIDs/xWindow/participants(若 checker 暴露);
- 可选的 fixed original / residual original / new inside-window / spillover best-effort 分类统计;
- std-cell anchor id;
- bridge filler ids;
- 失败原因:无合法 master、fixed-cell 冲突、搜索预算耗尽、窗口过大、所有 overlay 均未达到
  checker-clean;若可分类,再补充 residual/new/spillover 解释。

---

## 11. 实现 TODO

1. 定义 filler-engine 的 pure repair planner,输入 `FillerRepairRequest`,输出
   `FillerRepairResult`。
2. 实现 100% utility preflight;失败时直接 fatal diagnostic,不生成 overlay,不调用 checker。
3. 建立 master -> `FillerTypeId` / same-size replacement table。
4. 实现 violation 归一化、开窗和 cluster 合并。
5. 实现 cell-centric / bridge filler candidate 扩展。
6. 实现 Plan sorting V2。
7. 实现 `isCheckerClean(result)` gate:`isLegal == true && violations.empty()`。
8. 实现 overlay result best-effort 分类,仅用于 diagnostics/ranking,不作为第一版 accept 条件。
9. 实现 greedy prefix search。
10. 实现 beam search fallback,包含 bridge/group seed。
11. 实现最终 full overlay 检查;若合并后不 clean,尝试 conflict cluster merge retry。
12. 加 fake checker 单测:
   - 非 100% utility:存在 gap 时直接 fatal,不调用 checker,changes 为空;
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
   - `isLegal=true` 但 `violations` 非空,必须拒绝;
   - `isLegal=false` 即使 `violations` 为空,也必须拒绝并记录 checker diagnostic;
   - overlay 看似修掉 original 但 checker 返回任意 remaining violation,必须拒绝;
   - overlay 在 window 边界产生 spillover;若 checker 暴露 guard/boundary,必须诊断并扩窗或拒绝;
   - batch API 返回顺序与 candidate overlay 输入顺序一一对应;
   - batch API 中一个非法 overlay 不影响其他 overlay result;
   - beam budget exhausted。
13. 等 `checkPlaceWithOverlay` 接口稳定后接真实 checker。

---

## 12. 需要 checker team 确认/修改

1. `Violation` 除 `type(MW/MS)` 与 `rowIDs` 外,能否继续暴露 participant 的
   `isFiller / instanceId / row / xRange / implant type`?这些字段能显著提高临时 signature
   匹配质量。
2. `Violation.rowIDs` 是否保证 deterministic ordering?若不保证,filler engine 会排序后使用。
3. `Violation.type` enum 是否只含 MW/MS,还是还会区分 intra/inter?若不区分,intra/inter
   需要从 `rowIDs` 或其他字段推导。
4. `CheckResult.isLegal=false` 时,是否保证 `violations` / `diagnostics` 至少说明主要原因?
5. `CheckResult` 目前不区分 original/fixed/residual/new/spillover;第一版可以接受。
   filler engine 会以 checker-clean 为最终 gate,分类只做 best-effort diagnostics/ranking。
6. 已同意的 batch API 是否保持如下签名?

```cpp
std::vector<CheckResult> checkPlaceWithOverlays(
    const CheckRequest& targetPlace,
    const std::vector<std::vector<FillerChange>>& candidateOverlays) const;
```

7. batch API 返回结果顺序是否与 `candidateOverlays` 输入顺序一一对应?若需要显式关联,
   建议未来加 `requestIndex`,不要在 `CheckResult` 里保存 request 裸指针。
8. batch API 对单个非法 overlay 的错误是否只落在对应 `CheckResult`,不影响同 batch 其他
   overlay?
9. 是否需要新增可选 `guardRegion` / `collectRegion`,专门用于收集 window 边界 spillover
   violation?
10. `CheckResult.violations` 是否能标记 violation 在 checker 内部局部区域还是 guard/boundary?
11. `checkPlaceWithOverlay` / batch overlay 是否支持一个 overlay 内多个 filler changes?
12. candidateFillers 是 checker/opto 给 target std-cell 周边 editable filler,还是 repair
   侧从 DB 根据 W0-W5 自己收集?建议至少覆盖 W3/W4,不能仅 participant filler。
13. replacement master 是 repair 侧传 `MasterId`,还是 checker/DB 提供
   `FillerTypeId -> same-size master` 查询?
14. same width/height/orient-compatible 由 checker 强校验,还是 repair 侧保证即可?
15. 单个 `targetPlace` 的 checker runtime 会随 batch size 和 overlay change 数如何增长?
    需要给 repair 一个默认 call/window budget。
