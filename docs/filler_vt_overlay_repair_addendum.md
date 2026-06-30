# Spec Supplement - Filler VT Overlay Repair Implementation Assessment

状态:评估补充,不替代主 spec。
关联主 spec:`docs/filler_vt_overlay_repair_spec.md`。
目标分支:`claude/filler-vt-overlay-repair-plan-2023`。
最后更新 2026-07-01。

## 1. 总体结论

Plan D(checker-guided overlay search)是正确的主方向。

本 feature 的根因是 opto/ECO 改变 std cell VT/type 后,周围 filler 仍保持旧 implant type,从而产生真实 checker 看到的 MW/MS 违例。repair engine 不应该复刻一套简化 DRC;它应该负责候选生成、局部开窗、overlay 搜索和结果分类,并把每个候选 overlay 交给 checker 做真实判定。

相比旧的 `claude/filler-vt-weight-2023` 权重方案,Plan D 更适合当前问题,因为贴着 fixed std cell 的 filler 往往正是应该优先尝试的对象。旧规则 `touches_cell -> weight 0` 在本问题里不成立。

主要风险不在搜索框架本身,而在接口信息不足导致分类误判。尤其是 violation matching、checker 覆盖范围、spillover 判断、batch API 返回语义,需要在实现前收紧。

## 2. 必须收紧的接口点

### 2.1 Violation identity 不能过粗

主 spec 允许 checker 只提供 `type + rowIDs`,但这对实现来说太弱。同一行、同一几何位置可能存在多条不同 rule 或不同 participant 组合的 violation。只靠 `type + rowIDs` 会把 residual/new/fixed 分类做粗,甚至把不同 violation 错配。

建议 checker 尽量提供以下字段:

| 字段 | 原因 |
|---|---|
| `type` | MW/MS 基础分类 |
| `relationship` | intra-row / inter-row,决定开窗行集合 |
| `rowIDs` | inter-row violation 的行耦合信息 |
| `xRange` 或 bbox | 横向开窗与 signature 匹配 |
| participant instance ids | 区分同位置不同 root cause |
| participant role | anchor cell / fixed cell / filler / bridge |
| participant implant/type hint | 生成 target VT 排序 |
| inside/boundary flag | 支持 spillover 分类 |

若 checker 暂时只能提供 `type + rowIDs`,filler engine 仍可运行,但 diagnostics 必须标明 classification 是 coarse matching;accept clean overlay 时也应更保守,最好依赖 `CheckResult.isLegal` 和 guard check。

### 2.2 guardRegion / collectRegion 应第一版预留

主 spec 把 spillover 作为重要分类,但当前 API 不显式传 check/guard region。没有 guard region 时,repair engine 很难判断 overlay 是真的 clean,还是只是把 violation 推到 checker 当前局部作用域之外。

建议第一版就保留 optional region:

```cpp
struct CheckOverlay
{
    CheckRequest targetPlace;
    std::vector<FillerChange> fillerChanges;
    std::optional<Rect> guardRegion;  // collect spillover/boundary violations
};
```

语义建议:

- `targetPlace` 仍然是 changed std-cell anchor,不是窗口。
- `guardRegion` 只用于扩大 violation collection / spillover detection。
- checker 可先忽略该字段,但接口层预留能避免后续破坏式改 API。

### 2.3 Batch API 语义需要硬约定

`checkPlaceWithOverlays(targetPlace, candidateOverlays)` 应保证:

- 返回顺序与输入 overlay 顺序一一对应。
- 单个非法 overlay 只影响对应 `CheckResult`,不影响同 batch 其他 overlay。
- 相同 input overlay 返回 deterministic result。
- 一个 overlay 内必须支持多个 `FillerChange`。
- `CheckResult.isLegal=false` 时,仍尽量返回 violations 或 diagnostics 说明原因。

没有这些保证,beam search 很难可靠复现和调试。

## 3. 搜索策略优化

### 3.1 Score 使用 lexicographic tuple

主 spec 中的加权 score:

```text
100000 * residualOriginalViolations
+ 50000 * spilloverViolations
+ ...
```

建议实现为字典序比较,避免 magic number 失效:

```cpp
struct OverlayScore
{
    int residualOriginal = 0;
    int spillover = 0;
    int newInsideWindow = 0;
    bool checkerIllegal = false;
    int changes = 0;
    int sortingPenalty = 0;
};
```

比较顺序:

1. `residualOriginal` 少者优先。
2. `spillover` 少者优先。
3. `newInsideWindow` 少者优先。
4. `checkerIllegal=false` 优先。
5. `changes` 少者优先。
6. `sortingPenalty` 小者优先。

clean overlay 必须满足:

- no residual original violation;
- no new inside-window violation;
- no unacceptable spillover;
- `CheckResult.isLegal == true`;
- final full overlay check 仍 clean。

### 3.2 增加 group move,不要只依赖单 move beam

MW 经常需要多个 filler 同时改 type。若只从单 filler move 开始,第一步可能没有 score improvement,beam 会浪费很多 checker call。

候选生成阶段应主动生成 group overlay seeds:

- std-cell anchor 左右 bridge filler pair;
- 上下相邻行中与 cell 边界对齐的 bridge filler pair;
- 同一短 filler run 整段同改;
- 与同一个 std-cell anchor 接触的一组 fillers;
- 同一 violation cluster 共享的一组 candidate fillers;
- 多条 violation 共同触达的 filler set。

beam search 仍保留,但输入不应只有 atomic moves,也应包含这些高价值 group seeds。

### 3.3 Cluster 独立 clean 后仍需 merge retry

每个 cluster 单独求得 clean overlay,不代表合并后的 full overlay clean。最终 full overlay check 如果出现 new/spillover/residual,不应只直接失败。

建议流程:

1. 独立求解每个 cluster。
2. 合并 all cluster changes 做 final check。
3. 若 final fail,根据失败 violation 与 changed fillers 建 conflict graph。
4. 合并相关 cluster。
5. 在 merged cluster 上扩大窗口重新搜索。
6. retry 超预算后才返回 no solution diagnostics。

这能处理两个局部 repair 互相影响的情况。

### 3.4 多 anchor / guard final check

主 spec 用单个 `targetPlace` 作为 checker anchor。若一个 repair cluster 涉及多个 changed std cell 或多个 fixed std-cell participants,单 anchor 可能无法覆盖全部影响。

短期实现可以保持单 anchor,但最终 check 应覆盖所有 changed fillers 的 guard region。中期建议支持:

```cpp
struct CheckOverlayBatchRequest
{
    std::vector<CheckRequest> targetPlaces;
    std::vector<FillerChange> fillerChanges;
    std::optional<Rect> guardRegion;
};
```

至少 diagnostics 中应记录 final check 使用了哪个 anchor 和 guard region。

## 4. 开窗与候选策略建议

### 4.1 W4 / bridge filler 是默认必选,不是兜底

本问题最常见的修复对象是 std cell 与大 implant region 之间的短 filler。它可能不在 checker participant list 中,但必须进入候选集合。

实现时建议每个 cluster 默认加入:

- anchor std cell 左右相邻 filler;
- anchor std cell 上下行与其 x-boundary 对齐的 filler;
- width 小于 rule window 的短 filler;
- 位于两块不同 VT 大区域之间的 bridge filler;
- 与 violation xRange 相交且距离 anchor cell 不超过一个 rule distance 的 filler。

### 4.2 Candidate replacement 由 legalReplacementMasters 驱动

不要让 repair engine 猜 master 兼容性。`EditableFiller` 应包含:

```cpp
struct EditableFiller
{
    InstanceId instanceId = 0;
    MasterId currentMasterId = 0;
    FillerTypeId currentTypeId = 0;
    RowId rowId = 0;
    DbCoord x = 0;
    DbCoord width = 0;
    DbCoord height = 0;
    PhysOrientation orientation;
    std::vector<MasterId> legalReplacementMasters;
};
```

repair engine 只在 `legalReplacementMasters` 中枚举,保证 same width/height/orient-compatible。

### 4.3 Plan sorting V2 保留,但只作为搜索排序

Plan sorting V2 很适合减少 checker calls,但不能作为最终合法性判断。排序结果只决定评估顺序;accept/reject 必须由 checker result 分类和 final check 决定。

固定 cell 的处理原则:

- fixed cell 是投票和约束,不是禁止改动理由。
- 与 anchor std cell VT 匹配的 filler targetVT 应高优先级。
- 与其他 fixed cell 冲突的 targetVT 应降权,但不直接禁止;最终由 checker 判定。

## 5. 100% utility preflight 实现建议

语义上必须确认 full utility;实现上不必每次全 design 扫描。

建议维护 occupancy/full-utility cache:

- 初次构建全局 occupancy bitmap。
- ECO / filler insertion / placement change 标记 dirty rows 或 dirty ranges。
- repair 前检查 target window + dirty cache 状态。
- debug/fatal diagnostic 中仍报告至少一个 gap 的 row/col range/site count。

preflight fail 时必须:

- 不生成 candidate filler;
- 不调用 checker overlay;
- `hasSolution=false`;
- `changes` 为空;
- diagnostics 使用 fatal/error code,例如 `NonFullUtility`。

## 6. 推荐实现顺序

1. 定义 pure planner API: `FillerRepairRequest -> FillerRepairResult`。
2. 定义 fake checker 和 fake DB,先锁定 overlay/checker 语义。
3. 实现 100% utility preflight。
4. 建立 master/type replacement table。
5. 实现 violation normalization 和 temporary signature。
6. 实现 W1/W3/W4/W5 window builder。
7. 实现 cluster merge graph。
8. 实现 candidate filler collection,特别是 bridge filler。
9. 实现 Plan sorting V2。
10. 实现 group move generation。
11. 实现 batch checker wrapper 和 overlay result cache。
12. 实现 lexicographic score。
13. 实现 greedy prefix search。
14. 实现 beam search fallback。
15. 实现 expand-window retry。
16. 实现 final full overlay check。
17. 实现 final fail 后的 conflict cluster merge retry。
18. 接真实 checker API。

## 7. 最小验收测试集

除了主 spec 中列出的测试,建议第一批必须覆盖这些路径:

- 非 100% utility:有 gap 时不调用 checker,直接 fatal。
- 单 filler 修好 intra-row MS。
- 单 filler 修好 inter-row MS。
- MW 必须两个 bridge filler 同时改才 clean。
- 单 move 无 improvement,group seed 或 beam 找到 clean。
- 同一 x/y 位置两条 violation,不同 participant,不能去重。
- 一个 filler 同时影响多条 violation。
- overlay 修掉 original 但引入 new inside-window violation,必须拒绝。
- overlay 把 violation 推到 guard 边界,必须 classify spillover。
- 两个 cluster 单独 clean,合并后 fail,触发 merge retry。
- 缺 same-size replacement master。
- fixed-cell targetVT 冲突,checker reject。
- batch API 某个 overlay illegal 不影响其他 overlay result。
- final full overlay check fail 时 diagnostics 带 best overlay 和 residual signatures。

## 8. 最终建议

可以按 Plan D 开工,但第一阶段不要直接写复杂搜索。先把 checker contract、fake checker、violation signature、guard/check coverage 和 batch API 语义打牢。否则 solver 会很快写得很聪明,但难以证明 `clean overlay` 真的 clean。

实现上推荐把 repair engine 做成 deterministic pure planner:输入 request 和 checker adapter,输出 result;所有 DB commit 仍留给上游。这样最容易测试、复现和 debug。
