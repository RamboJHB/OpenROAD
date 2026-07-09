# HandOff — Filler VT Overlay Repair(交接文档)

> 写给下一个 agent。本文档由本轮的决策者/reviewer 编写:在拿到真实 checker
> 实现(`src/dpl2/src/drc/ImplantLayerChecker.{h,cpp}` + `ImplantLayerCheckerHelper.cpp`)
> 之后,对已有工作做了一次全面 review,并把"接下来要做什么、怎么做、什么不能碰"
> 固定在这里。**先读 spec,再读本文,然后按 §4 的顺序开工。**

权威文档:`docs/filler_vt_overlay_repair_spec.md`(V2,最终版)。
本文与 spec 冲突时以 spec 为准;本文补充的是 spec 写作时还不知道的
checker 实现细节。

---

## 1. 项目一句话

设计 100% utility(所有 site 被 std cell / filler 覆盖);opto 一次只改
**一个** std cell 的 VT;周围 filler 还是旧 implant 类型,产生 implant
MW/MS violation。我们(dpl2 内的 repair engine)拿到 checker 的 violation
快照,**只通过同宽同高同位置的 filler master 换型(swap)** 找到一组
`FillerChange`,交回 checker 用 overlay 方式验证,干净则由 infrastructure
commit。engine 永远不碰 DB、永远不自己判 DRC(checker-as-oracle)。

阶段词汇:**本阶段只有 swap;下一阶段才是 rewrite(merge/split)。**
代码里不存在也不允许出现 Move / FillerRewrite 抽象。

## 2. 现状盘点(截至 commit `b7b72ab`,分支 `claude/wizardly-carson-secahu`)

### 2.1 已完成

| 部分 | 位置 | 状态 |
|---|---|---|
| Spec V2(算法、接口、协议、测试集全部钉死) | `docs/filler_vt_overlay_repair_spec.md` | 完成 |
| V1 spec/addendum(打了版本标签存档) | `docs/*_v1.md` | 存档 |
| 纯 planner engine(spec §11 TODO 1–11) | `src/dpl2/src/fillerRepair/` | 完成,35 个确定性测试全绿 |
| UDM-free 基础类型 + XInterval | `src/dpl2/src/drc/ImplantBaseTypes.h` | 完成(checker 侧持有,planner alias) |
| 真实 checker 源码导入 + spec §5 接口脚手架 | `src/dpl2/src/drc/ImplantLayerChecker.{h,cpp}`、`ImplantLayerCheckerHelper.cpp` | 类型/声明完成;overlay API 是 **stub** |
| fake checker / fake design / fake candidate provider | `src/dpl2/src/fillerRepair/fake/` | 完成,继续用于单元测试 |
| 测试 harness(35 case,含用户 5 行 grid 场景) | `src/dpl2/src/fillerRepair/test/` | 全绿 |

engine 五层管线(spec §3/§6)已全部实现并被测试覆盖:
①SwapGenerator ②Ranker(第三 VT 降级→直接参与者→bridge→邻居多数票→宽度→位置)
③SubsetSearcher(完全空间判定 + N_s 截断) ④OracleGate(baseline-delta 唯一
判定标准,含 signature 匹配、halo 相关性分类、协议校验、cache)
⑤Result/Diag,外面套 L0→L1→L2 窗口升级循环 + ExpansionCutoff。

### 2.2 未完成(= 你的工作)

spec §11 **TODO 12**:真实 checker/infra 对接 + CMake 集成。具体拆解见 §4。

## 3. Reviewer 结论:拿到真实 checker 后发现了什么

以下每条都有 file:line 依据,是本轮 review 的核心产出。

### 3.1 【最重要】overlay API 目前是 stub,**永远报 clean,绝不能接给 engine**

`ImplantLayerChecker.cpp:3273`(`checkPlaceWithOverlay`)/ `:3302`(批量版)
是占位实现:除了 requestId echo、duplicate-filler → `InvalidOverlay` 之外,
任何合法 overlay 都返回 `status=Checked, isLegal=true`,并带
`{"Stub", "checkPlaceWithOverlay stub: no real DRC performed"}` 诊断。

engine 的 accept 标准是"baseline 与 candidate 的 delta 全干净",stub 会让
**第一个 candidate 直接通过**。所以在真实实现完成前,engine 只能继续用
`fake/FakeImplantChecker`。对接顺序必须是:先做真 overlay(§4 步骤 1),
再写 adapter(§4 步骤 3),最后才把 engine 指向真 checker。

### 3.2 真实 overlay 实现的推荐路线(复用 checkDirect 机制)

checker 里已有两条检查路径:

- **fast path** `checkPlace` (`ImplantLayerChecker.cpp:1379`):
  site 对齐校验 → `overlapInfo`(removableFillers / blockingOverlap)→
  `excludedInstances = removableFillers + target` → candidate
  `instantiate(..., /*isCandidate=*/true)` + `slotPolarityOk` +
  `mergeShapes(targetIntervals, true)` → 逐 rule `evalRule(rule, targets,
  mode, excludedInstances)` → `makeViolations`。target-local,只查目标
  相关 shape,快。
- **slow path** `checkDirect` (`:1500`):从 committed instances 重建完整
  scan snapshot(`scanSnapshot(request, excludedInstances)`)→ `scanShapes`
  → 逐 rule `scanRule` → `scanViolations`。全设计扫描,慢但机制通用。

**overlay 的本质** = checkDirect 的推广:excludedInstances 里除了 target
和 removableFillers,再加上**每个被 swap 的 filler**;snapshot 里以
candidate 方式注入 target 的新 pose **和每个 filler 的新 master**
(`instantiate(fillerId, newMasterId, 原 rowId, 原 x, 原 orientation, true)`
—— swap 定义保证 row/x/orientation 不变,只有 master 换)。这样
`scanSnapshot` → `scanRule` → `scanViolations` 全部原样复用。

**必须做的两个收敛**(否则预算内跑不动、结果也不符合 spec §5):

1. **guardRegion 限扫**:checkDirect 是全设计扫描;engine 每个 window 预算
   512 次 checker 调用(spec §7),全扫 512 次不可接受。snapshot 重建时只
   收集 guardRegion 外扩 `queryRadius(rule)`(`:3178`)范围内的 instance;
   guardRegion 是 `OverlayCheckRequest` 里的 `CheckerRect`。
2. **结果裁剪**:返回的 `violations` 只保留与 guardRegion 相交的(spec §5:
   checker 只对 guardRegion 内负责)。engine 的 delta 分类依赖这一点。

批量版 `checkPlaceWithOverlays`:保持"逐个独立、按输入顺序"语义即可
(engine 的协议测试要求顺序无关、逐 request 隔离失败)。同一批次里
targetPlace/guardRegion 相同,snapshot 的 committed 部分可以整批复用,
只有 candidate 注入逐 overlay 不同 —— 这是最有价值的批内优化。

**协议红线**(engine 已按 spec §5.2 实现并有 Misbehaving/Reversing checker
测试):requestId 必须原样 echo;丢 id / 重复 id 会被 engine 判
CheckerProtocolError 直接 abort;非法 overlay(重复 filler、未知 instance、
master 尺寸不符)只标记该 request 为 `InvalidOverlay`,不影响同批其他 request。

### 3.3 makeViolations / scanViolations 不填 spec §5 新字段 —— 必须补

头文件里 `Violation` 已扩展(`ImplantLayerChecker.h:86-93`):
`kind / relation / rowIds / participants`。但两个构造点都只填 legacy 字段:

- `makeViolations` (`ImplantLayerChecker.cpp:1885`):填 ruleId/ruleSource/
  primaryLayer/secondaryLayer/measured/required/xWindow/relationship/
  instances/shapeIds/mergedShapeIds/targetInterval/neighborInterval/layerName。
- `scanViolations` (`:2454`):同上的 scan 版。

engine 的 signature(`fillerRepair/Signature.cpp:107`)按
`(ruleId, kind, relation, primaryLayer, secondaryLayer, rowIds)` 匹配,
rowIds 缺失时会 fallback 到 anchor row 并打 flag —— 能跑但匹配精度下降,
inter-row violation 会被误并。**填充是硬需求,不是锦上添花。**

映射方案(全部信息 checker 内部已有,不需要新数据):

| spec 字段 | 来源 |
|---|---|
| `kind` | `isWidthRule(ruleSource)` → `MinWidth`,否则 `MinSpacing` |
| `relation` | `Relationship::IntraInstance/IntraRow/InterRow` 一一对应到 `ViolationRelation` |
| `rowIds` | fast path:target/neighbor `MergedShape.rowId`(makeViolations 里两个 shape 都查得到);scan path:`ScanOutcome.instanceIds` → `instances_` 查 `PlacedInst.rowId`(或通过 `targetShapeId/neighborShapeId` 回查 `ScanShape.rowId`,需要把 shapes 传进 scanViolations)。排序去重 |
| `participants` | `violation.instances` 逐个查 `instances_`:masterId、rowId、xRange = `[columnId*siteWidth_, columnId*siteWidth_ + masterWidth)`、`isFiller = isFillerInstance(id)`、`isTarget = (id == request.instanceId)`。**注意 `PlacedInst.columnId` 是 site 单位**(见 §3.4) |

overlay 路径里被 swap 的 filler 是 candidate,不在 `instances_` 的新 master
下 —— participants 里报它时 masterId 用 **overlay 后的新 master**(engine
靠 instanceId 匹配,masterId 仅参考,但别报错的)。

### 3.4 类型二元性是**设计决定**,不要"统一",要 adapter

`ipl::CheckRequest` 内嵌 `eUTL::PhysOrientation`(UDM 类型),永远进不了
UDM-free 的 planner。所以 `fillerRepair::Types.h` 持有一套结构同构的 wire
类型是**故意的**,不是重复代码。对接方式是薄 adapter,单向换皮:

| planner (`fillerRepair::`) | checker (`ipl::`) | 转换 |
|---|---|---|
| `Orient` (R0/R180/MX/MY) | `eUTL::PhysOrientation` | 枚举映射表,adapter 里写死 |
| `x`(DBU) | `PlacedInst.columnId`(**site 单位**) | `x = columnId * siteWidth_`;反向除。**这是最容易埋 bug 的地方,adapter 单测必须覆盖** |
| `Region{XInterval x; rowLo, rowHi}`(row-based) | `CheckerRect`(y-based) | `y = rowId * rowHeight`(rowHeight 来自 helper 的 rows 构建);半开区间约定两侧一致(DBU 半开) |
| `fillerRepair::Violation` 等 wire 类型 | `ipl::Violation` 等 | 字段一一抄,基础 id 类型本来就 alias 自 `ImplantBaseTypes.h`,无损 |

adapter 建议放 `src/dpl2/src/fillerRepair/adapter/`(可含 UDM 头,planner
本体目录保持 UDM-free)。三个抽象接口各配一个实现:

1. `ImplantOverlayChecker` → 包一层 `ImplantLayerChecker::checkPlaceWithOverlay(s)`;
2. `PlacementView` → 基于 `ImplantInput` / checker 的 `instances_`、masters、
   rows(注意 `instancesInRow` 必须按 x 排序返回,engine 依赖有序性);
3. `FillerMasterCandidateProvider` → 同宽同高、`isCoreFiller`、VT 不同的
   master 集合。VT 从 master 名解析:helper 的 `parseLayerName`
   已能拆 "VTUL_N" → 家族(R/L/UL)+ 极性;12-master 库(spec 附录 A)
   每个宽度 {2,3,4,8} 三种 VT 全齐,provider 恒返回 2 个候选。

`siteWidth` 取 `ImplantLayerChecker::siteWidth()` (`:2649`)。

### 3.5 其他 review 发现(小,但要知道)

- `commitPlace` (`:2539`) 是 infrastructure 的 commit 通道:删 removable
  fillers → 重放 target。**将来 commit 我们的 FillerChange 也走类似机制**,
  但那是 checker/infra 的活,engine 只返回 changes,不要越界。
- `checkPlace` 有 `isSameCommittedPose` 短路(`:1458`)。overlay 场景 target
  pose 恒定、只有 fillerChanges 变,同样的思路可用于批内复用(§3.2)。
- fake checker 的规则模型(run-based MW/MS)与真实 checker(LEF 规则 +
  merged shape + containment suppression + P/N band)行为不同,grid case
  已经暴露过这种差异 —— **这正是 checker-as-oracle 边界的意义**:engine
  测试锁的是"对给定 oracle 的确定性行为",不是 DRC 语义。换真 checker 后
  单元测试(fake)不许动,另加集成测试(§4 步骤 5)。
- 未决的用户决定:**输出格式**。当前是 all-or-nothing(修不干净就
  `hasSolution=false` + BestOverlay 诊断)。用户问过"能否把修不掉的
  violation 一起返回",给过 A(维持)/B(部分修复模式)/C(结构化残留
  violation 字段)三个选项,推荐 C,**用户尚未拍板**。不要擅自实现,先问。

## 4. 下一个 agent 的任务清单(按此顺序,做完一步跑一次测试再进下一步)

1. **checker:真实 `checkPlaceWithOverlay` / `checkPlaceWithOverlays`**
   (`ImplantLayerChecker.cpp:3273` 起)。按 §3.2 路线:checkDirect 机制 +
   swap filler 进 excludedInstances + 新 master candidate 注入 +
   guardRegion 限扫 + 结果裁剪 + 批内 snapshot 复用。替换 stub 时**注释掉
   而不是删除**(项目惯例:改这两个文件保留现有 code)。
2. **checker:`makeViolations` / `scanViolations` 填充
   `kind/relation/rowIds/participants`**,按 §3.3 的映射表。两条路径都要填,
   排序保持现有 determinism(现有 sort 在 `:1984` / `:2521`)。
3. **adapter 层**(`src/dpl2/src/fillerRepair/adapter/`):§3.4 的三个接口
   实现 + 类型转换,重点单测 columnId↔DBU 与 Region↔CheckerRect。
4. **CMake 集成**(spec TODO 12):fillerRepair 目录进 dpl2 构建;
   `test/run_tests.sh` 的 g++ 直编测试保留(它不依赖 UDM,是最快的回归)。
5. **集成测试**:真 checker + adapter 重放 spec §10 场景(至少:单 swap 可解、
   非单调 pair、unrelated-halo 不否决、definitive no-solution、协议错误
   abort、用户 5 行 grid)。
6. **输出格式**:先向用户要 A/B/C 决定,再动 `FillerRepairResult`。

## 5. 红线(违反任何一条 = 返工)

1. **只做 swap。** 不引入 Move / FillerRewrite / merge / split;那是下一阶段
   (rewrite)的词汇,spec §4.3 已经把口留好了。
2. **checker-as-oracle。** engine 永远不判 DRC 合法性;所有"干净与否"必须
   来自 baseline-delta 判定(spec §6.7),不要在 engine 里加几何规则。
3. **engine 是纯 planner、确定性。** 相同输入 → 相同 changes/诊断/调用次数。
   不碰 DB,不含 UDM 头(UDM 只允许出现在 adapter/ 和 drc/ 侧)。
4. **fake 与 35 个测试不许删不许改语义。** 每次改动跑
   `src/dpl2/src/fillerRepair/test/run_tests.sh`,必须全绿(`-Wall -Wextra
   -Werror`)。
5. **改 `ImplantLayerChecker.{h,cpp}` / helper 时保留现有 code**,要删的
   注释掉;新类型走 additive 扩展(现有做法见头文件里的 EVOLVED 注释)。
6. **debug print 规范**:清晰的 因→果 逻辑链 + data change,别废话。参考
   engine 现有 `Log.h` 用法(stage 标签 + cat() 拼接,FR_VERBOSE=1 打开)。
7. **stub 未替换前不得把 engine 接到真 checker**(§3.1)。

## 6. 构建、测试、工作方式

```bash
# 单元测试(纯 planner + fake,不需要 UDM/CMake)
cd src/dpl2/src/fillerRepair/test
./run_tests.sh                 # 35 cases,应输出 "OK: 35 test(s) passed"
./run_tests.sh <name-substr>   # 按名字过滤
FR_VERBOSE=1 ./run_tests.sh <case>  # 完整逻辑链日志
```

- 分支:`claude/wizardly-carson-secahu`(fork RamboJHB/OpenROAD),只在此
  分支开发、commit、push(`git push -u origin claude/wizardly-carson-secahu`)。
- 开发范围:**只在 `src/dpl2/` 内**(fillerRepair/ + drc/ 两处)+ `docs/` spec。
- commit 风格:小步、单主题、`fillerRepair:` / `drc:` 前缀,message 说清
  因果(参考 `git log --oneline` 里的现有历史)。
- 读代码顺序建议:spec §5/§6 → `fillerRepair/README.md` →
  `FillerRepairEngine.cpp`(主循环)→ `OracleGate.cpp`(判定核心)→
  checker 的 `checkPlace`/`checkDirect`。

## 7. 关键文件速查

| 文件 | 作用 |
|---|---|
| `docs/filler_vt_overlay_repair_spec.md` | 权威 spec(V2) |
| `src/dpl2/src/drc/ImplantBaseTypes.h` | UDM-free 基础 id + XInterval(双方共享) |
| `src/dpl2/src/drc/ImplantLayerChecker.h` | checker 接口 + spec §5 类型(EVOLVED) |
| `src/dpl2/src/drc/ImplantLayerChecker.cpp` | 真 checker;overlay stub 在 `:3273` |
| `src/dpl2/src/drc/ImplantLayerCheckerHelper.cpp` | UDM 数据抽取(columnId 为 site 单位的出处) |
| `src/dpl2/src/fillerRepair/Types.h` | planner wire 类型(UDM-free) |
| `src/dpl2/src/fillerRepair/FillerRepairEngine.{h,cpp}` | 主管线 + RepairConfig |
| `src/dpl2/src/fillerRepair/OracleGate.{h,cpp}` | baseline-delta 判定 + 协议校验 + cache |
| `src/dpl2/src/fillerRepair/Signature.{h,cpp}` | violation 签名/相关性(依赖 §3.3 的字段填充) |
| `src/dpl2/src/fillerRepair/fake/` | 单元测试用假件(保留) |
| `src/dpl2/src/fillerRepair/test/` | 35-case harness + run_tests.sh |
