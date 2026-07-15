# Test Plan — fillerRepair 子模块测试拓展

> 写给负责测试拓展的 AI。真实 checker core 已有独立 harness(见下),但尚未通过
> adapter 接入 engine;等待对接期间继续把纯 planner 各
> 子模块(PlacementView coverage / Signature / Window / Swap generation / Ranker / SubsetSearch /
> OracleGate / Engine 主循环)的测试覆盖做扎实。当前 80 个 planner 测试全绿
> (`test/run_tests.sh`,`-Wall -Wextra -Werror`;ASan 同样全绿)。
> **进度(2026-07-13)**:P0、可由当前接口表达的 P1、P2 五项不变量均完成。
> P1 仅余 `ranker_majority_per_band`,它受 `MasterInfo` 只有单 `vt` 的数据模型
> 阻塞,必须等真实 adapter 提供 P/N band 元数据,不得用 master 名伪造。
> **V2.1 #9(filler-domain 枚举)已落地**:Ranker 返回 `FillerDomain`、
> SubsetSearch 枚举 filler 组合 × domain 赋值、cap 按 filler 数——本计划中
> Ranker/SubsetSearch 的测试一律按该语义写。
> **真实 checker 进度(2026-07-12)**:`src/dpl2/src/drc/test/run_tests.sh`
> 直接编译生产 `ImplantLayerChecker.cpp`,仅以 `DPL2_FAKE_UDM` 跳过自动 UDM
> extraction,4 个 dense overlay 用例(width/spacing × intra/inter-row)加
> raw/baseline/rowIds、planner/checker 类型共存、invalid batch 隔离、
> row/hash/guard 回归、64-candidate 混合批确定性,共 10 个全绿;
> `SANITIZE=address ./run_tests.sh` 也全绿。checker 与 engine 尚未做 TODO 12 adapter。
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

## 1. 现有覆盖(planner 80 个 + checker 10 个,勿重复)

- Swap 构造/校验、canonicalKey、wire 转换(3)
- PlacementView coverage:全覆盖 OK、gap、overlap/offgrid/illegal、engine fatal 短路、多行确定性、
  行首尾 gap、三重 overlap 完整 participants(7)
- fake checker 协议与规则:echo/order、invalid 隔离、intra/inter 检测、
  guard 过滤、target override(6)
- Signature:normalize、匹配、relatedness、rule-distance fallback(4)
- Window:L0 membership/精确集合、bridge 条件、设计边界、guard 两圈 ring(6;
  unfixable ring 单元测试已随 hint 移除,2026-07-15 瘦身)
- adaptive-L1 #8:每步 K 限制/不 sweep 整段、blocking 侧方向、耦合行 ±1、
  fixed boundary、远端 filler E2E 解、完整枚举后 blocking 不变截止(3 个新增;
  旧 L1 boundary case 已替换)。
- 生成器:basic、no usable master、rejected candidate(3);Ranker 顺序、filler key、
  domain 内序(filler-domain)(3);
  枚举顺序/完备性(1);#9 回归 filler cap 不挤出(1)
- Engine E2E/不变量:原有 9 项 + batch-size 不变、全文确定性、guard-only 不编辑、
  预算上限、无 ring filler 仍可由 adaptive-L1 解;复杂 3 行/8-domain/桥接干扰场景验证
  高优先 VT pair 与 domain-tail 第三 VT pair 均在 21 次 checker call 内成功(16)
- Gate:cache 单次评估、delta 分类分支(2)
- V2.1 批 1 回归:unexplained illegal、baseline mismatch、multiset、
  per-violation ruleDistance、definitive-last-window(5)
- 杂项(candidate provider 等)(1)
- fake-UDM provider(adapter 预演):derivation、malformed、候选契约、
  engine E2E(4)
- 真实 `ImplantLayerChecker` core:8×200 dense layout 上 width/spacing ×
  intra/inter-row;每组覆盖 clean、original 残留、新 violation、无关 baseline
  过滤(4);raw API 保留无关 baseline 并携带 rowIds(1);planner/checker 类型同 TU
  共存、invalid batch 隔离、同 x 不同行 hash + guard 裁剪(3);64-candidate
  clean/residual/new/invalid 混合批重复执行后 violation 顺序/hash/诊断确定(1)。
  独立 harness,不计入上述 planner 80 个。

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
- `signature_field_mismatch_each` ✅
- `signature_xwindow_tolerance_edges` ✅
- `relatedness_row_and_distance_edges` ✅
- `rule_distance_fallback`:violations 全零 requiredValue → 退回 siteWidth。✅

**PlacementView coverage**
- `precheck_multi_row_issues_deterministic`:多行多 issue,顺序与两次运行
  一致性。✅
- `precheck_gap_at_row_edges`:行首 gap、行尾 gap 的 xLo/xHi/siteCount。✅
- `precheck_overlap_three_instances`:三个 instance 叠一段,issue 的
  instances 列表内容。✅

**Window**
- `window_L0_exact_membership`:构造有"参与者 filler、anchor 相邻 filler、
  bridge filler、以及都不是的 filler"的布局,断言 editable 集合**恰好**是
  前三类(第四类不进)。✅
- `window_bridge_conditions_each`:三种 bridge 条件各自独立触发(左贴、
  右贴、上下行与加宽 anchor span 重叠)。✅
- `window_at_design_edges`:anchor 在 row 0 / 顶行、x 在行首尾时 rows±1、
  guard rows±2、ring 的 clamp 行为。✅

**SubsetSearch** ✅ 全部完成:complete/budget 边界、overflow clamp、size-3 cap
与笛卡尔积顺序;2026-07-13 将 `space == budget` 的 contract 断言改为强制并修复
实现误标 truncated 的 bug。

**Ranker**(#9 已落地:filler 级键 + domain 内序分开测)
- `ranker_filler_key_isolated`:filler 级排序键逐个验证(direct、bridge、
  width、position)。✅
- `ranker_domain_order_isolated`:domain 内序逐个验证(anchor VT 第一、
  majority 第二、第三 VT 垫底但不剔除)。✅
- `ranker_majority_per_band`:构造上下行 band 多数与同行多数不同的布局,
  验证按 band 计数。**阻塞:当前 planner projection 没有 per-band VT 字段。**

**Swap generation (`Swap`)**
- `swapgen_rejected_candidate_diag`:provider 返回一个通不过 makeSwap 校验的
  master(如尺寸不符)→ `RejectedCandidate` 诊断 + 跳过,不 abort。✅

### P2 — 不变量/健壮性 ✅ 五项完成

- `engine_batch_size_invariance`:同一 case 在 `batchSize=1` 与 `=32` 下,
  `hasSolution`/`changes` 完全一致。✅
- `engine_determinism_full_transcript`:同一 request 跑两遍,diagnostics 全文
  与 requestCount 逐项相等。✅
- `engine_never_edits_guard_only`:所有发出的 `fillerChanges` 的 instanceId
  必须属于当时窗口 editableFillers。✅
- `engine_budget_ceiling`:任何 case 下 requestCount ≤ 窗口数 × 预算。✅
- `engine_adaptive_solves_beyond_ring`(原 engine_unfixable_hint_but_solved):
  violation 近旁无 filler、但 adaptive-L1 能拉到可修 filler 且 oracle 判 clean →
  hasSolution=true(hint 断言已随瘦身移除)。✅
- `engine_complex_ranked_pair_fast`:3 行、8 个 filler domain、两条跨行 violation
  与 direct/bridge/coupled decoy;首选 VT pair 在 21 次调用内成功,反转 snapshot
  顺序后解、调用数和 batch 数不变。✅
- `engine_complex_third_vt_still_succeeds`:同一 127-candidate 搜索空间要求 domain-tail
  第三 VT pair,仍在 21 次调用内成功,验证排序降级不会剪枝。✅
- (可选)固定 seed 的随机小布局 smoke:生成 5-10 个随机 3 行布局,断言
  上述不变量(不断言具体解);seed 固定写死。

### P0 — 真实 checker 后续覆盖

- `checkPlaceWithOverlaysRaw` bridge-MW:残留 original 不触及 target 时 raw 必须
  保留、blocking API 可过滤。
- `Violation.rowIds/hash/isInGuard`:同 x 不同行 hash 不同,guard 能按行裁剪。✅
- invalid overlay batch 隔离:duplicate/non-filler/unknown/footprint mismatch
  只污染自己的 candidate。✅
- `ComplexMixedBatchDeterministic`:生产 checker 一次处理 64 个混合候选并重复整批,
  clean/residual/new/invalid 分类及 violation 顺序/rowIds/hash/diagnostic 完全确定。✅
- planner/checker 类型可在同一 adapter TU 共存。✅
- 真 UDM extraction + adapter E2E 等 TODO 12;当前 fake-UDM harness 只替代 DB
  边界,不替代 checker core。

## 3. 明确不做

- 不在 planner `test_main.cpp` 中复刻真实 checker DRC 语义;checker 行为测试统一
  放 `src/dpl2/src/drc/test/`。
- 不写依赖 fake 规则模型细节的"伪 DRC 正确性"断言(见总原则 2)。
- #8 adaptive-L1 已落地并有回归;后续只补发现的新边界,不保留旧 L1 sweep 语义。

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
SANITIZE=address ./run_tests.sh    # 80 cases + ASan

# 真实 checker core(fake UDM boundary,生产 checker 源码原样编译)
cd src/dpl2/src/drc/test
./run_tests.sh                    # 10 cases
SANITIZE=address ./run_tests.sh   # 10 cases + ASan
```
