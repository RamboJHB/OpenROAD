# HandOff — Filler VT Overlay Repair(交接文档)

> 写给下一个 agent。**我们的责任范围是 repair engine(`src/dpl2/src/fillerRepair/`)。
> checker 的实现不归我们**;checker 的事实只作为 engine 依赖的契约记录在 §5,不是
> 我们的任务。本轮 reviewer 复审把 12 项 engine 内的改动固定成 spec **V2.1**
> (`docs/filler_vt_overlay_repair_spec.md` §0),本文把它们拆成可执行的三批工作。

**开工顺序:先读 spec §0 修订记录 + §6,再读本文 §3 的批次清单,然后从批 1 开始。**
spec 与本文冲突时以 spec 为准。

---

## 1. 项目一句话

设计 100% utility;opto 一次只改一个 std cell 的 VT;周围 filler 还是旧 implant
类型,产生 implant MW/MS violation。repair engine 拿到 checker 的 violation 快照,
**只通过同宽同高同位置的 filler master 换型(swap)** 找一组 `FillerChange`,交回
checker 用 overlay 验证,干净则由 infrastructure commit。engine 永远不碰 DB、永远
不自己判 DRC(checker-as-oracle)。阶段词汇:**本阶段只有 swap;下一阶段才是
rewrite(merge/split)**。代码里不允许出现 Move / FillerRewrite 抽象。

## 2. 现状(截至 commit `b5dd6b2`,分支 `claude/wizardly-carson-secahu`)

| 部分 | 位置 | 状态 |
|---|---|---|
| Spec **V2.1**(含 §0 修订记录) | `docs/filler_vt_overlay_repair_spec.md` | 定稿 |
| 纯 planner engine(TODO 1–11) | `src/dpl2/src/fillerRepair/` | 已实现,**但按 V2 旧语义**;35 个确定性测试全绿 |
| checker 源码 + spec §5 接口脚手架 | `src/dpl2/src/drc/ImplantLayerChecker.{h,cpp}` | 类型/声明在;overlay API 是 stub(非我方责任) |
| fake checker / design / candidate provider | `src/dpl2/src/fillerRepair/fake/` | 继续用于单元测试 |

**关键认知**:engine 代码是 **V2 语义**,spec 已升到 **V2.1**。下面三批工作就是把
engine 从 V2 收敛到 V2.1。V1 存档 spec(`spec_v1` / `addendum_v1`)已删除(冗余)。

## 3. 三批工作(按此顺序;每批做完跑 `test/run_tests.sh` 必须全绿)

优先级:**OracleGate 正确性 > 窗口简化 > 搜索域建模**。这三处比再加 ranking 特征
更有价值。每条都标了 spec §0 的修订号、file:line 依据和验证方式。

### 批 1 — OracleGate 正确性 ✅ 已完成(commit 见 git log,40 个测试全绿)

> 全部落地。5 个新回归测试:`gate_rejects_unexplained_illegal`(#1)、
> `gate_baseline_mismatch_aborts_search`(#2+#4)、
> `gate_multiset_new_violation_not_absorbed`(#3)、
> `gate_per_violation_rule_distance`(#5)、
> `engine_definitive_reflects_last_window`(#10:L0 完备 + L1 截断 ⇒ 报 truncated)。
> `runBaseline` 签名改为收 `RepairWindow`(baseline 一致性门需要 window)。
> 下面保留每条的实现说明,供审阅与回溯。

**1.1 双向自洽拒绝(#1)** — `OracleGate.cpp:88`
现在 `summary.inconsistent = result.isLegal && !result.violations.empty();` 只捕获
一个方向。反方向 `!isLegal && violations.empty()`(未解释的非法结果)会落到 `:131`
的 clean 判定被**误 accept**。真实 checker 恰好会产生这种形状(blocking overlap、
site 不对齐、polarity mismatch → `isLegal=false` + 只有 diagnostics,见
`ImplantLayerChecker.cpp:1503-1533`);fake checker 从不产生,所以现有测试没抓到。
修:`inconsistent = (result.isLegal != result.violations.empty())`。
`CheckerApi.h:47` 的 `isCheckedResultUsable` 已经写对了这个思想,classify 没用它。
注意 spec §10 的 gate 测试清单里**本来就要求**这条("`Checked && !isLegal &&
violations 为空` 拒绝")——是实现漏了,补测时用 ScriptedChecker 造该形状即可。

**1.2 BaselineMismatch 门(#2+#4)** — `OracleGate.cpp:runBaseline` (`:36-74`)
现在 baseline 只查 `status==Checked`(`:64`),不验证 original 是否复现。而
residualOriginals 是拿 originals 匹配 candidate(`:91-98`)——若输入快照过期、
baseline 里根本没这些 violation,candidate 里当然也没有,residual=0 → 误判"修好"。
修:baseline 拿到后,用 `sameSignature` 确认**每条 original 都在 baseline 里**;
且 baseline 的窗口内/相关 violation 不多于 original。任一不满足 → `BaselineMismatch`
fatal,engine abort 请上游重出快照。这条同时吸收 #4(见 spec §0 末尾"不采纳原文"
说明:related pre-existing 不在 classify 里重判,而在这道门拦下)。

**1.3 multiset 一对一匹配(#3)** — `OracleGate.cpp:102-111`
现在每条 candidate violation 独立扫 baseline、不消耗。`sameSignature` 有一个 site
的容差(`Signature.cpp:127-133`),所以两条同 signature 的 candidate violation 会被
同一条 baseline finding 一起吸收,漏掉真正新增的那条。修:baseline / candidate 各
当 multiset,按 signature 做消耗式一对一匹配(一条 baseline 只抵消一条 candidate)。
residualOriginals 那个循环方向是保守的(只多报不漏报),顺手一起改成一对一更干净。

**1.4 per-violation ruleDistance(#5)** — `OracleGate.cpp:124` + `Signature.cpp:157`
`rule_distance_` 构造时定死为 originals 的最大 requiredValue(`Signature.cpp:164`)。
`isRelatedToOverlay` 用它给**新** violation 判 halo 相关性;新违例若来自 requiredValue
更大的规则会被误判 unrelated 放行。修:判新违例时用
`max(rule_distance_, v.requiredValue)`。

**1.5 definitive 语义(#10)** — `FillerRepairEngine.cpp:205` + `:223`
`anyDefinitive |= plan.complete && !sr.budgetExhausted;` 是 OR 累积;L0 complete +
L1 truncated 仍会宣称 "window space exhausted definitively"(`:223`)。逻辑站不住
(L0 complete 只证明 L0 无解)。修:不用 `|=`,记录**最后一个实际搜索过的窗口**的
complete 标志;ExpansionCutoff 提前 break 时沿用 break 前那轮的值。

### 批 2 — 窗口/管线简化(减代码)

> 状态:**#6 / #7 / #11 已完成**(loop 改为 `level<=1`,unfixable 降级 Warning,
> 删 finalCheck + `OracleGate::finalCheck`)。**#8 adaptive-L1 仍待做**(下面 2.2)——
> L1 目前仍是"sweep 到边界"的老实现。

**2.1 删 L2(#7)✅** — `FillerRepairEngine.cpp` loop 改为 `level<=1`、`Window.*`
注释、spec §6.3。单 cluster 下 L2==L1,ExpansionCutoff 必然触发,白付一次
buildWindow + 诊断噪音。已删;level 循环结构保留给 rewrite 阶段留口。

**2.2 adaptive-L1(#8)** — `Window.cpp` buildWindow(L1 分支)、`FillerRepairEngine.cpp`
主循环。现在 L1 横向 snap 到 fixed/core 边界可吞整条 filler run → `fullSpaceSize`
超 budget(`SubsetSearch.cpp:50`)→ 退化成 size≤4 + member cap 截断枚举。改为:
每轮向 **best 非-clean candidate 的 blocking violation 所在侧**扩固定 K≈2 个 filler,
重算 `plan.complete`,循环到预算尽或扩窗截止。engine 现有的 `best`(`:196-204`)已经
在追踪 best 非-clean summary,扩窗方向从它的 blocking violation 取。

**2.3 swap-unfixable 降级(#6)✅** — `FillerRepairEngine.cpp` stage 2b。原来
`hasFillerNearViolation` 失败会提前 `return`(hard fail)。已改为:不提前 return,
只 push 一条 **Warning** 诊断,继续正常搜索。`hasFillerNearViolation` 保留作 hint。

**2.4 删 finalCheck(#11)✅** — `FillerRepairEngine.cpp` foundClean 分支 +
`OracleGate::finalCheck`。finalCheck 用相同 cacheKey → 必然 cache 命中 → 零验证增益。
已删:`sr.foundClean` 后直接 `result.changes = toFillerChanges(...)` 返回,
`OracleGate::finalCheck` 方法及声明一并移除。

### 批 3 — 搜索域建模(#12 随 adapter)

**3.1 filler-domain 枚举(#9)✅ 已完成** — `Ranker.*` 返回
`std::vector<FillerDomain>`(filler 按 direct/bridge/width/position 排序,每个
filler 保留**完整** master domain,domain 内按 anchor VT → 邻接 majority → 稳定
master id 排序、第三 VT 在 **domain 内**垫底);`enumerateOverlays` 枚举
"filler 组合(字典序)× domain 赋值(末位 filler 变最快)",memberCap 按
**filler 数**;同 filler 冲突按构造不可能(dup 检查删除)。回归测试
`enumeration_filler_domain_not_crowded_out` 锁住"cap 按 filler 后跨 filler
size-2 组合存在"(旧语义下 cap=2 只覆盖 f1 的两个 option,size-2 一个都出不来)。
**注意枚举顺序语义变化**:size-1 从"全体主选先于全体第三 VT"变为"逐 filler
展开完整 domain"(f1.third 先于 f2.best)——spec §6.6 V2.1 文本本就如此定义;
用户 5 行 grid 测试因此换了一个同样 oracle-clean 的首解(2012→vt0,原 3013→vt1),
测试已更新并注明原因。

**3.2 precheck 上收(#12)** — `PreCheck.*`、`FillerRepairEngine.cpp:42`
现在每次 repair 全设计逐行扫(O(design))。改:full-utility 权威结果由 infrastructure
按 design revision 缓存下发,planner 消费;engine 保留 **窗口内 O(window) 防御性
复核**(这同时化解 `rowLegalSpan` 单区间表达不了 macro/blockage 多段的问题——窗口
内单段假设成立,多段由 adapter 报 issue)。此项随 §4 的 adapter 对接一起做。

### 未决:输出格式(需先问用户)

当前是 all-or-nothing(修不干净 → `hasSolution=false` + BestOverlay 诊断)。用户问过
"能否把修不掉的 violation 一起返回",给过 A(维持)/ B(部分修复模式)/ C(结构化
残留 violation 字段),我推荐 C,**用户尚未拍板**。动 `FillerRepairResult` 前先问。

## 4. checker / infra 依赖(**不是我们的任务**,但 engine 依赖这些契约成立)

engine 接真 checker 前,这些必须由 checker/infra 侧就位。记录在此仅为让 engine 侧
知道边界、并在集成时能验证。

1. **checker 已交付真实 overlay API(2026-07-12,helper 已删)**;两条契约项 +
   一个编译 bug **已在 checker 侧改好**(带 `[fillerRepair-fix]` 标记,说明文档
   `drc/CHECKER_REPAIR_CONTRACT.md`):新增 `checkPlaceWithOverlaysRaw`(不做
   blocking 过滤,复用 `checkOverlayRegion`)、填充 `Violation.rowIds`、对齐
   `validateOverlayRequest` 头/实现。**待 checker RD review 并在其完整环境
   (UDM+Grid)编译通过前,engine 仍只接 `fake/FakeImplantChecker`**;对接时
   用 raw API,不用 blocking 形态。
2. **新 wire 形态(以实物为准)**:`checkPlaceWithOverlays(request, guard,
   vector<vector<FillerChange>>)`——单 target/guard + N 候选,结果按输入顺序
   关联(无 requestId/status;invalid 候选 = 诊断 + isLegal=false,逐候选隔离)。
   engine 协议校验届时改 size+order;participants 由 adapter 从
   `violation.instances` + `placedInsts` 合成;kind 从 ruleSource 推导。
3. **adapter 层**(engine 侧要写,属我方):`ipl::` ↔ `fillerRepair::` 类型转换。
   三处易埋 bug 的转换:`Orient`↔`PhysOrientation`、`x`(DBU)↔
   `CheckRequest.colId`/`PlacedInst.colId`(**site 单位**,`x = colId*siteWidth`)、
   `Region`(row-based)↔ guard `eUTL::Rect`(y-based,`y = rowId*rowHeight`)。
   建议放 `src/dpl2/src/fillerRepair/adapter/`(可含 UDM 头,planner 本体保持
   UDM-free),重点单测这三个转换。**前置**:planner 先自持基础类型(Types.h
   停止 alias `ipl`)——checker 新 header 已在 `ipl` 命名空间自定义同名类型,
   `ImplantBaseTypes.h` 现为 planner 专用,同 TU include 两边会 ODR 冲突。
4. **依赖拓扑已钉死(spec §3.3 / AGENTS D15)**:checker 调 engine(具体、单向
   编译依赖),engine 调 checker 只经自己的抽象 oracle 接口——无编译环;
   `checkPlaceWithOverlay[s]` 是纯查询、禁止内部触发 repair——无运行时递归。
   repair 入口建议加不可重入 assert。candidate provider 的真实实现坐在 checker
   的 master 表上,数据通路已由 `fake/FakeUdmCandidateProvider`(AGENTS D16)
   预演:VT = implant layer family(parseLayerName),绝不解析 master 名。

## 5. 红线(违反任一 = 返工)

1. **只做 swap。** 不引入 Move / FillerRewrite / merge / split(下一阶段词汇,spec §4.3 留口)。
2. **checker-as-oracle。** engine 永不判 DRC;干净与否只来自 baseline-delta 门(§6.8)。
3. **engine 是纯 planner、确定性。** 相同输入 → 相同 changes/诊断/调用次数;不碰 DB,
   本体不含 UDM 头(UDM 只允许出现在 adapter/)。
4. **fake 与 35 个测试不许删不许改语义。** 每次改动跑 `test/run_tests.sh`,必须全绿
   (`-Wall -Wextra -Werror`);V2.1 的每条改动都应补对应回归 case(ScriptedChecker /
   MisbehavingChecker 已够用)。
5. **debug print 规范**:清晰的 因→果 逻辑链 + data change,别废话(参考 `Log.h`
   的 stage 标签 + `cat()` 用法,`FR_VERBOSE=1` 打开)。
6. **commit 风格**:小步、单主题、`fillerRepair:` / `drc:` 前缀,message 说清因果。

## 6. 构建 / 测试 / 工作方式

```bash
cd src/dpl2/src/fillerRepair/test
./run_tests.sh                       # 35 cases,应输出 "OK: 35 test(s) passed"
./run_tests.sh <name-substr>         # 按名字过滤
FR_VERBOSE=1 ./run_tests.sh <case>   # 完整逻辑链日志
```

- 分支:`claude/wizardly-carson-secahu`,只在此分支开发/commit/push。
- 开发范围:**只在 `src/dpl2/`**(fillerRepair/ + 必要时 drc/ 的 adapter 支持)+
  `docs/` spec。
- 读代码顺序:spec §0 + §6 → `README.md` → `FillerRepairEngine.cpp`(主循环)→
  `OracleGate.cpp`(判定核心,批 1 的战场)→ `SubsetSearch.cpp` / `Ranker.cpp`(批 3)。

## 7. 关键文件速查

| 文件 | 作用 |
|---|---|
| `docs/filler_vt_overlay_repair_spec.md` | 权威 spec(V2.1);§0 = 本轮 12 项修订记录 |
| `src/dpl2/src/fillerRepair/OracleGate.{h,cpp}` | 批 1 战场:baseline-delta 门 + 协议校验 + cache |
| `src/dpl2/src/fillerRepair/FillerRepairEngine.{h,cpp}` | 主循环:窗口升级、definitive 语义(#10)、swap-unfixable(#6)、finalCheck(#11) |
| `src/dpl2/src/fillerRepair/Window.{h,cpp}` | 批 2 战场:L0/L1、guardRegion、adaptive-L1(#8)、删 L2(#7) |
| `src/dpl2/src/fillerRepair/Ranker.{h,cpp}` / `SubsetSearch.{h,cpp}` | 批 3 战场:filler-domain 枚举(#9) |
| `src/dpl2/src/fillerRepair/Signature.{h,cpp}` | signature / relatedness;ruleDistance(#5) |
| `src/dpl2/src/fillerRepair/PreCheck.{h,cpp}` | precheck 上收(#12) |
| `src/dpl2/src/drc/ImplantLayerChecker.{h,cpp}` | checker(非我方责任);overlay stub 在 `:3273`,spec §5 字段填充点在 `:1885`/`:2454` |
| `src/dpl2/src/fillerRepair/fake/` | 单元测试用假件(保留) |
| `src/dpl2/src/fillerRepair/test/` | 35-case harness + run_tests.sh |
