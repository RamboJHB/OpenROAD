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

## 2. 现状(2026-07-13,分支 `claude/wizardly-carson-secahu`)

| 部分 | 状态 |
|---|---|
| Spec | **V2.1 定稿**(§0 修订记录 = 12 项 review 裁定);V1 存档已删(冗余) |
| planner(`src/fillerRepair/`) | TODO 1–11 + V2.1 engine 修订全部实现;批 1、批 2(#6/#7/#8/#11)、批 3 #9 全落地;仅 #12 adapter/precheck 上收未做 |
| 测试 | planner 79 个,`-Werror` + ASan 全绿;真实 checker core 9 个(`src/drc/test/run_tests.sh`,含 raw-vs-blocking 契约测试),`-Werror` + ASan 全绿;fake-UDM provider 4 个 |
| checker(`src/drc/`) | overlay API 真实现;本轮不改 checker 源码,只扩 standalone harness:类型共存、invalid batch 隔离、row/hash/guard 已覆盖。wire 仍是顺序关联、无 status、batch=单 target+N 变更。**真实 UDM extraction/adapter 前 engine 仍只接 fake** |
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
基础 id + XInterval 由 `fillerRepair/BaseTypes.h` 自持;checker 保留独立 `ipl::`
类型,adapter 显式转换。checker harness 在同一 TU include 两边防止 ODR 回归。
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

### D15. checker↔engine"互相依赖"的消解(spec §3.3)
**问题**:checker 调 engine 生成 solution,engine 调 checker 验证 overlay。
**裁定**:依赖倒置,engine 是底层,编译依赖恒为 checker→engine 单向:engine
只认自己的抽象 `ImplantOverlayChecker`(CheckerApi.h)+ 自有 UDM-free wire
类型;checker 的 repair 入口构造 engine 并把自己包进 `CheckerOracleAdapter`
注入(adapter 兼做类型换皮,checker 侧持有)。运行时无递归的保障是协议红线:
overlay API 是纯查询(const),**禁止内部触发 repair**;repair 入口加不可重入
assert。构建:libfillerRepair 零依赖 ← checker;adapter 随 checker 目标。
**否决备选**:并入 checker(毁独立测试)、双向抽象(空转)、std::function
(类型面弱)、orchestrator 拥有两者(最干净但当前集成事实是 checker 驱动;
engine 对谁驱动不敏感,future option)、再拆共享库(两侧已有各自基础类型,无必要)。

### D16. Fake-UDM candidate provider(adapter 预演,`fake/FakeUdmCandidateProvider.*`)
**目的**:真 provider 将坐在 checker 的 master 表上(buildMasters 从 UDM 构建),
先把那条数据通路 UDM-free 地预演一遍,给 adapter 定型。
**关键忠实点**(参考 `ImplantLayerChecker.cpp buildMasters` + helper):VT =
master 的 implant shape 所在 layer 的 **family**(`parseLayerName`:按最后一个
'_' 拆,VTS/VTL/VTH/VTUL + N/P),**绝不解析 master 名字**;单 master 混族 →
unusable(`master_implant_family_mismatch`);宽度 DBU 必须 site 对齐;shape 必须
铺满 master 宽;每行两个 half-row band shape(极性不影响 VT 推导)。
**VtId 映射钉死**:family 枚举序 VTS=0/VTL=1/VTH=2/VTUL=3,推导失败 kUnknownVt。
**API**:`describeMasters(ids)`(逐 id 宽度/VT/usable/reason,输入序保持)+
engine 的 `getUsableMasterCandidates`(同宽高、usable、filler、非当前,id 升序)
+ `registerInto(FakeDesign)`(保证 view 与 provider 的 master 数据一致——engine
构造 Swap 时会对 view.masterInfo 校验)。附录 A 12-master 库内置
(`addAppendixALibrary`,R→VTS/L→VTL/UL→VTUL 为待 library 确认的假定映射)。
**教训(测试)**:E2E 场景要么把解做成唯一(用规则约束排除掉其他 clean),
要么按 D14 锁"枚举序第一个 clean"并注明——第一版场景里 602→VTS 也是合法解,
被 engine 先找到,不是 bug。

### D17. 2026-07-12 checker 更新版的审查结论(commit 见 git log)
**事实**:checker RD 交付新版 `ImplantLayerChecker.{h,cpp}`,helper 删除、类型
内联(`Dbu` 替代 `DbCoord`、新增 `ColId`,CheckRequest/PlacedInst 改用 site 单位
colId)、接入 dpl2 legalizer(DRCChecker/Grid)、**overlay API 真实现**:
`checkPlaceWithOverlays(request, guard, vector<vector<FillerChange>>)`,batch 内部
先算一次 baseline(旧 filler),逐候选 validate(dup/非 filler/尺寸不符 → 诊断 +
isLegal=false,per-candidate 隔离)、全设计快照 + guard 内规则评估限定 + guard
裁剪,`Violation` 带签名 hash(= 我们 §6.2 的键,好消息)。结构上正是 HandOff
§3.2 推荐的路线。**按原样合入,未改他们一行代码。**
**两条契约项(engine 对接的 blocker,已在 checker 侧改好——见
`drc/CHECKER_REPAIR_CONTRACT.md`,均带 `[fillerRepair-fix]` 标记;待 RD review)**:
(1) blocking 过滤(touchesInstance ∪ 非 old)会吞掉"未修好但不触及 target"的
original——§1.2 bridge-MW 类必踩;xWindow 包含判 old 还会隐藏"缩小未消除"。
**改法**:新增 `checkPlaceWithOverlaysRaw`——复用 `checkOverlayRegion` 返回 guard
内全量违例、不过滤,delta 归 engine;原 blocking 形态保留给 legalizer。
(2) `Violation.rowIds` 从未填充。**改法**:`scanRule`×2 + `scanViolations` +
`makeViolations` 补填。附带修 `validateOverlayRequest` 头/实现不一致(编译 bug)。
我改的是 RD 的文件,只保住两条语义,形态他们可换。
**连带决策(2026-07-13 已落实前置)**:planner 基础类型已迁到
`fillerRepair/BaseTypes.h`,不再 include/alias `ipl`;adapter 可在两套独立类型间
显式转换,checker harness 的共存编译测试已通过。`drc/ImplantBaseTypes.h` 已无代码
引用,待 checker RD/CMake 集成确认后删除。requestId 取消 → 关联按顺序,engine 协议校验届时改
size+order,adapter 按 index 合成 id。participants 缺失 → adapter 从
violation.instances + placedInsts 合成。

### D18. 真实 checker standalone harness 与 guard 扫描修复(2026-07-12)
**测试边界**:`src/drc/test/run_tests.sh` 用 test-only fake UDM/mini-gtest,但直接
编译生产 `ImplantLayerChecker.cpp`;`DPL2_FAKE_UDM` 只关闭 Session 自动初始化与
缺失 helper 的 extraction,测试显式注入 `ImplantInput`。因此规则/index/overlay/
violation 路径是真 checker,不是 FakeImplantChecker。dense 8×200 的 MW/MS ×
intra/inter-row 4 例 + raw/baseline/rowIds、类型共存、invalid batch、row/hash/guard
共 8 例在 `-Werror` 与 ASan 下全绿。

**编译漂移**:首次直编译暴露 `evaluate/evalRule`、`findNeighbors` 参数、
`ScanOutcome.instances/instanceIds`、`RowInput/RowId` 四组头实现不一致,已按 cpp
实现与真实容器类型对齐。

**行为根因**:guard committed rect 曾全部标成 candidate,随后 neighbors 又跳过
candidate,所以 inter-row/spacing false clean;反向仅保留 committed 又会漏掉
换型后残留在 committed shape 上的新违例。另一个 bug 是 candidate/context 同层
接触 interval 被强制分段,制造 false MW/MS。裁定为 guard-wide scan:context 只按
guard 裁剪,所有 shape 可作 target/neighbor,同层接触几何跨 provenance 合并,
`containsCandidate` 仅保留为 OR 元数据;双向 pair 扫描按签名/窗口/参与者去重;
末端 blocking/raw 各自按契约输出。

**仍未完成**:真实 UDM extraction(原 helper 已删但等价实现未交付)、完整 dpl2
CMake/依赖闭包、checker↔engine adapter(TODO 12)。用户提供的 DePlace/network
文件已归位,但它们的完整构建仍依赖其余 infrastructure/UDM 文件。

### D19. #8 adaptive-L1 落地(2026-07-12)
**决策**:删掉 L1 一次 sweep 到 fixed/core boundary 的实现。L0 搜完未 clean 时,
OracleGate 的 best summary 携带实际 `blockingViolations`(residual originals +
in-window/related-halo new);Window 按它们相对当前 x 的位置确定左/右,在 blocking
rows 及 ±1 耦合行上每侧每行最多加入配置 K 个连续 filler(默认 2),遇 non-filler
立即停。每步重新 generate/rank/enumerate,所以 `plan.complete` 按新 domain 重算。

**截止**:下一步没有新增 editable filler → 截止;扩窗后的窗口完成完整枚举且
blocking violation multiset 按 pinned signature 与上一步相同 → 截止。后者只在
当前窗口 complete 时启用,截断窗口不能据此宣称更远 filler 无关。最终 definitive
仍只取最后实际搜索窗口(D10)。

**测试**:旧 `window_L1_extends_to_fixed_boundary` 被 K 限制测试替换;新增耦合行+
fixed boundary、adaptive 才能找到远端 filler 的 E2E、unchanged-blocking cutoff;
原 last-window definitive 回归改用 K=6 直接造截断 adaptive window。planner
总数 79,普通/ASan 全绿。

### D20. 2026-07-13 planner hardening 与测试扩展
**实现**:修复 SubsetSearch 在 `space == budget` 时已枚举完整却误标 truncated 的
边界;`plan.complete` 现在由完整空间与实际 emitted 数共同决定。PreCheck 改为按
coverage segment 扫描,多重 overlap 的 `CoverageIssue.instances` 带完整、排序后的
参与者,最终 issue 按 row/x 确定排序。planner runner 原生支持
`SANITIZE=address`。

**类型**:新增 `fillerRepair/BaseTypes.h`,消除 planner 对
`drc/ImplantBaseTypes.h` 的依赖。checker 生产源码未修改;仅在 checker test TU
验证两套 header 共存。

**测试**:planner 63→79,补完可由当前接口表达的 P1/P2 项;checker 5→8。普通与
ASan 全绿。未伪造 `ranker_majority_per_band`:当前 `MasterInfo` 只有单 `vt`,必须等
真实 adapter 提供 P/N band 元数据。bridge-MW 专用 checker fixture 与真实
UDM/adapter E2E 仍待完成。

### D20. 对 2026-07-12 批次(#8 + checker harness + 类型解耦)的 review(修 3 处)
**总评**:批次质量高——adaptive-L1 忠实 spec §6.3(终止性有保证:单调增长 +
双 cutoff)、fake-UDM 边界干净(`DPL2_FAKE_UDM` 只圈 ctor 与 initFromUDM)、
`BaseTypes.h` 类型解耦正是 D17 要求的方向、checker 的 scan 语义修正
(candidate 不再抑制 target/neighbor、同层接触合并)方向正确且有 8 测试佐证。
**review 修掉的三处**:
(1) **去重放错路径**:方向/band 重复折叠只加在 `makeViolations`(fast path,
不产生该类重复),真正产生重复的 `scanViolations` 没有 → raw API 一条物理违例
报两次。抽 `sortAndDedupViolations` 两处共用 + 比较器 hash/participants
tiebreak(重复必相邻)。
(2) **guard 边界截断幽灵违例**:新的精确-guard 快照裁剪把跨边界 run 切断,
断口上捏造 width 违例(测试窗两侧各 3 条)。修法:快照纳入按
guard + max(queryRadius) 外扩(纵向 +1 行),结果裁剪仍用精确 guard——
padded 边缘的断口离精确 guard ≥ 1 radius,必被结果裁剪丢弃。
(3) **缺 raw-vs-blocking 契约测试**:补 `BlockingHidesResidualButRawReports`
(bridge 场景:target 离开 F1 run,残留违例 participants 不含 target;
blocking 判 isLegal=true = false accept 实锤,raw 恰好报一条、fix 后干净)。
它同时用精确计数锁死 (1)(2)。
**附带**:删孤儿 `drc/ImplantBaseTypes.h`(D17 解耦后零引用);发现他们的
contract 文档声称"scanViolations 折叠重复"超前于代码——修复后文档为真。
**教训**:声称的行为要有精确计数的测试钉住(`.empty()`/presence 断言抓不住
重复和幽灵);guard 类空间裁剪永远要问"跨边界的几何被切断后会不会说谎"。

## 5. 实现要点与陷阱(接手前必读)

- **对接 blocker(D17/D18)**:checker core 已在 fake-UDM boundary 下直编译并
  8/8 + ASan 全绿(raw API + rowIds + guard scan 修复见
  `drc/CHECKER_REPAIR_CONTRACT.md`),但**真实 UDM extraction 与 adapter 完成**前,
  engine 仍只接 `fake/FakeImplantChecker`。对接时用 `checkPlaceWithOverlaysRaw`,不用
  blocking 形态的 `checkPlaceWithOverlay[s]`。
- **checker 字段契约(D17 后)**:新版 `Violation` 的 rowIds 与签名 hash 已填充;
  participants 不存在,adapter 从 `violation.instances` + `placedInsts` 合成;
  kind 从 ruleSource 推导,relation = relationship。
- **单位**:planner 全 DBU、半开区间 `[xl,xh)`;checker 的
  `PlacedInst.colId` / `CheckRequest.colId` 是 **site 单位**
  (`x = colId * siteWidth`)。adapter 换算是头号 bug 温床。
- **类型边界(D17/D20)**:planner 已自持 `BaseTypes.h`;checker `ipl::` 类型独立。
  adapter 必须显式做范围/单位转换,不可重新共享同命名空间结构。
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
4. **adaptive K 是启发式参数**:默认每相关行/侧 K=2;只影响扩窗速度与 checker
   call 分布,不改变 accept gate。若真设计的跨行耦合需要更大步长,通过 config
   调整,不在算法里 hard-code 工艺值。

## 7. 下一步(优先级序)

1. **TODO 12 对接**(checker overlay core 已可测,等真实 UDM extraction/RD确认):adapter 层
   (`fillerRepair/adapter/`,可含 UDM)+ CMake + 用真 checker 重放 spec §10。
2. **#12 precheck 上收**:随 adapter 一起(infra 按 design revision 缓存
   full-utility;planner 留 O(window) 防御复核)。
3. **补 per-band/bridge-MW/真实 UDM E2E 测试**:只在真实元数据/API 可用后做。
4. **问用户拍板 D12 输出格式**,再动 `FillerRepairResult`。

## 8. 工作方式(硬约束)

- 分支:`claude/wizardly-carson-secahu`(fork RamboJHB/OpenROAD),只在此分支
  开发/commit/push。
- commit:小步单主题,`fillerRepair:` / `drc:` / `docs:` 前缀,message 讲清因果。
- 语义改动顺序:spec → 代码+测试 → HandOff/README 状态 → 本文决策日志。
- 每次改动 `./run_tests.sh` 必须全绿;合入前跑一次 ASan。
- fake 与既有 79+8 测试不许删、不许改语义(测试暴露 bug 走 TestPlan §4 流程)。
- 修改 `drc/ImplantLayerChecker.{h,cpp}`/helper 时保留现有代码(删除 → 注释),
  新类型 additive 扩展。
