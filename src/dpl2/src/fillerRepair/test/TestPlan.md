# Test Plan — fillerRepair 子模块测试拓展

> 写给负责测试拓展的 AI。背景:真实 checker 尚未接入,等待期间把纯 planner 各
> 子模块(PreCheck / Signature / Window / SwapGenerator / Ranker / SubsetSearch /
> OracleGate / Engine 主循环)的测试覆盖做扎实。当前 56 个测试全绿
> (`test/run_tests.sh`,`-Wall -Wextra -Werror`)。
> **进度**:第一批拓展(commit `a1a0750`,15 个)已合入并通过 review——
> **P0 全部完成**;P1 完成 Signature 3 个与 SubsetSearch 3 个。
> 剩余:P1 的 PreCheck / Window / Ranker / SwapGenerator 各组,及 P2 全部。
> **V2.1 #9(filler-domain 枚举)已落地**:Ranker 返回 `FillerDomain`、
> SubsetSearch 枚举 filler 组合 × domain 赋值、cap 按 filler 数——本计划中
> Ranker/SubsetSearch 的测试一律按该语义写。
>
> 先读:`src/dpl2/AGENTS.md`(项目记忆与红线)→ spec §0/§6/§10
> (`docs/filler_vt_overlay_repair_spec.md`)→ 本文。

## 0. 总原则(违反任一条的测试不收)

1. **确定性**。任何测试同输入两次运行产出完全相同的结果。禁止依赖真实时间、
   随机数(除非固定 seed 并把 seed 写进测试名/注释)、map 未定序遍历。
2. **不测 fake 的 DRC 语义,测 planner 对 oracle 的行为**。fake checker 的
   run-based MW/MS 是刻意简化;断言"engine 对给定 oracle 结果做了正确的
   分类/搜索/协议处理",不断言"这个布局按真实 DRC 应该有什么违例"。
   gate 层的分支覆盖优先用 `ScriptedChecker`(按 overlay key 直接指定返回),
   不要费劲用 fake 规则去凑几何。
3. **测试暴露 bug 时不要顺手改 engine 语义**。先写下失败测试 + 现象,单独
   报告/修复(engine 改动要对照 spec;spec 是权威)。
4. **每个新布局都按项目格式注释**:
   `Vt Type: ... | Widths: {...} | cell type: 1=std cell, 0=filler | Format: (vt type, width, cell type)`。
5. **回归测试的验收标准**:针对某个已修 bug 的测试,必须"把那个修复 revert
   掉就失败"。写完自查一次(临时 revert 或推演)。
6. 保持现有 harness 结构:单个 `test_main.cpp` 注册表 + 名字过滤 +
   `FR_VERBOSE=1`。文件超过 ~2500 行后可拆分为 `test_<module>.cpp`
   (拆分时保持单二进制、单注册表,run_tests.sh 一并更新)。
7. 定期跑 ASan 版本(见 §5 命令);新测试合入前至少跑一次。

## 1. 现有覆盖(41 个,勿重复)

- Swap 构造/校验、canonicalKey、wire 转换(3)
- PreCheck:全覆盖 OK、gap、overlap/offgrid/illegal、engine fatal 短路(4)
- fake checker 协议与规则:echo/order、invalid 隔离、intra/inter 检测、
  guard 过滤、target override(6)
- Signature:normalize、匹配、relatedness(3)
- Window:L0、L1 扩到 fixed 边界、guard 两圈 ring(3)
- 生成器:basic、no usable master(2);Ranker 顺序(filler-domain)(1);
  枚举顺序/完备性(1);#9 回归 filler cap 不挤出(1)
- Engine E2E:单 swap、非单调 pair、unrelated halo、definitive 无解、
  unfixable hint、空 snapshot、协议错误、批序无关、用户 5 行 grid(9)
- Gate:cache 单次评估、delta 分类分支(2)
- V2.1 批 1 回归:unexplained illegal、baseline mismatch、multiset、
  per-violation ruleDistance、definitive-last-window(5)
- 杂项(candidate provider 等)(1)

## 2. 待补测试(按优先级;名字用建议的 case 名)

### P0 — 正确性关键路径 ✅ 全部完成(a1a0750)

**OracleGate / baseline 一致性门**
- `gate_baseline_unexpected_inwindow_aborts`:baseline 里出现 originals 之外的
  **窗口内** violation → `BaselineMismatch` fatal(现在只测了"original 缺失"
  方向,门的 (b) 分支没测)。
- `gate_baseline_halo_extra_allowed`:baseline 里 originals 之外的违例在
  **窗口外**(halo)→ 门放行(这是 spec 6.8 规则 5 的允许项)。
- `gate_baseline_outside_guard_original_skipped`:某条 original 在本窗口 guard
  之外 → 门不因它缺席而 fail(扩窗场景的保护;这是实现时的关键 scoping
  决策,见 AGENTS.md 决策日志)。
- `gate_residual_one_to_one`:两条同 signature originals、candidate 只剩一条
  同 signature finding → `residualOriginals == 1`(multiset 语义:一条已修、
  一条残留),而不是 0 或 2。

**协议边界(用 Misbehaving 变体)**
- `gate_batch_extra_result_rejected`:返回条数 > 请求条数 → protocol error。
- `gate_single_wrong_echo_on_baseline`:baseline 单发接口 echo 错 id →
  abort(现在只测了批量路径)。
- `gate_status_not_checked_carries_on`:批内一个 request 返回
  `CheckerError`/`Unsupported` → 该 candidate 不入选,同批其他正常评估,
  搜索继续(隔离语义在 gate 层的体现)。

**classify 分支补漏**
- `gate_fatal_diag_makes_unusable`:`status==Checked` 但 diagnostics 里带
  `Severity::Fatal` → usable=false → 拒绝。
- `gate_new_violation_no_rows_goes_halo`:新违例 `rowIds` 为空 → 不算
  in-window,走 halo 相关性判定(relatedness 的空 rows 保守路径)。

### P1 — 子模块单元补强

**Signature**(✅ a1a0750 完成前四项,nullopt 分支并入 field_mismatch)
- ~~`signature_field_mismatch_each`~~ ✅
- ~~`signature_xwindow_tolerance_edges`~~ ✅
- ~~`relatedness_row_and_distance_edges`~~ ✅
- `rule_distance_fallback`:violations 全零 requiredValue → 退回 siteWidth。
  (仍待做)

**PreCheck**
- `precheck_multi_row_issues_deterministic`:多行多 issue,顺序与两次运行
  一致性。
- `precheck_gap_at_row_edges`:行首 gap、行尾 gap 的 xLo/xHi/siteCount。
- `precheck_overlap_three_instances`:三个 instance 叠一段,issue 的
  instances 列表内容。

**Window**
- `window_L0_exact_membership`:构造有"参与者 filler、anchor 相邻 filler、
  bridge filler、以及都不是的 filler"的布局,断言 editable 集合**恰好**是
  前三类(第四类不进)。
- `window_bridge_conditions_each`:三种 bridge 条件各自独立触发(左贴、
  右贴、上下行与加宽 anchor span 重叠)。
- `window_at_design_edges`:anchor 在 row 0 / 顶行、x 在行首尾时 rows±1、
  guard rows±2、ring 的 clamp 行为。
- `unfixable_ring_boundary`:filler 恰在 ring 内第 2 个 instance(true)/
  第 3 个(false);行方向 ±2(true)/±3(false)。

**SubsetSearch** ✅ 全部完成(a1a0750:complete/budget 边界、overflow clamp、
size-3 cap 与笛卡尔积顺序)。

**Ranker**(#9 已落地:filler 级键 + domain 内序分开测)
- `ranker_filler_key_isolated`:filler 级排序键逐个验证(direct、bridge、
  width、position)。
- `ranker_domain_order_isolated`:domain 内序逐个验证(anchor VT 第一、
  majority 第二、第三 VT 垫底但不剔除)。
- `ranker_majority_per_band`:构造上下行 band 多数与同行多数不同的布局,
  验证按 band 计数。

**SwapGenerator**
- `swapgen_rejected_candidate_diag`:provider 返回一个通不过 makeSwap 校验的
  master(如尺寸不符)→ `RejectedCandidate` 诊断 + 跳过,不 abort。

### P2 — 不变量/健壮性(高价值,建议做)

- `engine_batch_size_invariance`:同一 case 在 `batchSize=1` 与 `=32` 下,
  `hasSolution`/`changes` 完全一致(枚举序早停语义不受批大小影响)。
- `engine_determinism_full_transcript`:同一 request 跑两遍,diagnostics 全文
  与 requestCount 逐项相等(现有个别 case 有弱化版,做一个严格版)。
- `engine_never_edits_guard_only`:所有发出的 `fillerChanges` 的 instanceId
  必须 ∈ 当时窗口 editableFillers(用包装 checker 记录每个 request 断言)。
- `engine_budget_ceiling`:任何 case 下 requestCount ≤ 窗口数 × 预算。
- `engine_unfixable_hint_but_solved`:ring 内无 filler、但 L1 能拉到可修
  filler 且 oracle 判 clean → **hint Warning 存在且 hasSolution=true**
  (V2.1 #6 降级的正向收益,现在没有测)。
- (可选)固定 seed 的随机小布局 smoke:生成 5-10 个随机 3 行布局,断言
  上述不变量(不断言具体解);seed 固定写死。

## 3. 明确不做

- 不给 stub 的 `ImplantLayerChecker::checkPlaceWithOverlay` 写行为测试
  (它会被真实现替换;checker 实现不是本模块责任)。
- 不写依赖 fake 规则模型细节的"伪 DRC 正确性"断言(见总原则 2)。
- 不为 #8 adaptive-L1 预写行为测试(接口未定,写了也是猜;等重构落地后按
  spec §6.3 补。#9 已落地,不受此限)。

## 4. 工作流

1. 每加 3-5 个测试跑一次全量 `./run_tests.sh`;合入前跑一次 ASan(§5)。
2. 测试名进注册表,按现有分区顺序插入(模块内聚)。
3. commit 风格:`fillerRepair: tests — <一句话>`;一批一个主题。
4. 发现 engine bug:写失败测试(先注释掉注册或标注 EXPECTED-FAIL 说明),
   在 commit message 和 AGENTS.md「已知问题」里记录,不擅自改语义。

## 5. 命令

```bash
cd src/dpl2/src/fillerRepair/test
./run_tests.sh                     # 全量,-Werror
./run_tests.sh <name-substr>       # 过滤
FR_VERBOSE=1 ./run_tests.sh <case> # 带 [fr] 决策链日志

# ASan(内存错误;本项目曾靠它抓到一个测试布局引发的段错误)
cd src/dpl2/src/fillerRepair
g++ -std=c++17 -g -fsanitize=address -O0 \
  Swap.cpp PreCheck.cpp Signature.cpp Window.cpp SwapGenerator.cpp \
  Ranker.cpp SubsetSearch.cpp OracleGate.cpp FillerRepairEngine.cpp \
  fake/FakeImplantChecker.cpp test/test_main.cpp -o /tmp/fr_asan && /tmp/fr_asan
```
