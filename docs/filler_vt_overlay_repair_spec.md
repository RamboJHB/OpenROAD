# 功能规格 — Filler VT Overlay 修复 V2.1(checker-guided,本阶段 swap-only)

状态:**V2.1 定稿**。分支:`claude/wizardly-carson-secahu`。基线:`2023-base`。
最后更新 2026-07-13。对齐 `src/dpl2/src/fillerRepair` 实现与 `src/dpl2/src/drc`
checker 源码。

V2.1 相对 V2 是一次 reviewer 驱动的修订,聚焦三处高价值改动:**OracleGate 正确性、
窗口模型简化、搜索域建模**。完整修订记录见 [§0](#0-v21-修订记录);V2 相对 V1 的
差异清单见 [§12](#12-与-v1-的差异清单)。V1 存档文件(`spec_v1` / `addendum_v1`)
已作为冗余文档移除,其内容被 §12 差异表完整覆盖。

**一句话**:design 已 100% utility(无空 site);opto/ECO 一次改一个 std cell 的
VT/type 后产生 implant MW/MS 违例。checker 把 violation snapshot 交给 filler repair
engine;repair engine 通过 utility precheck 后,在局部窗口内生成 **swap move**
(同位置同尺寸换 filler master),按启发式排序后**枚举 move 子集并
成批交给 checker overlay API 验证**,用同一 `guardRegion` 下的 baseline-delta clean 作为
唯一 accept 标准。找到 clean 解返回 `FillerRepairResult`,由上游 commit;修不了则返回
diagnostics,不动 DB。

---

## 0. V2.1 修订记录

V2 定稿并完成 planner 实现(TODO 1–11,35 个确定性测试)后,拿到真实 checker
源码,做了一次 reviewer 复审。以下 12 项修订都在 **repair engine 范围内**
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
| 6 | 简化 | `UnfixableByTypeSwap` 用无 oracle 的 ring 论证做 hard abort,与 checker-as-oracle 有张力,收益极小 | 降级为 Warning 提示,不提前终止,仍走正常搜索 | §6.2 |
| 7 | 简化 | 单 cluster 下 L2 恒等于 L1、ExpansionCutoff 必触发,名义三级实际两级 | 删 L2,窗口模型改为 L0 + adaptive-L1 | §3.2、§6.3、§7 |
| 8 | 简化 | L1 一次扩到 fixed/core 边界可吞整条 filler run,`3^k>预算` 立即退化为截断枚举 | 改渐进扩窗:每步向 blocking 侧扩 K≈2 个 filler,尽量维持完备枚举 | §6.3 |
| 11 | 简化 | final full-overlay check 用相同 cacheKey,必然 cache 命中,零验证增益 | 删除该步;clean 一经 §6.8 判定即返回 | §6.4、§6.9 |
| 9 | 建模 | 成员上限作用在扁平 swap 列表上,高排名 filler 占满前缀,关键 filler / 第三 VT 可能整体出局 | 先 rank filler、每 filler 保留全部 master domain,再枚举 per-filler 赋值;上限按 filler 数 | §6.6、§6.7 |
| 12 | 建模 | 每次 repair 全设计逐行扫描过重;`rowLegalSpan` 单区间表达不了多段 legal segment | full-utility 状态由 infrastructure 按 design revision 缓存下发,planner 只做窗口局部 O(window) 防御性复核 | §5.4、§6.1 |

落地分三批(实施顺序,详见 `src/dpl2/HandOff.md`):
**批 1 正确性** #1/#2/#3/#4/#5/#10 —— 都是小改动、现有 harness + ScriptedChecker
可造回归;**批 2 简化** #6/#7/#8/#11 —— 减代码,改动集中在窗口相关 case;
**批 3 建模** #9/#12 —— #9 动 Ranker/SubsetSearcher 接口,#12 随 adapter 对接。

保留不采纳原文的一处:#4 的原始处方(对 related pre-existing 也否决)会让那条
连 baseline 都有的违例在每个 candidate 里都出现、导致 repair 恒失败;正确修法是把
它并入 #2 的 baseline 一致性门——按 §2.3 假设,窗口内/相关的 pre-existing violation
本就不该存在,发现即报输入异常。

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

- design 100% utility(硬前置条件,repair engine 自己 precheck,§6.1)。
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
  `OverlayCheckRequest`(每个是一个 atomic overlay 方案),输出对应的真实 violation
  snapshot。checker 是唯一的 DRC oracle——MW/MS/P/N/PRL 的全部规则细节封装在
  checker 内,repair engine 对其免疫。
- **infrastructure RD**:只做 filler 替换候选查询(第一版 per-instance,演进为
  per-span tiling 查询,§5.3)。commit 由 infrastructure 执行,不在 repair 内。
- **filler repair engine**:utility precheck、violation 归一化、开窗、move 生成、
  排序、子集搜索、baseline-delta 判定、结果与 diagnostics。实现为 deterministic
  **pure planner**,真实 DB/checker 通过 adapter 接入,先用 fake adapter 单测。

第一版代码位置:`src/dpl2/src/fillerRepair/`。

### 3.2 五层管线

repair engine 内部是五层管线,每层单独可测、单独可替换:

```text
┌─ 窗口控制外环(L0 → adaptive-L1,预算尽/近边界则渐进扩窗,§6.3)──┐
│                                                                  │
│  ① SwapGenerator   窗口内生成候选 swap                           │
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

### 3.3 依赖方向与调用拓扑(checker↔engine"互相依赖"的裁定)

集成事实:**checker 调用 engine** 生成 fix solution,**engine 调用 checker**
验证 overlay——表面上是循环依赖。裁定:用依赖倒置消解,**engine 是底层,
方向恒为 checker → engine,无环**。

**编译/链接层(无环)**:

- fillerRepair 是纯底层库,**不 include 任何 checker 头**。它调用的"checker"
  是它自己定义的抽象接口 `fillerRepair::ImplantOverlayChecker`(CheckerApi.h);
  wire 类型和基础 id/interval 都由 `fillerRepair::` 自有的 UDM-free
  `Types.h`/`BaseTypes.h` 提供(§5.2 决策);checker 的 `ipl::` 类型独立,
  只在 adapter 中显式转换。
- checker → engine 单向依赖:checker 侧的 repair 入口 include fillerRepair 头,
  构造 engine,并把**自己**包进 `CheckerOracleAdapter :
  public fillerRepair::ImplantOverlayChecker` 注入(adapter 同时做 ipl↔
  fillerRepair 的类型换皮:Orient↔PhysOrientation、columnId·siteWidth↔DBU、
  Region↔CheckerRect)。candidate provider 的真实实现同理:基于 checker 的
  master 表,adapter 实现 engine 的 `FillerMasterCandidateProvider`。
- 构建目标:libfillerRepair(零依赖)← checker/drc。adapter 文件放
  `fillerRepair/adapter/`(允许 UDM 头)或 drc/ 侧,随 checker 目标链接。

**运行时(无递归)**:

```text
checkPlace(检测) → checker 的 repair 入口
    → engine.repair(snapshot)                  [checker→engine,具体调用]
        → oracleAdapter.checkPlaceWithOverlay  [engine→接口,抽象调用]
            → checker 的 overlay 查询(const,纯查询)
    → engine 返回 FillerRepairResult
→ checker/infra commit
```

协议红线(并入 §5.2):`checkPlaceWithOverlay[s]` 是**纯查询**,禁止在内部
触发 repair;repair 只能从检测/修复入口进入。防御:repair 入口加不可重入
assert(in-repair flag)。overlay API 本身 const、不 mutate DB,不会再进
commit/检测路径,递归按构造不可能。

**生命周期**:engine 纯 planner、per-request 状态自隔离,checker 每次修复调用
就地构造(便宜);adapter 无状态(包 this)。

**否决的备选**:(a) 把 engine 并进 checker 类——毁掉纯 planner 的独立
编译/测试(当前全部单测不依赖 UDM/CMake 的能力就没了);(b) checker 也只认
engine 的抽象接口(双向抽象)——checker→engine 是具体的单向调用,无环,
再抽象是空转;(c) `std::function` 回调注入——与现有抽象接口等价但类型面更弱;
(d) 第三方 orchestrator 拥有两者(flow: check→repair→commit)——架构上最
干净,但当前集成事实是 checker 驱动;engine 对"谁驱动"不敏感,将��N���$z{-���jםling(两个 bridge filler 合一、在 cell 边界切开宽 filler、短 run 重铺),控制
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
- **接口(前置条件)**:checker API 从 `FillerChange` 版本化升级为
  primitive-op 形态(remove/add + overlayId echo,职责归属与协议见 §5.2 决策
  记录);infrastructure 候选查询切换到 per-span tiling 键(§5.3 演进方向)。
- **不变量**:重铺精确覆盖 span、宽度和相等,100% utility 继续按构造保持。

### 8.2 multi-height

- `FillerRewrite.rowIds` 多行;窗口按"跨行 instance 拉入其全部行"规则扩展(§6.3);
- utility precheck 对 multi-row instance 按行分别记覆盖;
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

至少记录:precheck 状态(失败时 gap row/x range/site count);anchor
(`targetPlace` 五元组);窗口级别与实际 `repairWindow`/`guardRegion`/halo 来源;
初始 violation 数与归一化 signature;候选 filler 数、生成 swap 数;
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
- (future)rewrite 不变量(span 精确覆盖完整 instance、宽度和相等)单测,
  随第二步 rewrite 一起引入,本阶段无此代码。
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
- swap-unfixable 提示:violation 外扩 ring 内无 editable filler,diagnostics 带
  `UnfixableByTypeSwap` Warning,但仍进入正常搜索(不再 fast-fail,V2.1 #6)。
- 扩窗截止:扩窗后无新增 editable filler/move 时停止扩窗,不烧剩余预算。
- anchor-follow 首发:典型单/双 filler case 在首个 batch 内 clean。
- 小窗口完备枚举:窗口内确无解时,枚举完 3^k 空间后**确定性**扩窗;definitive 只
  按最后搜索窗口的完备性断言(L0 完备 + L1 截断 ⇒ 非 definitive,V2.1 #10)。
- 第三 VT 降权但可达:正解需要第三色的犄角 case 仍能被找到。
- 确定性:同输入两次运行,产出完全相同的 changes/diagnostics/call 序列。

---

## 11. 实现 TODO

1. 定义 pure planner API(`FillerRepairRequest -> FillerRepairResult`)与
   `Swap` 结构、overlay cache key。
2. fake checker + fake candidate provider,先锁定 overlay/协议语义。
3. 100% utility precheck(fatal 短路路径)。
4. violation 归一化 + signature 匹配(§6.2 的钉死规则)。
5. L0 + adaptive-L1 window builder + guardRegion 生成。
6. Swap 生成器(只产原子 swap move;组合由⑧枚举、方向由⑦排序承担)。
7. Ranker(5 特征;当前单-VT projection 已实现,真实 P/N per-band 计数随 adapter
   元数据补齐)。
8. SubsetSearcher(排序枚举、批产出、预算)。
9. OracleGate(batch wrapper、canonical cache、baseline-delta gate、best-overlay
   记录)。
10. final full-overlay check 与输出/diagnostics。
11. §10 测试集全绿。
12. 等 `ImplantOverlayChecker::checkPlaceWithOverlays` 稳定后接真实 checker;
    `src/dpl2` CMake 接入后纳入 build/test。

状态(2026-07-13):TODO 1–11 在当前单-VT planner projection 下已实现,
`src/dpl2/src/fillerRepair/` 下 79 个确定性测试普通版/ASan 全绿。SubsetSearch
的 exact `space == budget` 完备性边界已修复;planner 基础类型已迁到
`fillerRepair/BaseTypes.h`,可与 checker header 在同一 adapter TU 共存。
TODO 12(真实 checker/infra 对接 + CMake)未做,checker 源码已导入
`src/dpl2/src/drc`。V2.1 修订(§0)落地进度,详见 `src/dpl2/HandOff.md`:
批 1(OracleGate 正确性 #1/#2/#3/#4/#5/#10)**已完成**;批 2(窗口/管线简化)
**#6/#7/#8/#11 全部完成**(adaptive-L1 已替代边界 sweep);
批 3(搜索域建模)**#9 filler-domain 枚举已完成**,#12 随 adapter 对接。真实
`ImplantLayerChecker` core 已在 fake-UDM boundary 下直接编译并通过 8 个用例及
ASan:4 个 dense width/spacing × intra/inter-row,以及 raw/rowIds、类型共存、
invalid-batch 隔离、row/hash/guard 回归。这不等同于 TODO 12 adapter/真实 UDM
extraction 完成。`ranker_majority_per_band` 仍等待真实 band 元数据;checker 的
bridge-MW 专用 raw-vs-blocking fixture 与真实 adapter E2E 仍待补。

---

## 12. 与 V1 的差异清单

| # | 项目 | V1 | V2 定稿 | 理由 |
|---|---|---|---|---|
| 1 | 原子操作 | `FillerChange{instanceId, newMasterId}` | 本阶段内外统一 swap/`FillerChange`(`Swap` 带 row/span 几何元数据);无通用 Move 抽象层;rewrite 与 primitive-op API 全部列入 future work(§4.3/§5.2/§8.1) | 操作演进钉死为两步:swap → rewrite;merge/split 的抽象与 API 升级同批设计,避免投机 |
| 2 | 搜索内核 | greedy prefix + beam(N/K/D、survivor 配额、partial 打分) | 排序枚举 move 子集 + batch 验证,首 clean 即停 | 一条代码路径;无 partial 打分噪声与 magic knob;MW 非单调 case 是普通 size-2 子集;批并行友好 |
| 3 | 窗口 | W0-W5 六级,按类型选入口 | L0 + adaptive-L1(V2.1;V2 曾为 L0/L1/L2) | 规则尺度 ~1 site,多级状态过多;渐进扩窗避免一次吞整段 run 后退化为截断枚举(V2.1 #7/#8) |
| 4 | cluster | 划分 + 独立求解 + final merge + conflict-graph retry | 单 cluster(opto 单 cell),final check 保留 | 单 anchor 下所有 violation 同邻域;merge retry 机制移到 batch opto 时代 |
| 5 | 排序 | 12 特征 + 6 级 tie-break | 5 特征起步,其余 backlog;per band-slot 计数 | 排序只影响 call 数;先测命中率再加特征 |
| 6 | signature | "至少 ruleId+kind+relation+rowIds,尽量加…" | 匹配键与"相关性"定义钉死(§6.2) | delta 分类是最脆一环,不能留自由度 |
| 7 | 既有 violation | baseline-delta 允许 halo 无关违例 | 同 V1,并明确"忽略邻域遗留违例场景"为假设;顺手修历史违例列为未来 score 增量 | 按最新需求收敛 |
| 8 | beam | 主路径兜底 | escape hatch 接口位,第一版不实现 | 枚举预算不够时才需要 |
| 9 | 候选 API | per-instance | per-instance(wrapper)+ per-span tiling 演进方向 | merge/split 需要按几何键查询 |
| 10 | 扩展性 | 未系统化 | §8 专章:merge/split、multi-height、batch opto 的分层扩展路径 | 新增维度 |

保留不变的 V1 决策:checker-as-oracle(不复刻 DRC)、只在 checker 结果上 accept、
baseline-delta gate 与 guard halo 语义、two-cell guardRegion、canonical cache、batch
协议(requestId echo 等)、pure planner + fake adapter 先行、失败不返回 partial、
diagnostics 要求。(V2.1 收窄:full-utility 权威结果改由 infrastructure 缓存下发,
planner 只做窗口局部防御性复核,§5.4/§6.1 #12。)

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
