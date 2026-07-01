# 功能规格 — Filler VT Overlay 修复(checker-guided,只换 type)

状态:方案定稿提案。分支:`claude/filler-vt-overlay-repair-plan-2023`。
基线:`2023-base`。最后更新 2026-07-01。当前对齐本地 `src/dpl2` header draft。

**一句话**:design 已经全局铺满 filler,无空 site;std cell 改 VT/type 后产生 implant
MW/MS 违例。checker 把 violation list 交给 filler engine;filler engine 先确认 design 是
100% utility,否则直接报错。通过 preflight 后,filler engine **只替换 filler 的 VT/implant
type**(同宽同高同位置换 master),枚举/搜索若干 overlay 方案,并对每个方案调用 DP checking
API 在局部 window/cluster 对应的 `guardRegion` 上重查。第一版 accept 标准收敛为
baseline-delta clean:每个 request 带 `guardRegion`,并在同一个 `guardRegion` 下用
baseline request 与 overlay request 做 delta classification。
residual/new/spillover 分类是第一版 accept gate 的一部分:original target violation 必须消失,
repairWindow 内不能新增 violation,guard halo 内不能出现由 changed filler 引入或移动出来的 violation。
找到 clean 的 `FillerChange` 集合后返回;修不了则返回 diagnostics,不动 DB。

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
6. 每个 overlay 方案都调用 DP checking API,让 checker 在该局部 window/cluster 对应的
   `guardRegion` 上重新检查,不 commit DB。
7. filler engine 用 baseline-delta clean 判定 accept/reject:同一 `guardRegion` 下先跑
   baseline(empty `fillerChanges`),再比较 candidate overlay result。guard halo 中 baseline
   已有且与本次 changed filler 无关的 violation 不应导致 candidate fail。
8. 找到 clean overlay 后返回 `FillerRepairResult.changes`。
9. 搜不到 baseline-delta clean 解则 `hasSolution=false`,返回 diagnostics 给上游。

核心原则:**checker 负责产生 violation snapshot 和真实 DRC 判定;filler engine 负责开窗、
候选生成、overlay 搜索、baseline-delta clean 收敛和 result 分类。**

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
- 不调用 checker overlay check。
- 不返回 partial changes。
- `FillerRepairResult.hasSolution=false`。
- `FillerRepairResult.changes` 为空。
- `diagnostics` 带 fatal/error 级别的 `NonFullUtility` 或等价 code,并报告至少一个 gap 的
  row/x range/site count。

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

checker 当前本地 `src/dpl2/src/drc/ImplantLayerChecker.h` 中的 `CheckRequest` 语义如下:

```cpp
struct CheckRequest
{
    InstanceId instanceId = 0;  // upstream opto changed std cell
    MasterId masterId = 0;
    RowId rowId = 0;
    DbCoord x = 0;
    PhysOrientation orientation = PhysOrientationE::R0;
};
```

这会改变本 spec 的几个结论:

- `targetPlace.instanceId` 是被 opto 改 VT/type 的 std cell,也是 violation 的根因 anchor。
- `masterId/rowId/x/orientation` 描述该 std cell 的 candidate placement 和 master,供 checker 重建局部 implant
  check context。
- repair 的 W0-W5 "窗口"是 filler engine 的**候选 filler 选择、搜索预算和 batch 分组**
  概念;不能复用 `targetPlace` 表达。每个 checker request 必须额外携带
  `guardRegion`,由本次 repair window 外扩 two-cell guard halo 得到。
- 每次调用 checker overlay API 时,`targetPlace` 保持同一个 std-cell anchor;
  `fillerChanges` 表示候选方案;checker 按自己的 target-local 规则重查。
- 如果 repair 扩大候选窗口,它既允许更多 filler 进入 `fillerChanges`,也扩大本次 overlay
  搜索和诊断上下文;同时必须重新计算并传入更大的 `guardRegion`。
- 因为 checker 不是全局 DRC,当 overlay 改到 target std-cell 局部作用域边界附近时,
  checker 必须在 `guardRegion` 内返回 edge/spillover violation,用于 repair 侧分类。

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

## 3. 推荐 API 与职责边界

本节不拘泥于当前 header 草案,而是给 checker RD 和 infrastructure RD 的最小稳定接口。
核心边界:

- checker RD:只做 non-mutating overlay DRC verify。输入一个或多个
  `OverlayCheckRequest`;每个 request 是一个 atomic overlay 方案,输出对应 request 的真实
  violation snapshot。
- infrastructure RD:只做 placement/filler/master 查询。包括 100% utility precheck、editable
  filler 收集、same-size legal filler master 查询。
- filler repair engine:只做 window/cluster、候选排序、overlay 搜索和 baseline-delta clean 收敛。

### 3.1 共享基础类型

`TargetPlace` 表示上游 opto/ECO 改动的 std cell。它也可以在实现里继续命名为
`CheckRequest`,但语义必须是 target std-cell anchor,不是 repair window。

```cpp
struct TargetPlace
{
    InstanceId instanceId = 0;  // changed std cell
    MasterId masterId = 0;     // new/candidate std-cell master
    RowId rowId = 0;
    DbCoord x = 0;
    PhysOrientation orientation = PhysOrientationE::R0;
};

struct FillerChange
{
    InstanceId instanceId = 0;  // filler instance only
    MasterId newMasterId = 0;  // same w/h/orient-compatible filler master
};
```

推荐把 MW/MS 和 intra/inter 变成稳定 enum,不要让 repair 侧依赖字符串或猜
`RuleSource` 映射:

```cpp
enum class ViolationKind
{
    MinWidth,
    MinSpacing
};

enum class ViolationRelation
{
    IntraInstance,
    IntraRow,
    InterRow
};

struct ViolationParticipant
{
    InstanceId instanceId = 0;
    MasterId masterId = 0;
    RowId rowId = 0;  // row containing this participant instance/shape
    XInterval xRange;
    bool isFiller = false;
    bool isTarget = false;
};

struct Violation
{
    int ruleId = 0;
    ViolationKind kind = ViolationKind::MinWidth;
    ViolationRelation relation = ViolationRelation::IntraRow;

    LayerId primaryLayer = 0;
    std::optional<LayerId> secondaryLayer;

    // Must be sorted unique. Inter-row reports all touched rows; intra-row reports one row.
    std::vector<RowId> rowIds;

    XInterval xWindow;
    DbCoord measuredValue = 0;
    DbCoord requiredValue = 0;

    std::vector<ViolationParticipant> participants;
};
```

`Violation` 不需要 stable id。repair 会为一次 request 内的 diagnostics 生成临时
signature,但最终 correctness 只看 baseline-delta clean。

### 3.2 Checker RD API

checker 需要同时保留 single-overlay 和 batch-overlay 两个 API。一个
`OverlayCheckRequest` 表示一个 atomic candidate:checker 必须把该 request 的所有
`fillerChanges` 作为同一次 overlay 一起应用后检查,然后返回一个 `CheckResult`。
batch API 输入是 request vector,不是在一个 request 里再嵌套 candidates。

```cpp
using OverlayRequestId = int;

struct OverlayCheckRequest
{
    OverlayRequestId requestId = -1;  // generated by repair engine; unique in this batch
    TargetPlace targetPlace;
    Rect guardRegion;  // repair window expanded by a two-cell guard halo
    std::vector<FillerChange> fillerChanges;  // one atomic overlay candidate
};

enum class CheckStatus
{
    Checked,
    InvalidOverlay,
    Unsupported,
    CheckerError
};

struct CheckResult
{
    OverlayRequestId requestId = -1;  // must echo OverlayCheckRequest.requestId
    CheckStatus status = CheckStatus::CheckerError;

    // Meaningful only when status == CheckStatus::Checked.
    bool isLegal = false;

    std::vector<Violation> violations;
    std::vector<Diagnostic> diagnostics;
};

class ImplantOverlayChecker
{
 public:
  CheckResult checkPlaceWithOverlay(
      const OverlayCheckRequest& request) const;

  std::vector<CheckResult> checkPlaceWithOverlays(
      const std::vector<OverlayCheckRequest>& requests) const;
};
```

raw checker snapshot clean helper。它只表示某次 checker snapshot 完全 clean;最终 repair accept
仍使用同一 `guardRegion` 下的 baseline-delta clean。

```cpp
bool isRawCheckerSnapshotClean(const CheckResult& result)
{
    return result.status == CheckStatus::Checked
        && result.isLegal
        && result.violations.empty();
}
```

checker API 约定:

- non-mutating,不 commit DB。
- 一个 `OverlayCheckRequest` 可以包含多个 `FillerChange`,支持 MW/bridge group move。
- 每个 `OverlayCheckRequest` 必须携带 `guardRegion`;checker 至少在该区域内 collect
  violations。`guardRegion` 只扩大 checking/diagnostics 范围,不表示 repair 可以修改
  guard-only 区域里的 filler。
- 每个 `CheckResult` 必须 echo 对应的 `OverlayCheckRequest.requestId`。
- batch 返回顺序建议与输入顺序一致,方便 debug 和 fast path,但 repair correctness 不依赖顺序。
- repair engine 用 `requestId` 找回原 request 的 `fillerChanges`;如果 result 缺失、重复
  `requestId`、未知 `requestId`、或 single API echo 错误,必须拒绝该 batch 并记录 checker
  protocol error。
- 单个非法 request 只影响对应 `CheckResult`,不影响同 batch 其他 request。
- `status != Checked` 时必须给 diagnostics;repair 会拒绝该 request。
- `status == Checked && isLegal == false` 时,尽量返回 violations 或 diagnostics。
- `status == Checked && isLegal == true && violations` 非空时,repair 仍按不 clean 拒绝。
- checker 不需要标注 original/fixed/residual/new/spillover,也不需要维护历史 violation。
  repair 会用同一个 `guardRegion` 下的 baseline request 和 overlay request 做 delta
  classification。

### 3.3 Infrastructure RD:100% Utility API

100% utility 是 filler repair 的硬前置条件,应该由 placement/infrastructure 提供,不放在
checker 里。

```cpp
struct SiteCoverageRequest
{
    // Empty means all legal std-cell rows.
    std::vector<RowId> rowIds;
};

enum class CoverageIssueKind
{
    Gap,
    Overlap,
    OffGrid,
    IllegalOccupant
};

struct CoverageIssue
{
    CoverageIssueKind kind = CoverageIssueKind::Gap;
    RowId rowId = 0;
    DbCoord xLo = 0;
    DbCoord xHi = 0;
    int siteCount = 0;
    std::vector<InstanceId> instances;
};

struct SiteCoverageResult
{
    bool isFullUtility = false;
    std::vector<CoverageIssue> issues;
    std::vector<Diagnostic> diagnostics;
};

class PlacementQuery
{
 public:
  SiteCoverageResult checkFullSiteCoverage(
      const SiteCoverageRequest& request) const;
};
```

语义:

- 每个 legal std-cell site 必须被 std cell 或 filler 精确覆盖一次。
- gap / overlap / off-grid / illegal occupant 都是 precondition failure。
- macro、blockage、core cutout 等非 legal std-cell site 由 infrastructure 排除。
- `isFullUtility == false` 时,repair 直接 fatal diagnostic,不生成 overlay,不调用 checker。

### 3.4 Infrastructure RD:Filler/Master Query API

repair 不应该自己扫描所有 master 猜哪些能换。infrastructure 直接给 editable filler 和
legal same-size replacements。

```cpp
struct FillerMasterInfo
{
    MasterId masterId = 0;
    DbCoord width = 0;
    DbCoord height = 0;
    DbCoord siteHeight = 0;
    Family family = Family::Unknown;  // VT / implant type abstraction
    bool isFiller = false;
};

struct EditableFiller
{
    InstanceId instanceId = 0;
    MasterId currentMasterId = 0;

    RowId rowId = 0;
    DbCoord x = 0;
    DbCoord width = 0;
    DbCoord height = 0;
    PhysOrientation orientation = PhysOrientationE::R0;

    Family currentFamily = Family::Unknown;

    // Excludes currentMasterId. Every entry must be same-size and legal.
    std::vector<MasterId> legalReplacementMasters;
};

struct FillerQuery
{
    std::vector<RowId> rowIds;
    DbCoord xLo = 0;
    DbCoord xHi = 0;
};

class FillerInfrastructureQuery
{
 public:
  std::vector<EditableFiller> collectEditableFillers(
      const FillerQuery& query) const;

  const FillerMasterInfo* getFillerMasterInfo(
      MasterId masterId) const;

  std::vector<MasterId> getLegalReplacementMasters(
      InstanceId fillerInstanceId) const;
};
```

语义:

- `collectEditableFillers` 只返回 filler instance,不返回 std cell/macro/non-editable instance。
- `legalReplacementMasters` 只返回 filler master,不返回当前 master 本身。
- 每个 replacement 必须 same width / same height / same site compatibility /
  orientation-compatible。
- repair 最终只输出 `FillerChange { instanceId, newMasterId }`,不移动、不 split、不 merge、不删 filler。

### 3.5 Filler Repair Engine 边界

第一版实现放在 `src/dpl2/src/fillerRepair/`,先做 pure planner。真实 DB/checker 通过 adapter
接入,方便后续移植到另一个 DB。

```cpp
struct FillerRepairRequest
{
    TargetPlace targetPlace;
    std::vector<Violation> violations;  // initial snapshot from checker
};

struct FillerRepairResult
{
    bool hasSolution = false;
    std::vector<FillerChange> changes;
    std::vector<Diagnostic> diagnostics;
};
```

planner 依赖:

- `ImplantOverlayChecker` 做 batch overlay check。
- `PlacementQuery` 做 100% utility preflight。
- `FillerInfrastructureQuery` 做 candidate filler 和 legal master 查询。

这样 skeleton 可以先用 fake checker / fake infrastructure 单测 baseline-delta clean、
candidate sorting、group seed、beam 和 diagnostics;真实 adapter 等各 RD 接口稳定后再接入。

---

## 4. 开窗策略(重点)

目标:窗口足够覆盖 implant interaction,但不要大到搜索爆炸。

注意:这里的"窗口"不是 `TargetPlace targetPlace`。它由 filler engine 生成,用于选
candidate filler、限制搜索、batch 分组和诊断 residual/new/spillover violation。当前已同意的
DP checking API 显式接收 `guardRegion`,但 `targetPlace` 仍然只是 changed std-cell anchor,
不是 repair window。

### 4.1 违例归一化

对每条 violation 先抽象成:

- `signature`:filler engine 生成的临时 key;至少使用
  `ruleId + kind + relation + sorted(rowIds)`,并尽量加入
  xWindow/instances/implant hint。
- `type`:从 `ViolationKind` 映射出的 MW 或 MS。
- `relation`:intra-row 或 inter-row。
- `rowIds`:checker 提供的行集合;为空时退回 `targetPlace.rowId` 并在 diagnostics 中标记。
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

每次确定一个 repair window 后,repair engine 必须计算:

```text
guardRegion = expandByCellRing(repairWindow, 2)
```

`expandByCellRing(window, 2)` 表示在窗口四周加入 two-cell guard halo:横向包含窗口左右
各两圈相邻 placed std-cell/filler instance,纵向包含上下各两条相邻 std-cell row 中与窗口
xRange 相交或贴近的 placed std-cell/filler instance。若实现阶段只能用几何距离近似,必须用
不小于 two-cell ring 的 conservative expansion,并在 diagnostics 中打印实际 `repairWindow`,
`guardRegion` 和 halo 来源。

所有从该 repair window 生成的 `OverlayCheckRequest` 都必须携带同一个 `guardRegion`。
guard-only 区域内的 filler 只能参与 checker collect 和 diagnostics,不能出现在当前 request 的
`fillerChanges` 中。

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
`targetPlace` 和覆盖所有 changed filler 的 `guardRegion` 做一次 single-overlay check 或单元素
batch check。final `guardRegion` 至少应覆盖 merged repair window 外扩 two-cell guard halo。
clean 判定仍然基于同一 `guardRegion` 下的 baseline delta 和 checker result。

如果各 cluster 单独 baseline-delta clean,但合并后的 full overlay 不 clean,不要立刻放弃。
建议用失败 result 中的 returned violations、changed filler 邻接关系和窗口重叠关系建立
conflict graph,合并相关 cluster,扩大窗口后重试。超过 call/window budget 后再返回 no
solution diagnostics。

---

## 5. 候选生成

对窗口内每个 editable filler:

1. 从 `legalReplacementMasters` 枚举 target master。
2. 去掉 current master。
3. 只保留 same width / same height / row orientation compatible 的 master。
4. 将 master 映射到 `Family` / VT。

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
| `position` | 最后按 x,row 保证 deterministic |

候选排序:

1. `directParticipant` 或 `bridgeScore` 高者优先。
2. `cellAnchorVote + diffEdgesRemoved + multiViolationTouch + islandScore` 高者优先。
3. `cellConflict` 低者优先。
4. 更窄 filler 优先。
5. `sameVtBefore` 更小优先。
6. x 更小优先,再 row 更小优先。

target VT 排序:

1. 匹配相关 fixed std-cell anchor 的 VT。
2. 匹配能同时减少最多 cluster violation 的 VT。
3. 匹配邻近 filler majority region 的 VT。
4. MW 使用 primary layer 对应 VT。
5. MS 使用能消掉最多异 VT 邻接的 VT。
6. 最后按稳定 type id 顺序。

---

## 7. checker-guided 搜索

### 7.0 算法 review 结论

今天 review 后保留 Plan D 主路径,但对算法做以下收紧:

1. **第一版接受 baseline-delta clean**。
   每个 overlay request 都带 `guardRegion`;repair engine 必须先对同一个 `guardRegion`
   发 baseline request(empty `fillerChanges`),再对 candidate overlay 做 check。accept gate
   比较 overlay result 相对 baseline 的变化,而不是要求 guard region 中所有历史 violation
   都为空。
2. **guard halo 里的 unrelated baseline violation 不应导致失败**。如果 guard halo 中已有
   与本次 `fillerChanges` 无关的 violation,它只能进入 diagnostics;只有 original target
   violation 未修掉、repairWindow 内新增 violation、或 guard halo 中出现由 changed filler 引入/
   移动出来的 violation 时,才拒绝该 overlay。
3. **搜索不能假设单调改善**。MW/bridge case 里单个 move 可能让 violation count 不变甚至更差,
   但 group move 可 clean。因此 beam 必须允许少量 non-improving partial overlay 存活。
4. **所有 overlay 必须 canonicalize/cache**。同一组 `(instanceId,newMasterId)` 不应重复调用
   checker;batch 前先去重,batch 后按 `requestId` 还原 result,并校验缺失/重复/未知 id。
5. **扩窗不能只依赖 residual classification**。如果 result 非 clean、best overlay 停滞、remaining
   violation 的 `xWindow`/instances 靠近当前 repairWindow 边界,都可以触发扩窗。
6. **最终 merge fail 要 retry**。cluster 单独 clean 但 full overlay 不 clean 时,用 changed
   filler、returned violations、窗口重叠建 conflict graph,合并 cluster 后重新搜索。

### 7.1 score

每个 overlay 都通过 batch checker API 得到真实 checker result。第一版 accept/reject 使用
同一个 `guardRegion` 下的 baseline result 和 overlay result 做 delta classification。
checker 只需要返回 violation snapshot;original/residual/new/spillover 由 repair 侧分类。

checker result 基础有效性:

```cpp
bool isCheckedResultUsable(const CheckResult& result)
{
    return result.status == CheckStatus::Checked
        && (result.isLegal || !result.violations.empty() || !result.diagnostics.empty());
}
```

clean 解定义:

- 当前 cluster overlay 在相同 `guardRegion` 下满足 baseline-delta clean。
- 合并所有 cluster 后的 final full overlay check 仍满足 baseline-delta clean。
- final check 必须使用同样或更大的 `guardRegion`;diagnostics 记录 `repairWindow`,
  `guardRegion`,halo 来源和 final classification reason。

baseline-delta clean 至少要求:

- original target violations 在 overlay result 中消失。
- `repairWindow` 内没有新增 violation。
- guard halo 内没有由 changed fillers 引入、touch changed filler、或从 repairWindow 边界移动出来的
  violation。
- overlay result 的 `status == CheckStatus::Checked`。
- overlay result 的 checker fatal/protocol diagnostics 为空。

非 clean overlay 的排序建议用 lexicographic tuple,不要依赖 magic number 权重:

```cpp
struct OverlayScore
{
    bool deltaClean = false;        // true always wins
    bool checkerError = false;      // result.status != CheckStatus::Checked
    bool checkerIllegal = false;    // Checked && result.isLegal == false
    int violationCount = 0;         // result.violations.size()
    int coarseResidualOriginal = 0; // optional best-effort estimate
    int guardOrBoundaryRisk = 0;    // optional if checker exposes region/boundary
    int changes = 0;
    int sortingPenalty = 0;         // Plan sorting V2 tie-break
};
```

比较顺序:

1. `deltaClean=true` 绝对优先。
2. 非 clean 中,`checkerError=false` 优先;checker 未完成的 request 只保留 diagnostics,不参与成功候选。
3. 非 clean 中,`checkerIllegal=false` 优先。
4. `violationCount` 少者优先。
5. 如果 signature 字段足够可靠,`coarseResidualOriginal` 少者优先。
6. 如果 checker 暴露 guard/boundary 信息,`guardOrBoundaryRisk` 少者优先。
7. `changes` 少者优先。
8. `sortingPenalty` 小者优先。

如果 checker 暂时缺少 `rowIds` 或 boundary/guard 字段,第 4/5 项可以关闭或仅打印 diagnostics。只要
result 非 clean,即使分类显示 original 好像被修掉,该 overlay 也不能作为最终解返回。

overlay canonical key:

```text
sorted_unique((instanceId, newMasterId))
```

同一个 `instanceId` 在一个 overlay 中只能出现一次;若重复出现且 `newMasterId` 不同,该 overlay
直接标为 invalid request。checker call 前必须用 canonical key 去重并查 cache。cache value
至少包含 `CheckResult`、score、diagnostics 摘要和 checker call index。

### 7.2 greedy prefix search

从空 overlay 开始:

1. 生成尚未应用的 sorted moves。
2. 逐个评估 `overlay + move`。
3. 选择 score strict improvement 最大的 move。
4. 加入 overlay。
5. checker result 满足 baseline-delta clean 则返回。

适合简单 MS、孤岛 MW、单 filler 修复。
如果没有 strict improvement,不要在 greedy 中硬加 move;转入 beam/group seed。

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
- 每层允许保留少量 non-improving partial overlay,避免错过必须同时改多个 filler 的非单调修复。
- 对同一个 filler,beam 不再枚举与当前 overlay 冲突的 second assignment。

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

- `hasSolution=true`: `changes` 经最终 full overlay 检查满足同一 `guardRegion` 下的
  baseline-delta clean。
- `hasSolution=false`:默认 `changes` 为空,diagnostics 说明为什么未找到 clean 解。
- 100% utility preflight 失败时,`hasSolution=false`,changes 为空,diagnostics 必须是
  fatal/error,原因是 placement precondition 不满足,不是搜索无解。
- partial repair 以后可加,但必须显式请求。

---

## 10. diagnostics

建议至少记录:

- 100% utility preflight 状态;若失败,记录 gap row/x range/site count;
- target std-cell anchor:`targetPlace.instanceId/masterId/rowId/x/orientation`;
- cluster/window id;
- rule type(MW/MS) 与 intra/inter;
- 初始 violation 数;
- final checker result 是否 checked / legal / clean;
- final checker result returned violation 数;
- candidate filler 数;
- generated move 数;
- checker call 数;
- batch checker call 数与 batch size;
- window expansion level;
- best overlay changes;
- remaining violation signature/kind/rowIds/xWindow/participants(若 checker 暴露);
- 可选的 fixed original / residual original / new inside-window / spillover best-effort 分类统计;
- std-cell anchor id;
- bridge filler ids;
- 失败原因:无合法 master、fixed-cell 冲突、搜索预算耗尽、窗口过大、所有 overlay 均未达到
  baseline-delta clean;补充 residual/new/spillover 解释。

---

## 11. 实现 TODO

1. 定义 filler-engine 的 pure repair planner,输入 `FillerRepairRequest`,输出
   `FillerRepairResult`。
2. 实现 100% utility preflight;失败时直接 fatal diagnostic,不生成 overlay,不调用 checker。
3. 在 helper/data-source adapter 中建立 master -> `Family` / same-size replacement table。
4. 实现 `ImplantOverlayChecker`、`PlacementQuery`、`FillerInfrastructureQuery` adapter,
   先用 fake adapter 测 planner。
5. 实现 violation 归一化、开窗和 cluster 合并。
6. 实现 cell-centric / bridge filler candidate 扩展。
7. 实现 Plan sorting V2。
8. 实现 overlay canonicalization/cache,避免重复 checker call。
9. 实现 `guardRegion` 生成:每个 repair window 外扩 two-cell guard halo,并写入每个
   `OverlayCheckRequest`。
10. 实现 baseline-delta clean gate:同一 `guardRegion` 下先做 baseline check,再比较 candidate
   overlay result。
11. 实现 greedy prefix search。
12. 实现 beam search fallback,包含 bridge/group seed 和少量 non-improving partial。
13. 实现最终 full overlay 检查;若合并后不 clean,尝试 conflict cluster merge retry。
14. 加 fake checker 单测:
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
   - `status != CheckStatus::Checked`,必须拒绝并记录 checker diagnostic;
   - `status == CheckStatus::Checked && isLegal == true` 但 `violations` 非空,必须拒绝;
   - `status == CheckStatus::Checked && isLegal == false` 即使 `violations` 为空,也必须拒绝并记录 checker diagnostic;
   - overlay 修掉 original 但在 `repairWindow` 内新增 violation,必须拒绝;
   - overlay 修掉 original 但在 guard halo 中新增/touch changed filler 的 violation,必须拒绝;
   - guard halo 中 baseline 已有且与 changed filler 无关的 violation,不能导致 candidate 失败;
   - guard-only filler 出现在 `FillerChange` 中,必须判 invalid request;
   - batch API 的每个 result 必须 echo 输入 request 的 `requestId`;repair 不依赖返回顺序做 correctness;
   - batch API 中一个非法 overlay 不影响其他 overlay result;
   - canonical 相同 overlay 只调用一次 checker;
   - 单 move 不改善但 two-filler group seed clean;
   - beam budget exhausted。
15. 等 `ImplantOverlayChecker::checkPlaceWithOverlays` 接口稳定后接真实 checker。
16. `src/dpl2` CMake 接入后,再把 skeleton 纳入 build/test。

---

## 12. 给 RD 的接口需求清单

### 12.1 给 Checker RD

需要 checker RD 提供 non-mutating single-overlay 和 batch-overlay check。推荐接口:

```cpp
struct TargetPlace
{
    InstanceId instanceId = 0;
    MasterId masterId = 0;
    RowId rowId = 0;
    DbCoord x = 0;
    PhysOrientation orientation = PhysOrientationE::R0;
};

struct FillerChange
{
    InstanceId instanceId = 0;
    MasterId newMasterId = 0;
};

using OverlayRequestId = int;

struct OverlayCheckRequest
{
    OverlayRequestId requestId = -1;  // repair-engine generated, unique in one batch
    TargetPlace targetPlace;
    Rect guardRegion;  // repair window expanded by a two-cell guard halo
    std::vector<FillerChange> fillerChanges;  // one atomic overlay candidate
};

enum class CheckStatus
{
    Checked,
    InvalidOverlay,
    Unsupported,
    CheckerError
};

struct CheckResult
{
    OverlayRequestId requestId = -1;  // echo OverlayCheckRequest.requestId
    CheckStatus status = CheckStatus::CheckerError;
    bool isLegal = false;
    std::vector<Violation> violations;
    std::vector<Diagnostic> diagnostics;
};

CheckResult checkPlaceWithOverlay(
    const OverlayCheckRequest& request) const;

std::vector<CheckResult> checkPlaceWithOverlays(
    const std::vector<OverlayCheckRequest>& requests) const;
```

`Violation` 推荐结构:

```cpp
enum class ViolationKind
{
    MinWidth,
    MinSpacing
};

enum class ViolationRelation
{
    IntraInstance,
    IntraRow,
    InterRow
};

struct ViolationParticipant
{
    InstanceId instanceId = 0;
    MasterId masterId = 0;
    RowId rowId = 0;
    XInterval xRange;
    bool isFiller = false;
    bool isTarget = false;
};

struct Violation
{
    int ruleId = 0;
    ViolationKind kind = ViolationKind::MinWidth;
    ViolationRelation relation = ViolationRelation::IntraRow;
    LayerId primaryLayer = 0;
    std::optional<LayerId> secondaryLayer;
    std::vector<RowId> rowIds;  // sorted unique touched rows
    XInterval xWindow;
    DbCoord measuredValue = 0;
    DbCoord requiredValue = 0;
    std::vector<ViolationParticipant> participants;
};
```

checker 侧必须确认的语义:

- 一个 request 内的 `fillerChanges` 是 atomic candidate,checker 必须一起 overlay 后检查。
- 每个 request 必须带 `guardRegion`;checker 至少在该区域内 collect violations。
- `guardRegion` 只扩大 checking/diagnostics 范围,不允许 repair 修改 guard-only filler。
- 每个 `CheckResult` 必须 echo 对应 request 的 `requestId`。
- batch 返回顺序建议与输入顺序一致,但 correctness 不依赖顺序。
- repair engine 会用 `requestId` 找回原 request 的 `fillerChanges`。result 缺失、重复
  `requestId`、未知 `requestId`、或 single API echo 错误都视为 checker protocol error。
- 单个 invalid/unsupported request 只影响自己的 `CheckResult`。
- `status != Checked` 时必须返回 diagnostics。
- `status == Checked && isLegal == false` 时尽量返回 violations 或 diagnostics。
- `status == Checked && isLegal == true && violations` 非空时,repair 仍会按不 clean 拒绝。
- checker 不需要维护历史 violation,也不需要标注 original/fixed/residual/new/spillover。
- `Violation.rowIds` 必须提供并 sorted unique;inter-row violation 要包含所有涉及行。
- `ViolationParticipant.rowId` 必须提供,用于区分同一 x 位置、不同 row/participant 的 violation。
- repair 会对同一个 `guardRegion` 做 baseline request 和 overlay request 的 delta
  classification,用于忽略 guard halo 中与本次 `fillerChanges` 无关的既有 violation。

### 12.2 给 Infrastructure RD

需要 infrastructure RD 提供 100% utility precheck:

```cpp
struct SiteCoverageRequest
{
    std::vector<RowId> rowIds;  // empty means all legal std-cell rows
};

enum class CoverageIssueKind
{
    Gap,
    Overlap,
    OffGrid,
    IllegalOccupant
};

struct CoverageIssue
{
    CoverageIssueKind kind = CoverageIssueKind::Gap;
    RowId rowId = 0;
    DbCoord xLo = 0;
    DbCoord xHi = 0;
    int siteCount = 0;
    std::vector<InstanceId> instances;
};

struct SiteCoverageResult
{
    bool isFullUtility = false;
    std::vector<CoverageIssue> issues;
    std::vector<Diagnostic> diagnostics;
};

SiteCoverageResult checkFullSiteCoverage(
    const SiteCoverageRequest& request) const;
```

需要 infrastructure RD 提供 filler/master 查询:

```cpp
struct FillerMasterInfo
{
    MasterId masterId = 0;
    DbCoord width = 0;
    DbCoord height = 0;
    DbCoord siteHeight = 0;
    Family family = Family::Unknown;
    bool isFiller = false;
};

struct EditableFiller
{
    InstanceId instanceId = 0;
    MasterId currentMasterId = 0;
    RowId rowId = 0;
    DbCoord x = 0;
    DbCoord width = 0;
    DbCoord height = 0;
    PhysOrientation orientation = PhysOrientationE::R0;
    Family currentFamily = Family::Unknown;
    std::vector<MasterId> legalReplacementMasters;
};

struct FillerQuery
{
    std::vector<RowId> rowIds;
    DbCoord xLo = 0;
    DbCoord xHi = 0;
};

std::vector<EditableFiller> collectEditableFillers(
    const FillerQuery& query) const;

const FillerMasterInfo* getFillerMasterInfo(
    MasterId masterId) const;

std::vector<MasterId> getLegalReplacementMasters(
    InstanceId fillerInstanceId) const;
```

infrastructure 侧必须确认的语义:

- `checkFullSiteCoverage` 检查所有 legal std-cell sites 是否被 std cell 或 filler 精确覆盖一次。
- gap/overlap/off-grid/illegal occupant 都要作为 precondition failure 返回。
- macro/blockage/core cutout 等非 legal std-cell site 由 infrastructure 排除。
- `collectEditableFillers` 只返回 filler instance。
- `legalReplacementMasters` 只返回 filler master,不包含当前 master。
- replacement 必须 same width / same height / same site compatibility / orientation-compatible。
