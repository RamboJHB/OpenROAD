# 功能规格 — Filler VT Overlay 修复 V2(checker-guided + span-rewrite planner)

状态:**V2 定稿**。分支:`claude/wizardly-carson-secahu`。基线:`2023-base`。
最后更新 2026-07-02。对齐本地 `src/dpl2` header draft(并行开发中)。

历史版本:
- `docs/filler_vt_overlay_repair_spec_v1.md`(V1 存档,greedy+beam 内核)
- `docs/filler_vt_overlay_repair_addendum_v1.md`(V1 实现评估补充,存档)

V2 相对 V1 的完整差异清单见 [§12](#12-与-v1-的差异清单)。

**一句话**:design 已 100% utility(无空 site);opto/ECO 一次改一个 std cell 的
VT/type 后产生 implant MW/MS 违例。checker 把 violation snapshot 交给 filler repair
engine;repair engine 通过 utility preflight 后,在局部窗口内生成 **span-rewrite move**
(第一版只实现"同位置同尺寸换 filler master"这一种),按启发式排序后**枚举 move 子集并
成批交给 checker overlay API 验证**,用同一 `guardRegion` 下的 baseline-delta clean 作为
唯一 accept 标准。找到 clean 解返回 `FillerRepairResult`,由上游 commit;修不了则返回
diagnostics,不动 DB。

---

## 1. 背景

### 1.1 问题链路与调用流程

1. opto/ECO **一次改一个 std cell** 的 VT/type(master 替换,位置不变)。
2. 该 cell 周围的 filler 仍保留旧 implant type,在 cell/filler 或 filler/filler 边界上
   产生 implant 层 DRC 违例。
3. checker 对 changed cell 做 target-local 检查,把完整 violation snapshot 交给
   filler repair engine。
4. repair engine 尽力修复 snapshot 中的所有 violation,把 solution
   (`FillerRepairResult.changes`)传回 checker。
5. checker 确认后交给 infrastructure commit。repair engine 全程不改 DB。

DRC 规则两类:**MW(min-width)与 MS(min-spacing)**,各分 intra-row / inter-row。
MS 按 P 区 / N 区(band polarity)分别检查;MW 的完整语义由 checker 负责,repair
engine 不依赖其细节(见 §3.1 职责边界)。示例工艺下 MW/MS 的 rule 尺度约为 1 个
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
重建局部 check context。repair 的"窗口"由 repair engine 自己生成(§6.3),每个
checker request 额外携带由窗口外扩得到的 `guardRegion`。

**opto 单 cell 内环调用**决定了:一次 repair 的所有 violation 都在同一个 anchor
邻域内,V2 第一版按单 cluster 求解(§6.4);checker call 的总开销 = 每次修复开销 ×
opto 改动次数,所以"好排序让首批候选命中"比"搜索策略高级"更能省时间。

---

## 2. 范围

### 2.1 第一版实现范围(只换 type)

- 只替换 filler master:同一 filler instance 换成同宽、同高、同 row/orientation
  兼容的另一个 filler master(即 span-rewrite 的退化形式,见 §4)。
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

- design 100% utility(硬前置条件,repair engine 自己 preflight,§6.1)。
- 初始 snapshot 中的 violation 全部视为本次待修对象;不考虑"邻域内存在改动前遗留
  violation"的场景(如未来需要"顺手修历史违例",在 score 中加次级最大化项即可,
  是纯增量,不影响本 spec 结构)。
- checker snapshot 是一次性交付,repair 不要求 checker 维护 violation 历史状态。

### 2.4 Roadmap(影响设计但不在第一版实现)

- **merge/split 修复**:把相邻 filler 合并或把宽 filler 切开来消 violation。
- **multi-height**:跨多行的 cell/filler。
- **batch opto**:一次多个 changed cell。

这三项决定了 §4 的 Move 抽象和 §5 的接口预留;它们的具体扩展路径见 §8。

---

## 3. 总体架构

### 3.1 职责边界

- **checker RD**:只做 non-mutating overlay DRC verify。输入一批
  `OverlayCheckRequest`(每个是一个 atomic overlay 方案),输出对应的真实 violation
  snapshot。checker 是唯一的 DRC oracle——MW/MS/P/N/PRL 的全部规则细节封装在
  checker 内,repair engine 对其免疫。
- **infrastructure RD**:只做 filler 替换候选查询(第一版 per-instance,演进为
  per-span tiling 查询,§5.3)。commit 由 infrastructure 执行,不在 repair 内。
- **filler repair engine**:utility preflight、violation 归一化、开窗、move 生成、
  排序、子集搜索、baseline-delta 判定、结果与 diagnostics。实现为 deterministic
  **pure planner**,真实 DB/checker 通过 adapter 接入,先用 fake adapter 单测。

第一版代码位置:`src/dpl2/src/fillerRepair/`。

### 3.2 五层管线

repair engine 内部是五层管线,每层单独可测、单独可替换:

```text
┌─ 窗口控制外环(L0 → L1 → L2,预算耗尽则升级,§6.3)─────────────┐
│                                                                  │
│  ① MoveGenerator   窗口内生成候选 move                           │
│                    第一版:仅 SwapMove;未来:+Merge/Split/…      │
│  ② Ranker          启发式排序(5 特征起步,只影响评估顺序,       │
│                    不判合法性,§6.6)                             │
│  ③ SubsetSearcher  按 rank 序枚举 size-1/2/3… 的非冲突 move      │
│                    子集,分批产出(§6.7)                          │
│  ④ OracleGate      canonical cache + batch checker call +        │
│                    baseline-delta clean 判定(§6.8)              │
│  ⑤ Result/Diag     首个 clean 即返回;预算尽扩窗;仍无解则       │
│                    diagnostics(§6.9)                            │
└──────────────────────────────────────────────────────────────────┘
```

选型理由(简单/高效/稳定/可扩展四维度):

- greedy 是 size-1 枚举、group seed 是人工挑选的 size-2/3 枚举、beam 是大子集空间的
  启发式路径——三者统一成"排序枚举"一条代码路径,**没有 partial overlay 打分、没有
  survivor 配额等 magic knob**,只有 clean/not-clean 二值判定,天然免疫 MW 非单调
  陷阱(双 filler 联动在枚举里只是一个普通的 size-2 子集)。
- 批间无串行依赖,batch checker API 可并行;好排序下答案通常在首批 16-32 个候选里。
- 未来 move 类型增加时,只扩展①②层;③④⑤层对 move 类型无感知(§4/§8)。
- beam 保留为 escape hatch(§8.3),第一版不实现。

---

## 4. 核心抽象:Move = Span Rewrite(planner 内部)

### 4.1 定义与不变量

**planner 内部**的算法原子操作定义为**区段重铺**,而不是"instance 换 master"。
注意这是 repair engine 的内部抽象;第一版对外(checker / infrastructure)的
wire format 保留 V1 的 `FillerChange`,由 adapter 无损转换(见 §5.2):

```cpp
struct FillerRewrite
{
    std::vector<RowId> rowIds;        // 第一版恰好 1 行
    XInterval span;                   // 被重铺的 x 区段
    std::vector<MasterId> newMasterIds;  // 从左到右依次实例化
};
```

不变量(planner 侧按构造保证,checker 侧可校验后判 `InvalidOverlay`):

- `span` 必须精确覆盖若干**完整的 filler instance**(不切开任何 instance 的一部分,
  不触碰 std cell/macro/blockage)。
- `sum(newMasterIds 的宽度) == span 宽度`,即重铺后不留 gap、不 overlap——
  100% utility 由 move 不变量自动保持。
- 每个 new master 必须是 filler master,且 site/row/orientation 兼容。

第一版(只换 type)是它的退化形式:`span` 恰好等于一个 filler instance 的 span,
`newMasterIds` 恰好一个元素,宽高与原 master 相同。merge = span 覆盖 2+ 个 filler、
单元素 masterSeq;split = span 覆盖 1 个 filler、多元素 masterSeq;multi-height =
`rowIds` 多行。**升级 move 类型不改变本节以下的任何定义。**

之所以内部不用 `FillerChange{instanceId, newMasterId}` 作原子操作:merge/split 会
创建/销毁 instance,新 instance 在 commit 前没有 instanceId,以 instanceId 为锚的
冲突判定、canonical key 与 delta 分类(§6.8)都会在扩展时失效;span 几何锚不会。

**wire format 取舍**:第一版所有 move 都是"单 filler 同尺寸换 master",
`SwapMove ↔ FillerChange` 一一对应、无损互转,因此对外接口保留 V1 形态,
`FillerRewrite` 作为 checker API 的演进形态列入 future work(§8.1)。扩展性真正
依赖 span 锚的部分(canonical key、冲突、相关性判定)全部在 planner 内部,不经过
checker API,所以这个取舍几乎不损失算法侧扩展性;代价是 merge/split 落地时
checker API 需要一次版本化升级。

### 4.2 canonical key、冲突与 cache

- 一个 **overlay 候选** = 一组 move(rewrite)的集合。
- canonical key = `sorted_unique((rowIds, span, newMasterIds))`。checker call 前必须
  用 canonical key 去重并查 cache;cache value 至少含 `CheckResult`、score 摘要和
  checker call index。
- 两个 move **冲突** = 存在同行且 span 相交(第一版"同一 instanceId 不能出现两次"
  是其特例)。冲突的 move 不进入同一个 overlay;若一个 overlay 内出现冲突 move,
  该 overlay 直接判 invalid,不发 checker。
- 同一 canonical overlay 在一次 repair 内只调用一次 checker。

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
`relation(IntraInstance/IntraRow/InterRow)`,`primaryLayer/secondaryLayer`,
sorted-unique `rowIds`,`xWindow`,`measuredValue/requiredValue`,`participants`)。
checker 侧必须保证:

- `rowIds` 提供且 sorted unique;inter-row violation 包含所有涉及行。
- `ViolationParticipant` 含 `instanceId/masterId/rowId/xRange/isFiller/isTarget`,
  其中 `rowId` 用于区分同 x 位置、不同 row/participant 的 violation。
- violation 不需要 stable id;repair 侧自己生成临时 signature(§6.2)。

### 5.2 Checker overlay API(第一版 wire format = V1 `FillerChange`)

```cpp
struct FillerChange
{
    InstanceId instanceId = 0;  // 只能是 filler instance
    MasterId newMasterId = 0;   // 同宽/同高/orientation 兼容的 filler master
};

using OverlayRequestId = int;

struct OverlayCheckRequest
{
    OverlayRequestId requestId = -1;  // repair 生成,batch 内唯一
    TargetPlace targetPlace;
    Rect guardRegion;                 // repair window 外扩 two-cell guard halo
    std::vector<FillerChange> fillerChanges;  // 一个 atomic overlay 候选
};

enum class CheckStatus { Checked, InvalidOverlay, Unsupported, CheckerError };

struct CheckResult
{
    OverlayRequestId requestId = -1;  // 必须 echo 请求的 requestId
    CheckStatus status = CheckStatus::CheckerError;
    bool isLegal = false;             // 仅 status == Checked 时有意义
    std::vector<Violation> violations;
    std::vector<Diagnostic> diagnostics;
};

class ImplantOverlayChecker
{
 public:
  CheckResult checkPlaceWithOverlay(const OverlayCheckRequest& request) const;
  std::vector<CheckResult> checkPlaceWithOverlays(
      const std::vector<OverlayCheckRequest>& requests) const;
};
```

checker 实现 overlay 的语义与 V1 相同:把每个 `fillerChanges` 中的 filler instance
按 `newMasterId` 换 master 后,在 target-local 规则下重查。planner 内部的 SwapMove
由 adapter 无损转换为 `FillerChange`。

**future work(merge/split 前置条件)**:届时 API 升级为 span 形态的
`FillerRewrite`(§4.1/§8.1),那是一次版本化的 breaking change。为降低升级成本,
现在需要 checker RD 遵守两点:① 接口版本化预留(升级时新增 v2 接口而非原地改
语义);② 不把"overlay 不会创建/销毁 instance"的假设固化到 checker 内部深处
(例如 violation participant 的 id 引用方式)。

**为什么不现在就切到 `FillerRewrite`(决策记录)**:span 形态的完整契约依赖
尚未定稿的设计——overlay 中新创建的 filler 没有 instanceId,`CheckResult` 的
`ViolationParticipant` 如何引用它(合成 id 还是 span 引用)直接影响 delta 分类
协议;multi-height 的 `rowIds` 对齐语义、commit 侧 instance id 分配流程同样未定。
现在拍板是投机性 API,届时大概率仍需 breaking。而 merge/split 落地时 checker
反正要新写 overlay 内删除/实例化 filler 的实现,API 升级与该工作同批完成,不产生
额外成本;V1 用 `instanceId` 引用则是零歧义、零新增工作量。上述 participant
引用问题应作为未来 v2 API 设计的第一个议题。

协议约定(与 V1 相同,原样保留):

- non-mutating,不 commit DB。
- 一个 request 内的 `fillerChanges` 是 atomic candidate,必须一起 overlay 后检查;
  batch API 输入是 request vector,不在一个 request 里嵌套 candidates。
- 每个 request 必须携带 `guardRegion`;checker 至少在该区域内 collect violations。
  `guardRegion` 只扩大 checking/diagnostics 范围,**不允许** repair 修改 guard-only
  区域内的 filler。
- 每个 `CheckResult` 必须 echo 对应 `requestId`;batch 返回顺序建议与输入一致,但
  correctness 不依赖顺序。result 缺失、重复/未知 `requestId`、single API echo 错误,
  一律视为 checker protocol error,repair 拒绝该 batch。
- 单个 invalid/unsupported request 只影响自己的 `CheckResult`。
- `status != Checked` 必须给 diagnostics;`Checked && !isLegal` 尽量给 violations 或
  diagnostics;`Checked && isLegal && violations 非空` repair 仍按不 clean 拒绝。
- checker 不需要维护 violation 历史,也不做 original/residual/new/spillover 分类;
  分类由 repair 侧用同一 `guardRegion` 的 baseline/overlay delta 完成(§6.8)。

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

class FillerMasterCandidateProvider
{
 public:
  MasterCandidateResult getUsableMasterCandidates(
      const MasterCandidateRequest& request) const;
};
```

语义(与 V1 相同):输入必须是 filler instance;candidates 只含可直接替换的
filler master(不含当前 master),保证 same width / same height / same site /
orientation 兼容;没有可用替换时返回 empty candidates + diagnostics,不是 error。
当前工艺下每个 filler 恒有 2 个同尺寸候选(3 VT − 当前,附录 A);
empty candidates 保留为防御性路径,不是常态。

**演进方向**(merge/split 需要,提前告知 infrastructure RD):查询按几何键而非
instance 键——"在 `(rowId, width, orientation)` 下有哪些可用 filler master(或
master 序列)可铺满该宽度"。第一版实现建议内部就按宽度建表,per-instance API 做成
它的 wrapper,升级时零迁移。

### 5.4 Repair planner API 与 utility preflight

```cpp
struct FillerRepairRequest
{
    TargetPlace targetPlace;
    std::vector<Violation> violations;  // checker 的初始 snapshot
};

struct FillerRepairResult
{
    bool hasSolution = false;
    std::vector<FillerChange> changes;  // 第一版 wire format,见 §5.2
    std::vector<Diagnostic> diagnostics;
};
```

100% utility preflight 由 repair engine 自己基于 DB/placement adapter 完成,不要求
checker/infrastructure 提供 API。结构与 V1 相同:

```cpp
enum class CoverageIssueKind { Gap, Overlap, OffGrid, IllegalOccupant };

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
```

---

## 6. 修复流程

### 6.1 100% utility preflight(硬前置条件)

每个合法 std-cell site 必须被 std cell 或 filler 精确覆盖一次;gap / overlap /
off-grid / illegal occupant 都是 precondition failure。macro、blockage、core cutout
等非法 site 由 DB adapter 排除。实现上可维护 occupancy cache + dirty range,语义上
必须等价于全量检查。

失败语义:不生成 move、不调 checker、`hasSolution=false`、`changes` 为空、
diagnostics 带 fatal/error 级 `NonFullUtility`(或等价 code)并报告至少一个 gap 的
row/x range/site count。原因:本方案按 span rewrite 保持覆盖不变量;若 design 本来
就没铺满,正确动作是先跑 filler insertion / placement repair,不是让 VT repair
承担补洞职责。

### 6.2 violation 归一化与 signature

对每条 violation 抽取:`type`(MW/MS)、`relation`(intra/inter-row)、`rowIds`
(为空时退回 `targetPlace.rowId` 并在 diagnostics 标记)、`xRange`(xWindow 与
participant bbox 的 union)、`vtHint`(MW 用 primary layer,MS 用 primary/secondary
layer,**按 band-slot / P-N 区分别记**)、`cellAnchors`(至少含
`targetPlace.instanceId`,再加参与或邻接的 fixed std cell)、`fillerParticipants`。

**signature 匹配键(钉死,delta 分类依赖它)**:

```text
signature = (ruleId, kind, relation, sorted(rowIds), xWindow 重叠 ≥ 阈值)
```

两条 violation 跨 baseline/overlay result 匹配,当且仅当前四项相等且 xWindow 重叠
超过阈值(建议:重叠长度 ≥ min(两者长度) 的一半,或距离 ≤ 1 site)。

**"与本次改动相关"的定义(钉死)**:violation 的 participants 触及任一 changed
span,或其 xWindow 距任一 changed span ≤ 1 个 rule distance。以 span 几何为锚而非
instanceId,保证 merge/split 时代 instance 被销毁后判定依然成立。

**swap-unfixable 快速判定**(借鉴 DAC'23 [Zou et al.] 的 unsolvable violation
分类思想,弱化为保守充分条件):若某条 original violation 的 xWindow 外扩
two-cell ring 内不存在任何 editable filler,则任何 swap move 都不可能影响它——
直接 fast-fail:`hasSolution=false`,diagnostics 记 `UnfixableByTypeSwap`,
不进入搜索、不调 checker(0 次 call)。判定只用几何与 participant 信息,不依赖
规则语义,不越 checker-as-oracle 边界。这类 case 通常意味着需要 merge/split
(§8.1)或上游回退该 opto 改动;明确的失败码让上游能区分"搜索无解"与
"结构性不可修"。

### 6.3 开窗:L0 → L1 → L2 与 guardRegion

窗口用于圈定 candidate filler、限制搜索、和 delta 分类;它不是 `targetPlace`。
三级窗口,从小到大:

| 级别 | 内容 |
|---|---|
| L0 | violation participants ∪ anchor std cell 左右相邻 filler ∪ bridge filler(见下) |
| L1 | L0 基础上:横向 snap 到完整 filler instance 并扩到最近 fixed cell/blockage/core 边界;纵向包含耦合相邻行(±1) |
| L2 | 合并所有 overlap/相邻且距离小于一个 rule distance 的 L1 窗口 |

- intra-row violation 从 L0 入,inter-row 从含 ±1 行的 L0 入(bridge 集合天然覆盖)。
- 升级触发:result 非 clean 且 remaining violation 的 xWindow/participants 靠近当前
  窗口边界;或当前窗口枚举预算耗尽仍无 clean。
- **扩窗截止**(借鉴 DAC'23 contour refinement 的终止准则):若升级后的窗口没有
  引入任何新的 editable filler/move,或(完备枚举前提下)升级后 remaining
  violation 集合与上一级完全相同,则违例与更远的 filler 无关——停止扩窗,直接
  进入失败路径,不烧剩余预算。
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
`repairWindow`、`guardRegion` 与 halo 来源。同一窗口生成的所有 `OverlayCheckRequest`
携带同一个 `guardRegion`;guard-only 区域的 filler 只参与 checking/diagnostics,
不得出现在 `fillerChanges` 中(违反判 invalid request)。

### 6.4 cluster:第一版单 cluster

opto 一次只改一个 cell,本次 snapshot 的所有 violation 都在同一 anchor 邻域内。
第一版直接把它们并成**一个 cluster、一个(逐级放大的)窗口**一次求解,等价于 V1
从合并窗口起步,但删去了"cluster 独立求解 → final merge → conflict-graph retry"
整套机制。该机制在 batch opto(多 anchor)时代再引入(§8.4)。

最终返回前仍保留一道 **final full-overlay check**:用覆盖所有 changed span 的
`guardRegion`(≥ merged window 外扩 two-cell halo)对完整 changes 做一次
single-overlay(或单元素 batch)check,仍以 baseline-delta clean 判定。单 cluster
下该检查通常与搜索中最后一次 clean check 相同,可由 cache 直接命中;保留它是为了
守住"返回值一定经过整体验证"的公理,且在扩窗/多窗口合并路径下不可省略。

### 6.5 move 生成(第一版:SwapMove)

对窗口内每个 editable filler:

1. 调 `getUsableMasterCandidates`;为空则该 filler 不生成 move,diagnostics 记
   no usable master。
2. 对每个 candidate master 生成一个 SwapMove(内部为
   `{ {rowId}, instanceSpan, {newMasterId} }` 的 span 表示,对外经 adapter 映射为
   `FillerChange{instanceId, newMasterId}`),并把 master 映射到 `Family`/VT
   供排序使用。
3. 绝不为 std cell、macro、guard-only filler、non-editable filler 生成 move。

**anchor-follow 首发种子(固定)**:根因永远是"anchor 换到新 VT、周围 filler
还是旧色",且库 VT 齐全保证该方向恒可构造(附录 A)。因此首批固定包含:
baseline + "anchor 相邻 filler 与 bridge filler 全部换成 anchor 新 VT"的 overlay +
它的 size-1/2 子集。预期多数 repair 一个 batch 即 clean,对 opto 内环总耗时是
乘法级收益。

**其余组合建议(group hint)**:MoveGenerator 同时产出少量高价值 move 组合,直接
插入③层同 size 队列最前——anchor 左右 bridge filler pair、上下行与 cell 边界对齐
的 bridge pair、同一短 filler run 整段同改、同一 violation 的 filler participants
全组。这继承了 V1 group seed 的价值,但只是枚举顺序的提示,不是独立搜索机制。

### 6.6 排序(Ranker)

排序只影响 checker call 次数,不影响正确性。第一版 5 特征,lexicographic 比较:

| 优先级 | 特征 | 含义 |
|---|---|---|
| 1 | `directParticipant` | filler 出现在 violation participants 中 |
| 2 | `bridgeScore` | 位于 anchor 与相邻大 implant 区之间的短 filler |
| 3 | `cellAnchorVote` | targetVT == anchor std cell 的新 VT(本问题最常见正解方向) |
| 4 | `width` | 更窄优先 |
| 5 | `position` | x 更小优先,再 row 更小,保证 deterministic |

target VT 顺序:① anchor 的新 VT;② 邻接 majority VT(**按 band-slot 分别计数**,
不能按 cell 整体数);③ 稳定 type id。所有顺序都只在
candidate provider 实际返回的 master 中取值(防御库变化;当前库各宽度 VT 齐全,
附录 A)。

**第三 VT 强降权**:三档 VT 下,每个 filler 的两个候选中总有一个"既非 anchor 新
VT、也非邻接 majority"的第三色,几乎不可能是解的一部分——固定排到 move 列表
队尾(降权,**不剔除**,三 VT 相邻的犄角场景仍可达)。有效分支因子由此从 2 降到
~1,有效搜索空间从 3^k 缩到 ~2^k;配合 §6.7 的完备枚举,正确性完全不依赖该启发。

fixed cell 是投票和约束,不是禁改理由(贴着 fixed cell 的 filler 往往最该先试)。
V1 的其余特征(`fillerVote`/`diffEdgesRemoved`/`multiViolationTouch`/`islandScore`/
`sameVtBefore`/`cellConflict`)列为 backlog,fake-checker 测试显示排序命中率不足时
再逐个引入;merge/split 时代加"move 类型偏好(swap 优先)、changed span 面积小者
优先"两项。backlog 还包括 **model-guided proposal**(DAC'23 的 inference /
forced-assignment 思想:用近似局部规则模型推导"该 filler 必须是某色否则必然
违例"的强制赋值,用于排序与剪枝)——anchor-follow 种子与第三 VT 降权正是它的
弱化版;模型不准只多花 checker call,正确性始终由 ④OracleGate 保证。

### 6.7 子集搜索(SubsetSearcher)

从排序后的 move 列表 `m1..mM` 枚举 overlay 候选:

- **枚举顺序** = 按 `(子集 size, 成员 rank 的字典序)`;anchor-follow 首发种子与
  group hint 插到对应 size 队列最前(§6.5)。
- **小窗口完备枚举(优先规则)**:窗口内 editable filler 数为 k 时,完整子集空间
  = 3^k 个着色方案(每个 filler 恒 2 候选,附录 A)。当 `3^k ≤ 剩余预算` 时
  (k ≤ 5 对应 243 ≤ 512),**完整枚举全部非冲突子集**,仍按 rank 序分批、首
  clean 早停。此时"窗口内无解"是**确定性结论**——扩窗触发精确、不可能漏解,
  排序只影响速度不影响完备性,`hasSolution=false` 的诊断含义从"预算耗尽"升级为
  "窗口内确定无解"。
- **成员限制(仅大窗口)**:`3^k > 剩余预算` 时启用截断——size 1 允许全部 M 个;
  size ≥ 2 限制在 rank 前 `N_s` 的 move 中(建议 `N_2 = 24`,`N_3 = 12`,
  `N_4 = 8`;size > 4 不枚举,直接扩窗)。
- 跳过含冲突 move 的子集;canonical key 去重、查 cache(§4.2)。
- **分批验证**:每批 16-32 个候选发 batch checker;批内按枚举序取第一个
  delta-clean 作为解(保证确定性),命中立即停止。
- **预算**:每窗口 checker call 上限 512(含 baseline)。预算耗尽或枚举完仍无
  clean → 窗口升级(L0→L1→L2);窗口耗尽 → 失败路径(§6.9)。

MW"必须多 filler 联动、单改不改善甚至更差"的非单调 case,在这里只是一个普通的
size-2/3 子集,不需要任何特殊机制;好排序 + group hint 下通常出现在首批。

### 6.8 accept gate:baseline-delta clean(唯一 accept 标准)

对每个窗口/guardRegion,先发一次 **baseline request**(同 `targetPlace`、同
`guardRegion`、空 `fillerChanges`),再比较 candidate overlay result。candidate 判
**delta-clean** 当且仅当:

1. `status == CheckStatus::Checked` 且无 checker fatal/protocol diagnostics;
2. 初始 snapshot 的 original violations 在 overlay result 中全部消失
   (signature 匹配,§6.2);
3. `repairWindow` 内没有新增 violation(overlay 有、baseline 无);
4. guard halo 内没有"与本次改动相关"(§6.2 定义)的新增或迁移 violation;
5. guard halo 内 baseline 已有且与本次改动无关的 violation **不导致失败**,只进
   diagnostics。

辅助规则(与 V1 相同):`Checked && isLegal && violations 非空` 按不 clean 拒绝;
`status != Checked` 的 request 只保留 diagnostics,不参与成功候选。

失败时为 diagnostics 挑选 best overlay 用的比较序(lexicographic,不用加权
magic number):

```text
deltaClean(true 绝对优先) > checkerError=false > checkerIllegal=false
> 相关 violation 数少 > changed span 总面积小 > 枚举序靠前
```

### 6.9 final check、输出与失败路径

- `hasSolution=true`:`changes` 已通过 final full-overlay check 的 baseline-delta
  clean(§6.4)。
- `hasSolution=false`:`changes` 为空(不返回 partial repair,避免把违例挪走但未清
  干净;explicit partial mode 未来可加,不默认开启),diagnostics 说明失败原因:
  swap-unfixable(§6.2,结构性不可修)、无合法 master、fixed-cell 冲突、
  枚举/call 预算耗尽、窗口到顶(含扩窗截止,§6.3)、所有 overlay 均未
  delta-clean;附 best overlay、remaining violations、窗口与预算统计。
- utility preflight 失败:`hasSolution=false`、changes 为空、fatal diagnostics,
  语义是 placement precondition 不满足,不是搜索无解。

---

## 7. 预算与参数(初值)

| 参数 | 值 | 说明 |
|---|---|---|
| 每窗口 checker call 上限 | 512 | 含 baseline;与 V1 持平 |
| batch 大小 | 16-32 | 批内枚举序定序 |
| 完备枚举阈值 | 3^k ≤ 剩余预算(k ≤ 5) | 满足则完整枚举,无解结论确定(§6.7) |
| size-2/3/4 成员上限 N_s | 24 / 12 / 8 | 仅大窗口截断时启用;超出则依赖扩窗 |
| 最大子集 size | 4 | 更大组合交给扩窗后的 L1/L2 |
| 窗口级数 | L0/L1/L2 | 到顶即失败路径 |

所有参数进 config,diagnostics 打印实际取值。

---

## 8. 扩展性设计

### 8.1 merge/split

- **①层增量**:新增 MergeMoveGenerator / SplitMoveGenerator,只提议少数高价值
  tiling(两个 bridge filler 合一、在 cell 边界切开宽 filler、短 run 重铺),控制
  分支爆炸的位置在生成器,不在搜索。split 的价值在于比 swap 更细的
  VT 粒度(例如 4 → 2+2 允许 span 的半段换 VT、半段保持,是 swap-only 覆盖不了的
  解形态);tiling 每段宽度必须取自库中实际存在的宽度集合(附录 A,当前为
  {2,3,4,8})。tiling 生成器可参考 DAC'23 的 DP row-optimal insertion(状态 =
  位置 × VT-interval 长度 × filler-interval 长度 × label,配 inter-row cost
  table 可线性化):在 span 上求近似违例最少的 tiling 作为 proposal,checker 仍作
  终判;MF(min filler width)约束由库宽度集合自然满足。
- **②层增量**:排序加 move 类型偏好与 span 面积项(§6.6)。
- **③④⑤层零改动**:冲突判定(span 相交)、canonical key、cache、delta 分类
  (span 几何锚)、gate、diagnostics 全部按 §4/§6.8 的定义直接适用。
- **接口(前置条件)**:checker API 从 `FillerChange` 版本化升级为 span 形态的
  `FillerRewrite`(§4.1/§5.2 已预留约定);infrastructure 候选查询切换到
  per-span tiling 键(§5.3 演进方向)。
- **不变量**:重铺精确覆盖 span、宽度和相等,100% utility 继续按构造保持。

### 8.2 multi-height

- `FillerRewrite.rowIds` 多行;窗口按"跨行 instance 拉入其全部行"规则扩展(§6.3);
- utility preflight 对 multi-row instance 按行分别记覆盖;
- 候选查询增加 row parity / orientation 约束(P/N band 翻转行),由 infrastructure
  候选 API 吸收;
- checker 已有 band-slot / inter-row 模型,oracle 边界不变;
- 搜索核不变。

### 8.3 beam escape hatch

当窗口内可行 move 数大到排序枚举预算不够(预计出现在 merge/split 时代的大窗口),
在③层后插入 beam searcher 作为替代子集生成器。它复用②的排序与④的 gate,不引入
新的 accept 语义。第一版不实现,只保留此接口位。

### 8.4 batch opto / 多 cluster

一次多个 changed cell 时,再引入 V1 的 cluster 划分(violation 构图连通分量)与
"独立求解 → final merge → conflict merge retry"机制;单 anchor 场景不需要。

---

## 9. diagnostics

至少记录:preflight 状态(失败时 gap row/x range/site count);anchor
(`targetPlace` 五元组);窗口级别与实际 `repairWindow`/`guardRegion`/halo 来源;
初始 violation 数与归一化 signature;候选 filler 数、生成 move 数、group hint 数;
枚举子集数、canonical cache 命中数、checker call 数(batch 次数与 batch size);
baseline result 摘要;final result 是否 checked/legal/delta-clean 与 returned
violation 数;best overlay 及其分类(residual original / new inside-window /
related-in-halo / unrelated-in-halo 统计);bridge filler ids;失败原因枚举。

---

## 10. 测试集(fake checker / fake provider)

前置与协议:

- 非 100% utility:存在 gap 时直接 fatal,不调 checker,changes 为空。
- guard-only filler 出现在 `fillerChanges` 中,判 invalid request。
- 非 filler instance 出现在 `FillerChange` 中,判 invalid request。
- planner 内部 Move 不变量(span 精确覆盖完整 instance、宽度和相等)单测,
  为 future `FillerRewrite` 升级预置。
- batch result 必须 echo `requestId`;乱序返回不影响 correctness;缺失/重复/未知
  id 判 protocol error 拒绝整个 batch。
- batch 中单个 invalid overlay 不影响其他 result。
- canonical 相同 overlay 只调一次 checker(cache 命中)。

gate 语义:

- `status != Checked` 拒绝并记录 diagnostics。
- `Checked && isLegal && violations 非空` 拒绝。
- `Checked && !isLegal && violations 为空` 拒绝并记录 diagnostics。
- overlay 修掉 original 但 repairWindow 内新增 violation,拒绝。
- overlay 修掉 original 但 guard halo 出现相关新增/迁移 violation,拒绝。
- guard halo 中 baseline 已有且无关的 violation,不导致 candidate 失败。

搜索行为:

- intra-row MS:单 filler swap 修好(size-1 首批命中)。
- inter-row MS:单 filler swap 修好。
- MW:必须两个 filler 同时改才 clean;单改不改善——size-2 子集或 group hint 命中。
- 同一位置两条 violation(不同 participant / 不同 P-N band),不去重,一起解。
- 一个 filler 关联多条 violation;一个 anchor 引发多条 violation。
- 三 VT:邻居 majority 不是正确的 anchor VT(验证 target VT 顺序)。
- 缺 same-size replacement master(fake provider 构造;当前真实库 VT 齐全,
  此 case 为防御性,附录 A),返回 no usable master diagnostics;fixed cell 约束冲突。
- 必须扩窗(L0 不够,L1 修好);枚举预算耗尽触发扩窗;窗口到顶返回 no solution
  且 diagnostics 带 best overlay 与 remaining violations。
- swap-unfixable 快速判定:violation 外扩 ring 内无 editable filler,0 次
  checker call 直接 fail,diagnostics 带 `UnfixableByTypeSwap`。
- 扩窗截止:升级窗口无新增 editable filler/move 时停止扩窗,不烧剩余预算。
- anchor-follow 首发:典型单/双 filler case 在首个 batch 内 clean。
- 小窗口完备枚举:窗口内确无解时,枚举完 3^k 空间后**确定性**扩窗(诊断区分
  "确定无解"与"预算耗尽")。
- 第三 VT 降权但可达:正解需要第三色的犄角 case 仍能被找到。
- 确定性:同输入两次运行,产出完全相同的 changes/diagnostics/call 序列。

---

## 11. 实现 TODO

1. 定义 pure planner API(`FillerRepairRequest -> FillerRepairResult`)、内部
   Move(`FillerRewrite`)与 canonical key/冲突判定,以及 Move ↔ `FillerChange`
   的 adapter 转换。
2. fake checker + fake candidate provider,先锁定 overlay/协议语义。
3. 100% utility preflight(fatal 短路路径)。
4. violation 归一化 + signature 匹配(§6.2 的钉死规则)。
5. L0/L1/L2 window builder + guardRegion 生成。
6. SwapMove 生成器 + bridge filler 收集 + group hint。
7. Ranker(5 特征,per-band 计数)。
8. SubsetSearcher(排序枚举、批产出、预算)。
9. OracleGate(batch wrapper、canonical cache、baseline-delta gate、best-overlay
   记录)。
10. final full-overlay check 与输出/diagnostics。
11. §10 测试集全绿。
12. 等 `ImplantOverlayChecker::checkPlaceWithOverlays` 稳定后接真实 checker;
    `src/dpl2` CMake 接入后纳入 build/test。

---

## 12. 与 V1 的差异清单

| # | 项目 | V1 | V2 定稿 | 理由 |
|---|---|---|---|---|
| 1 | 原子操作 | `FillerChange{instanceId, newMasterId}` | planner 内部升格为 span rewrite(swap 是退化形式);对外 wire format 保留 `FillerChange`,`FillerRewrite` 列入 future work | merge/split 会创建/销毁 instance,instanceId 锚在扩展时失效;span 锚的冲突/cache/delta 分类全在 planner 内部,接口可延后升级 |
| 2 | 搜索内核 | greedy prefix + beam(N/K/D、survivor 配额、partial 打分) | 排序枚举 move 子集 + batch 验证,首 clean 即停 | 一条代码路径;无 partial 打分噪声与 magic knob;MW 非单调 case 是普通 size-2 子集;批并行友好 |
| 3 | 窗口 | W0-W5 六级,按类型选入口 | L0/L1/L2 三级 | 规则尺度 ~1 site,六级状态过多;语义等价、测试面减半 |
| 4 | cluster | 划分 + 独立求解 + final merge + conflict-graph retry | 单 cluster(opto 单 cell),final check 保留 | 单 anchor 下所有 violation 同邻域;merge retry 机制移到 batch opto 时代 |
| 5 | 排序 | 12 特征 + 6 级 tie-break | 5 特征起步,其余 backlog;per band-slot 计数 | 排序只影响 call 数;先测命中率再加特征 |
| 6 | signature | "至少 ruleId+kind+relation+rowIds,尽量加…" | 匹配键与"相关性"定义钉死(§6.2) | delta 分类是最脆一环,不能留自由度 |
| 7 | 既有 violation | baseline-delta 允许 halo 无关违例 | 同 V1,并明确"忽略邻域遗留违例场景"为假设;顺手修历史违例列为未来 score 增量 | 按最新需求收敛 |
| 8 | beam | 主路径兜底 | escape hatch 接口位,第一版不实现 | 枚举预算不够时才需要 |
| 9 | 候选 API | per-instance | per-instance(wrapper)+ per-span tiling 演进方向 | merge/split 需要按几何键查询 |
| 10 | 扩展性 | 未系统化 | §8 专章:merge/split、multi-height、batch opto 的分层扩展路径 | 新增维度 |

保留不变的 V1 决策:checker-as-oracle(不复刻 DRC)、只在 checker 结果上 accept、
baseline-delta gate 与 guard halo 语义、two-cell guardRegion、canonical cache、batch
协议(requestId echo 等)、100% utility preflight 归属 repair、pure planner + fake
adapter 先行、失败不返回 partial、diagnostics 要求。

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
`Family` 的映射由 adapter 从 master 的 implant 层导出(参见
`ImplantLayerCheckerHelper` 的 layer 解析路径),不依赖 master 名字符串解析,
本附录命名仅供人读。

对算法的推论(备注性质,算法不 hard-code 这张表,一切以
`FillerMasterCandidateProvider` 运行时返回为准):

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
5. **齐全库解锁的三项搜索精化**(已并入正文):anchor-follow 首发种子恒可构造
   (§6.5);第三 VT 强降权,有效分支因子 ~1(§6.6);小窗口 3^k 完备枚举,
   "窗口内无解"成为确定性结论(§6.7)。

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

- **问题互补**:该工作是 insertion 阶段的全局求解器,正是本 spec §6.1 preflight
  失败时应先运行的那类上游工具;本 feature 是 insertion 之后、opto 内环的增量
  repair。规则形式化(intra/inter MW/MS、三 VT)一致。
- **架构差异**:该工作自建完整规则引擎(它没有 checker-in-the-loop);本 feature
  保持 checker-as-oracle,不复刻 DRC——但它证明了近似规则模型作为 **proposal
  生成器**的有效性,模型不准只多花 checker call,不影响正确性。
- **已采纳**(V1):swap-unfixable 快速判定(§6.2,源自其 unsolvable violation
  分类);扩窗截止准则(§6.3,源自其 contour 终止准则)。
- **已列入 future**:DP row-optimal tiling 作为 split/tiling 生成器参考
  (§8.1);model-guided proposal 作为 Ranker backlog(§6.6)。
- **反向验证**:其 inference 规则生成的强制赋值,与本 spec 的 anchor-follow
  首发种子、第三 VT 降权方向一致;其"部分违例在 filler 阶段无解"的分类,
  佐证 `hasSolution=false` + 明确失败码的输出语义。
