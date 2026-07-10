# AGENTS.md — Filler VT Overlay Repair 项目记忆(dpl2)

> 本文件是给 AI agent(和人)的**项目记忆总档**:做了什么、为什么这么决策、
> 否决过哪些备选、还剩什么坑、下一步怎么走。目标是没有聊天记录也能无缝接手。
> 事实以 git 历史与 spec 为准;本文负责"为什么"。
>
> **文档分工**(避免重复维护):
> - `docs/filler_vt_overlay_repair_spec.md` — **权威 spec(V2.1)**,算法/接口/
>   协议的唯一裁判;§0 是 12 项修订的索引表。
> - `src/dpl2/HandOff.md` — 可执行任务清单(三批工作,file:line 级指引)。
> - `src/dpl2/src/fillerRepair/README.md` — 模块速览与 TODO 状态表。
> - `src/dpl2/src/fillerRepair/test/TestPlan.md` — 测试拓展作业指导书。
> - 本文 — 决策记忆。改动语义时,spec 先行,然后同步 HandOff/README,最后在
>   本文追加决策记录。

---

## 1. 项目一句话与边界

设计 100% utility(所有 site 被 std cell/filler 覆盖);opto/ECO **一次只改一个
std cell 的 VT**;周围 filler 保留旧 implant type,产生 implant 层 MW/MS 违例
(MS 分 P/N band;规则尺度约 1 site)。checker 把 violation snapshot 交给
repair engine;engine **只用同宽同高同位置的 filler master 换型(swap)** 找一组
`FillerChange`,交回 checker 用 overlay 验证;clean 则 infrastructure commit。

硬边界:
- engine 是 **deterministic pure planner**:不碰 DB、不判 DRC(checker-as-oracle)、
  同输入同输出。
- 开发只在 `src/dpl2/`(`fillerRepair/` 为主,`drc/` 只放共享类型与 checker 侧
  脚手架)+ `docs/`。
- **checker 的实现不是本项目责任**(用户明确划界);checker 事实只作为 engine
  依赖的契约记录。
- 词汇红线:本阶段只有 **swap**,下一阶段才是 **rewrite**(merge/split)。代码中
  不允许出现 Move / FillerRewrite 抽象(它们是论文概念,用户明确否决过,见 §4-D3)。

## 2. 现状(2026-07-09,分支 `claude/wizardly-carson-secahu`)

| 部分 | 状态 |
|---|---|
| Spec | **V2.1 定稿**(§0 修订记录 = 12 项 review 裁定);V1 存档已删(冗余) |
| planner(`src/fillerRepair/`) | TODO 1–11 实现;V2.1 **批 1 全落地**、**批 2 落地 #6/#7/#11**、**批 3 落地 #9(filler-domain 枚举)**;**#8/#12 未做** |
| 测试 | 56 个,`-Werror` + ASan 全绿(`test/run_tests.sh`);TestPlan 第一批拓展(`a1a0750`,tester 提交,15 个)已 review 合入 |
| checker(`src/drc/`) | 真实源码已导入 + spec §5 类型脚手架;**overlay API 是 stub,永远报 clean,严禁接给 engine**(engine 现在只接 fake) |
| 对接(adapter/CMake,TODO 12) | 未做 |

关键 commit(倒序):#9 filler-domain(见 git log 最新)→ `d293265` AGENTS/TestPlan
→ `a7de6f0` 批 2 部分(#6/#7/#11)→ `c1bafce` 批 1(6 个正确性修复 + 5 回归测试)
→ `df18536` spec V2.1 + 删 V1 存档 → `b5dd6b2` HandOff 初版 → `b7b72ab` checker
脚手架 → 更早为 planner 实现史。

## 3. 演进时间线(为什么会走到 V2.1)

1. **V1 spec**(greedy prefix + beam、W0-W5 六级窗口、cluster 划分/合并/重试)
   → review 后判定机制过多、magic knob 过多。
2. **V2 重写**:统一成"排序枚举 + batch 验证 + baseline-delta 唯一判定";
   窗口砍成 L0/L1/L2;单 cluster;签名钉死。期间吸收:12-master 库事实
   (每宽度 3 VT 齐全)→ 三项搜索精化(anchor-follow 靠排序、第三 VT 降权、
   小窗口 3^k 完备枚举);DAC'23 论文 → unfixable 判定与扩窗截止准则。
3. **实现 TODO 1–11** + 35 测试;导入真实 checker 源码打脚手架。
4. **Reviewer 复审**(拿到 checker 实现后,用户提供 12 条批评逐条裁定)→
   **V2.1**:OracleGate 正确性 > 窗口简化 > 搜索域建模。裁定全文见 spec §0;
   实施拆三批,批 1 全落地、批 2 落地 3/4。

## 4. 决策日志(决策 / 思考过程 / 否决的备选)

### D1. checker-as-oracle,engine 不复刻 DRC
**决策**:合法性只来自 checker;engine 只做窗口/枚举/分类。
**为什么**:MW/MS/P-N/PRL 语义复杂且会漂移,复刻 = 双份真相,必然分叉。
**否决备选**:DAC'23 式本地规则模型做主判定。保留其思想为 Ranker backlog
(model-guided proposal):模型不准只多花 checker call,不影响正确性。

### D2. 排序枚举替代 greedy+beam(V2 核心选型)
**决策**:①生成 ②排序 ③按 (size, rank字典序) 枚举子集 ④oracle 门 ⑤首 clean 早停。
**为什么**:greedy 是 size-1 枚举、seed 是人工 size-2/3、beam 是大空间启发式——
三者本是同一枚举的特例;统一后没有 partial 打分、没有 survivor 配额,MW 非单调
(必须两 filler 联动)只是普通 size-2 子集。
**否决备选**:保留 beam 为主路径(留作 escape hatch 接口位,spec §8.3,不实现)。

### D3. 只做 swap,不引入 Move/FillerRewrite 抽象 ⚠️ 用户强修正
**决策**:原子操作就是 `Swap`(= FillerChange 语义 + row/span/VT 元数据)。
**过程教训**:实现初期我引入了论文式 Move 抽象 + FillerRewrite + 冲突判定 +
adapter 转换,被用户明确叫停("你做多了,我们只做 swap!")。操作演进钉死为
两步:swap(本阶段)→ rewrite(下一阶段);merge/split 的抽象与 API 升级
届时同批做,现在做是投机。
**残留**:canonicalKey 仅作 checker-call cache key 存活;"冲突"按构造不可能
(枚举按 filler 分组)。

### D4. wire format 保留 V1 `FillerChange`;v2 升级为 primitive-op 形态(future)
**决策**:现在不改 API;merge/split 时代升级为 `remove(instanceId)* +
add(master,row,x,orient,overlayId)*`,overlayId 由请求方分配、checker echo。
**为什么**:swap-only 下 FillerChange 零歧义;op 形态消解"overlay 新建 filler
没有 instanceId、participant 无法引用"的协议难题,且与 commit 原语同构。
**否决备选**:现在就上 FillerRewrite 结构——multi-height 行对齐、id 分配流程
未定,投机 API。
**配套职责裁定**:overlay 删/建的**语义半**(tiling 规划,删哪些/放哪些)归
engine,**机械半**(candidate context 应用与规则执行)归 checker——tiling
知识不泄漏进 oracle,checker 索引不泄漏出 checker。

### D5. 类型二元性是设计决定,靠薄 adapter,不"统一"
**决策**:`fillerRepair::Types.h` 持有与 `ipl::` 结构同构的 UDM-free wire 类型;
共享基础 id + XInterval 放 `drc/ImplantBaseTypes.h`(checker 侧持有,planner alias)。
**为什么**:`ipl::CheckRequest` 内嵌 `eUTL::PhysOrientation`(UDM),UDM 头进
planner 就毁掉纯度与独立编译测试。
**三个已知转换坑**(adapter 必须单测):`Orient`↔`PhysOrientation`;
x(DBU)↔`PlacedInst.columnId`(**site 单位**,`x=columnId*siteWidth`);
`Region`(row-based)↔`CheckerRect`(y-based,`y=rowId*rowHeight`)。

### D6. baseline-delta 是唯一 accept 门;guard 进 cache key
**决策**:每窗口先跑空-overlay baseline(同 targetPlace/guard),candidate 与之
做 delta;cache key = guard + canonicalKey(同 overlay 换 guard 是不同问题)。
**为什么**:checker 不维护违例历史、不做 original/new 分类——分类责任全在
engine,才能对 checker 的实现细节免疫。
**halo 规则**:窗口内新增 → 拒;halo 相关新增 → 拒;halo 无关 pre-existing →
永不否决(否则邻区历史违例把一切可行解全毙掉)。

### D7. 签名必须含 implant layer;不按几何位置去重
**决策**:signature = (ruleId, kind, relation, primaryLayer, secondaryLayer,
sorted rowIds) + xWindow 容差(重叠 ≥ 短窗一半,或距离 ≤ 1 site)。
**为什么**:P/N band 在同一 x 间隙产生两条 MS,除 layer 外全同——按位置去重
直接违反问题事实(用户图例明确)。layer 未填时默认 0/nullopt,对
layer-agnostic checker 是 no-op。

### D8. V2.1 十二项裁定(用户提出 12 条批评,我逐条对代码核实后裁定)
全表见 spec §0。**特殊处理的三条**及理由:
- **#4(pre-existing related halo)**:观察成立但**处方换掉**。原处方"对
  related pre-existing 也否决"会让那条 baseline 里就有的违例在每个 candidate
  里都出现 → repair 恒失败。正确修法:并入 #2 的 baseline 一致性门——按 §2.3
  假设,窗口内/相关的 pre-existing 本就不该存在,出现即输入异常,
  `BaselineMismatch` fatal。
- **#6(unfixable fast-fail)**:不删逻辑,**降级为 Warning hint**。ring 论证
  大概率安全但无 oracle 佐证(ring 按 instance 计数、规则按 DBU,安全性依赖
  规则尺度小);而收益极小——没有附近 filler 时 L0 本就 NoEditableFiller、
  零 checker call。降级近乎免费的保险。
- **#11(finalCheck)**:**删除**而非升级成 expanded-guard 认证。相同 cacheKey
  → 必然 cache 命中 → 零验证增益;expanded-guard 有真实价值但每次 +2 call
  (新 guard 要新 baseline),留到 batch opto/多 guard 时代按实测预算决定。

### D9. BaselineMismatch 门的 scoping(批 1 实现时的关键决策)
**决策**:"originals 必须在 baseline 复现"只对 **in-guard** originals 生效;
"不得有意外违例"只对 **in-window** 的 baseline 违例生效(halo 的意外违例
= 允许的无关 pre-existing)。
**为什么**:baseline 是在 guardRegion 内收集的;original 落在本级窗口 guard 之外
时"看不到"是正常的(L0 小窗口 + 多违例分散的场景),不 scope 会在扩窗前
误报 mismatch 把可修 case 判死。
**配套**:门内匹配也是消耗式一对一(与 #3 同语义),一条 baseline finding
不能同时满足两条 originals。

### D10. definitive 语义 = 最后实际搜索的窗口(#10)
**决策**:`lastSearchedDefinitive` 每轮赋值(不是 `|=` 累积);ExpansionCutoff
提前 break 时沿用 break 前那轮的值。
**为什么**:L0 完备只证明 L0 无解;L1 截断后仍宣称 definitive 是错误陈述,
上游会据此放弃本可通过加预算/扩窗找到的解。

### D11. 测试快照从 checker 派生,不手搓(批 1 落地时的教训)
**事件**:BaselineMismatch 门落地后,`engine_ignores_unrelated_halo_violation`
段错误——它手搓的 snapshot violation 缺 layer 字段,与 fake checker 在 baseline
里产生的同一违例签名不匹配 → 门(正确地)拒绝。
**决策**:E2E 测试的 snapshot 一律用 checker 生成(窄 guard 圈出目标违例),
与生产一致(生产里 snapshot 本来就出自 checker)。手搓 Violation 只用于
gate/signature 的单元测试(配 ScriptedChecker)。
**教训**:凡是"engine 输入应来自 checker"的数据,测试也让它来自 checker,
否则测试锁的是假契约。ASan 抓到了这个段错误——合入前跑 ASan 值得。

### D12. 输出 all-or-nothing(**用户未拍板的开放决策**)
现状:修不干净 → `hasSolution=false` + BestOverlay 诊断,不返回 partial。
用户问过"修不掉的 violation 能否一起返回",给过三选项:A 维持 / B 显式
partial 模式 / C 结构化残留违例字段。**我推荐 C,用户尚未回复。动
`FillerRepairResult` 前必须先问。**

### D13. 12-master 库事实 → 三项搜索精化
库:`F_FILL{8,4,3,2}_63S6T9{R,L,UL}_1`,**每宽度 3 VT 齐全**(用户先给了不全
清单后更正;R/L/UL 含义按业界惯例推断为 RVT/LVT/ULVT,**待 library 团队确认**)。
推论:每 filler 恒 2 候选 → anchor-follow 恒可构造(排序实现,无种子机制)、
第三 VT 降权(降权不剔除,犄角场景仍可达)、3^k ≤ 预算时完备枚举("窗口内
无解"成为确定性结论)。算法不 hard-code 这张表,一切以 provider 运行时返回为准。

### D14. #9 落地:filler-domain 接口与枚举顺序语义(实现时决策)
**决策**:`rankFillers` 返回 `std::vector<FillerDomain>`——filler 级排序键只留
{direct, bridge, width, position},VT 选择特征(anchorVote/第三 VT 降级)下沉为
**domain 内排序**(anchor VT → 邻接 majority → 稳定 id,第三 VT 垫底);
`enumerateOverlays` 按 (size, filler 组合字典序, domain 笛卡尔积・末位变最快)
枚举,memberCap 数 filler。
**顺序语义变化(有意为之)**:size-1 从旧的"全体主选 → 全体第三 VT"变为
"逐 filler 展开完整 domain"(f1 的第三 VT 先于 f2 的主选)。这正是 spec §6.6
V2.1 文本的定义;代价是个别多解 case 的"首个 clean"换人——用户 5 行 grid 的
首解从 3013→vt1 变为 2012→vt0,**两者都是 oracle 验证的 delta-clean**,engine
契约是"钉死枚举序中的第一个 clean",测试已更新并注明。
**否决备选**:保留扁平 swap 列表、只把 cap 换算成 filler 数——不行,那仍无法
表达"入选 filler 带完整 domain",第三 VT 仍可能被前缀挤出。
**附带简化**:同 filler 冲突按构造不可能,枚举里的 dup 检查删除。

## 5. 实现要点与陷阱(接手前必读)

- **stub 陷阱**:`drc/ImplantLayerChecker.cpp` 的 `checkPlaceWithOverlay[s]`
  是占位,除协议 echo 外恒报 clean。engine 的 accept 是 delta 全干净 → 接上
  stub 第一个 candidate 就"通过"。真实现没到位前 engine 只接
  `fake/FakeImplantChecker`。
- **checker 需填 spec §5 新字段**:`makeViolations`/`scanViolations` 目前只填
  legacy 字段;`kind/relation/rowIds/participants` 缺失时 engine 会 rowId
  fallback 降精度(inter-row 违例可能误并)。这是对 checker 侧的输入契约要求。
- **单位**:planner 全 DBU、半开区间 `[xl,xh)`;`ipl::PlacedInst.columnId` 是
  site 单位。adapter 换算是头号 bug 温床。
- **fake checker 语义是简化**(run-based):用户 5 行 grid 在 MW=MS=1 下 fake
  报的违例与用户预期的不同——这不是 bug,是 oracle 边界;测试锁"对给定
  oracle 的确定性行为"。
- **debug print 契约**:`[fr][stage]` 因→果 + 数据变化,`FR_VERBOSE=1` 打开;
  别写废话。改代码时保持该风格(用户明确要求)。
- 测试基建:`test/run_tests.sh`(-Werror);ASan 命令见 TestPlan.md §5;
  名字过滤 `./run_tests.sh <substr>`。

## 6. 已知问题 / 风险(除三批未完项外)

1. **签名 xWindow 容差 vs 真 checker 抖动**:BaselineMismatch 门依赖签名匹配;
   若真 checker 跨 snapshot 的 xWindow 抖动 > 1 site,门可能误触发。集成时若见
   噪声,加 config(容差可调或门降级为 Error)——先观察再动,不预做。
2. **`rowLegalSpan` 单区间**表达不了 macro/blockage 多段 legal segment;#12
   上收 infra + 窗口局部复核会大幅缓解,真多段时由 adapter 报 issue。
3. **R/L/UL 含义未经 library 团队确认**(见 D13);影响仅注释/文档,不影响算法。
4. **#8 未做期间的已知次优**:L1 sweep 可吞整条 filler run → 完备枚举退化为
   截断枚举(spec §6.3 已写明目标行为,代码还是老实现,`Window.h` 头注释有
   显式 NOTE)。#9 落地后此项影响减半(截断时 cap 按 filler、domain 不丢),
   但"完备性丢失"本身仍在,#8 仍值得做。

## 7. 下一步(优先级序)

1. **#8 adaptive-L1**(批 2 收尾,最后一个算法项):每轮向 best 非-clean
   candidate 的 blocking violation 所在侧扩 K≈2 个 filler,重算完备性;取代 L1
   的边界 sweep。engine 的 `best` 已在追踪扩窗方向所需信息。
2. **测试拓展**(可并行,另一 AI 负责):按
   `src/fillerRepair/test/TestPlan.md` 执行(#9 已落地,P1 的
   SubsetSearch/Ranker 测试直接按 filler-domain 语义写)。
3. **TODO 12 对接**(等真 checker overlay 实现):adapter 层
   (`fillerRepair/adapter/`,可含 UDM)+ CMake + 用真 checker 重放 spec §10。
4. **#12 precheck 上收**:随 adapter 一起(infra 按 design revision 缓存
   full-utility;planner 留 O(window) 防御复核)。
5. **问用户拍板 D12 输出格式**,再动 `FillerRepairResult`。

## 8. 工作方式(硬约束)

- 分支:`claude/wizardly-carson-secahu`(fork RamboJHB/OpenROAD),只在此分支
  开发/commit/push。
- commit:小步单主题,`fillerRepair:` / `drc:` / `docs:` 前缀,message 讲清因果。
- 语义改动顺序:spec → 代码+测试 → HandOff/README 状态 → 本文决策日志。
- 每次改动 `./run_tests.sh` 必须全绿;合入前跑一次 ASan。
- fake 与既有测试不许删、不许改语义(测试暴露 bug 走 TestPlan §4 流程)。
- 修改 `drc/ImplantLayerChecker.{h,cpp}`/helper 时保留现有代码(删除 → 注释),
  新类型 additive 扩展。
