# Filler Insertion —— DRC 驱动的 dirty-filler 修复(需求定稿)

> **状态(2026-06)**:需求已与「做 filler DRC check 的人」「PV / detail router 的人」对齐确认。
> 本版**取代**早先的「只读 repair-planning API / 自检 6 类违例 / Zou2023 DP 近最优」草案(见 §11 历史)。
> 早先实现的只读 `ImplantRepairPlanner`(`src/dpl/src/ImplantRepairPlanner.*`,23/23 toy 测试)在新需求下**不再是主路径**——是否保留/改造见 §11。

---

## 1. 需求总览(一句话)

输入一个**带 DRC marker 的 design**,其中**触发 DRC 违例的 filler 已被上游 mark 成 dirty**;本步(filler insertion = "my work")的工作是:

> **删除这些 dirty filler,在其原 footprint 上重填正确的 filler,修复 `spacing` 与 `min-width` 两类 implant/base-layer 违例。**

这是一个**真改 DB** 的步骤(删实例 + 建实例),不是只读规划,也不是普通填白。
输入除了 design 与 DRC marker/dirty 标记,还包含**用户给定的 filler 库及其顺序**(`setFillerMode -core`),重填即从该库按 `preserveUserOrder` 选用。

白板流水线中的位置:
```
DRC Violation → Solver(可解?否→Unsolvable) → mark dirty filler → [filler insertion ← 本 doc]
```
Solver(可解性判定)与 mark-dirty 是**上游/别人负责**,不在本步。

---

## 1.5 背景:上游链(为什么会有 dirty filler)

dirty filler 与 implant 违例**不是凭空出现**,而是布线后两步**扰动**的产物:

```
… route → [opto] → [ECO legalization] → filler DRC check → mark dirty → [filler insertion ← 我们]
            └────── 扰动源 ──────┘        └─── 上游检测/标记 ───┘
```

| 步骤 | 做什么 | 对 filler 的影响 / 产物 |
|---|---|---|
| **opto(优化)** | postRoute/ECO 收 timing/DRV:gate sizing、buffer 插删、clone、pin swap(OpenROAD ≈ `rsz`) | 删受扰区 filler;**改变 cell 的 VT/implant 邻接** → 埋下新违例 |
| **ECO legalization** | 增量合法化:最小位移把 cell snap 回合法 site/row(OpenROAD ≈ `dpl` 增量 legalize) | 再次挪 cell、占位变化、原 filler 处留新空隙 → **制造/挪动** implant 违例 |
| **filler DRC check** | 扫 implant 的 `spacing` / `min-width` 规则 | 产出 **DRC markers** |
| **mark dirty filler** | 把惹事的 filler 标 dirty(需连带的也一并标 — §4 上游契约) | 产出 **dirty filler 集合** |
| **★ filler insertion(我们)** | 删 dirty + 按正确 VT 重填(只动 dirty) | 修掉 `spacing` / `min-width`;无解→回报上游 |

一句话:**opto 与 ECO legalization 是「扰动源」,本步是在其之后做 implant-aware 的 filler 收尾修复**。所以输入才是「带 DRC marker、dirty 已标」的 design——marker 与 dirty 正是这两步扰动的产物。

---

## 2. 输入 / 输出

### 输入
| 输入 | 说明 |
|---|---|
| `odb::dbBlock* block` | 已完成 placement/routing 的 design:rows/sites、placed insts、orient/flip、blockage/macro/fixed |
| **DRC markers** | 上游 filler DRC check 产出的 `spacing` / `min-width` 违例(位置、layer、rule) |
| **dirty 标记** | 触发违例的 filler 已被标 dirty(本步据此定位要删/要重填的对象) |
| **filler 库 + 顺序** | **(确认输入)** 可用 filler master 集合,每个含 `{VT/implant, width(site), dbMaster}`,**当前库无 1-site filler**(最小宽度 > 1 site);并带**用户给定的顺序**(`setFillerMode -core {…}` 的列表序),`preserveUserOrder` 决定装箱是否按此序优先选用 |
| 选项 | `preserveUserOrder true`、`check_signal_drc false`(`fitGap` 不暴露 — 见 §5;`avoid_abutment_patterns` 本期暂忽略) |

### 输出 / 副作用
- **删除** 被标 dirty 的 filler 实例。
- **创建** 修复后的 filler 实例(physical-only,orient 跟随行,标 PLACED)。
- 对窗口内**仍无法合法修复**的违例 → 不强行修,**报告为无解**回报上游。

---

## 3. 支持的违例类型(当前仅两类)

| 类型 | 含义 | filler 修复直觉 |
|---|---|---|
| **min-width** | implant/base-layer 某段 same-VT 条带宽度 < 最小宽度(常因 dirty filler VT 选错,形成过窄独立 implant 条) | 把 dirty filler 换成**正确 VT** 的 filler,使其 implant 与邻居并成一片;或在 footprint 内用更宽 filler 取代两窄 |
| **spacing** | 相邻 implant 区间距 < 最小间距 | 选合适 VT/宽度,避免制造过近的 implant 边界 |

> 其余类型(**min-area** 等)**当前不支持**。

---

## 4. 重填粒度 —— 严格「只动 dirty」(已定)⭐

- **重填窗口 = dirty filler 的 footprint**(相邻 dirty 合并成一段)。
- **绝不删/移 clean filler**。窗口边界(clean filler 或真实 cell)= **固定约束**:VT 连续等约束要**对这些固定边界成立**,但不能改它们。
- 窗口内换不出合法解 → **判无解**,回报上游(不扩窗去动 clean)。

### 上游契约(依赖,需与 DRC-check 人确认)
若某处修复需要连带相邻 filler(例:合并两颗 1-site 才能解 min-width,而其中一颗是 clean),**上游必须把那颗也 mark 成 dirty**,使「只动 dirty」即可自洽求解。否则该类违例会落到无解出口。

---

## 5. 选项语义(对齐 Innovus `setFillerMode` / `addFiller`)

| 选项 | 语义 | 本步行为 |
|---|---|---|
| `setFillerMode -core {cells}` | 指定 core filler 集合 | = 传入的 filler 库 |
| `-preserveUserOrder true` | 保持用户给的 filler 顺序,不按宽度重排 | 装箱时按传入顺序作选用优先级 |
| `-fitGap` | **精确填 / 避孤缝**:选 filler 组合凑出**正好填满** gap,避免留下填不掉的残缝。例:gap=9、库无 1-site filler → 用 **4+3+2**,**不能用 8**(否则剩 1 site 孤缝补不掉)。**无 1-site filler 时必须开**;只有有 1-site filler 兜底时才可关(参 Innovus **IMPSP-5186**)。 | **接口不暴露此选项**:Innovus 默认 `false`,但本库**无 1-site filler ⇒ 必须精确填**,故装箱行为**固定为精确填**(回溯凑满),不设 flag。 |
| `-check_signal_drc false` | 插 filler 时不做 signal DRC | no-op(filler 为 physical-only,本就不跑) |
| ~~`-avoid_abutment_patterns {1:1}`~~ | 禁特定相邻模式 | **本期暂忽略,不实现**(见 §8) |

---

## 6. 分阶段(Phase I 单行 / Phase II 多行)⭐

| | **Phase I(已实现)** | **Phase II(后续)** |
|---|---|---|
| 范围 | **逐行独立**修 dirty 窗口 | 跨行:真·多行高 + inter-row |
| 违例 | intra-row spacing / min-width | 再加 inter-row MW / MS |
| 上下文 | 仅窗口**左右**(同行)邻居 VT | 再加**上/下行**邻居 VT |
| filler | 仅 height-1 | 多高 filler 跨行 |

**先把 Phase I 做扎实,多行内容全部归 Phase II。**

### 6.1 Phase I 功能流程(单行)
| # | 阶段 | 动作 |
|---|---|---|
| 1 | 扫描 | 逐行找极大 dirty 连续段 → 单行窗口{row,col0,width};取左右固定邻 VT |
| 2 | 删 dirty | 清空窗口内 dirty 站点 |
| 3 | 选 VT 方案 | 延左 VT / 延右 VT / `L\|R` 拆分 / 孤立任意 VT(查 min-width) |
| 4 | 精确填 | 每段 `packExact` 回溯凑满(fitGap,见 §5);选序=preserveUserOrder |
| 5 | 落子 / 回报 | 解出→建 height-1 filler;无解→标记回报上游 |

---

## 7. Phase II:多行(multi-height + inter-row)—— 后续

> 当前 `FillerRepair` 是 **Phase I 单行**。以下为 Phase II 扩展计划,数据模型(`Filler.height`、`FillerGrid`)已为此预留,Phase II 是扩展而非重写。

### 7.1 真·多行高 filler
- 把相邻行**同列同宽**的 dirty 窗口竖直合并成矩形;
- 窗口高 H 用 `partitionHeight` 拆成 filler 高度条带,放跨行 filler;
- 决策点:D1 粒度、D2 跨行 VT band 连续、D3 R0/MX flip 配对、D4 各行精确填可行性。
- (这套逻辑此前实现过,见 git 历史 commit `f8ac28861`,因 Phase 划分回退到单行。)

### 7.2 inter-row MW / MS(对齐论文 Algorithm 4 的 cost-table 思想)
implant 是 2D:同 VT 在上下行拼成跨行区域。Phase I 只看左右,会漏:
- **inter-row MW**:同 VT 在相邻两行竖直重叠太窄(楼梯)< ωw2;
- **inter-row MS**:不同 VT 在上下行靠太近 < ωs2。

**要补**:
- 数据:窗口加 `top_vt[col]` / `bot_vt[col]`(查上/下行;`FillerGrid::vtAt` 已够,无需改接口);
- 规则:`Rules` 加 `ωw2`(inter-row MW)、`ωs2`(inter-row MS);
- 逻辑:选 VT 方案时按**端点**检查上下行(论文 cost-table 的硬约束版),撞了就排除该方案,全排除→无解。

论文按端点查表加**代价**(可留违例、最小化);我们按端点查**硬约束**(撞了就换/无解)。端点检查是论文把复杂度降到线性的关键,照搬即可。

---

## 8. `avoid_abutment_patterns {1:1}` 说明(本期暂忽略)

> **状态:本期不实现。** 原因:当前库**无 1-site filler**,我们放不出 1-site,filler 之间永不会出现 `1:1` → `{1:1}` 约束**暂时空触发**。待将来引入 1-site filler,或确认 `{1:1}` 实为 edge/implant 类型规则(Q-A)时再纳入。以下为概念留档。

- 记法 `{左宽 : 右宽}`,单位 = site。`{1:1}` = 禁「1-site 宽」紧挨「1-site 宽」。
- 根因:两颗最小宽 filler 拼接处的 implant 条带过窄/有缝 → 正是 min-width / spacing 违例来源。
- 修复倾向:同一空隙优先用**一颗 2-site** 取代 filler(1)+filler(1);`1:2` 等不在禁止表则允许。
- 与「只动 dirty」交互:若窗口边界是**固定 clean 1-site**,重填的边缘 filler 不能也用 1-site(否则与固定 clean 形成 1:1);避不开 → 判无解。
- (图解见 `filler_mia_figures/` 或随附 abut 图。)

---

## 9. 出 scope(上游 / 不做)

solver 可解性判定 · dirty 标记 · decap / M2 · trim-spacing 感知 · signal DRC 实跑 · min-area 等其它违例类型。

---

## 9.5 实现与移植(Phase I 已落地)

零依赖核心 + 可移植 grid 接口已实现(**Phase I 单行**):
- `src/dpl/src/FillerRepair.{h,cpp}` —— 算法核心(单行窗口、exact-fill、同行 VT 连续)。
- `FillerGrid`(接口)—— 算法唯一的 DB 接缝;另接一个 database 只需实现它。
- `src/dpl/src/FakeFillerGrid.h` —— 测试用内存实现(移植时替换)。
- `src/dpl/src/FillerGridAdapter.example.h` —— 适配器模板(照抄填空)。
- `src/dpl/test/filler_repair_test.cpp` —— 独立测试(g++ 可跑,**20/20**,均为单行情形)。
- **移植指南**:`docs/filler_repair_porting.md`(接口契约、SiteKind 映射、几何/master 职责、构建接入片段、检查清单)。

> DRC 部分按需求略过:测试直接喂「已标 dirty 的 grid」。
> **多行高 + inter-row 见 §7(Phase II)**;数据模型已预留 `height`,届时扩展。

## 10. OpenROAD 落点与可复用底座

落点:`src/dpl/src/FillerPlacement.cpp` + `src/dpl/src/Opendp.tcl`(命令 `filler_placement`)。

最新 master 已具备、**直接复用**的底座:
- **按 implant/VT 选 filler**:`splitByImplant()` / `getImplant(master)`(取 master obstruction 里 `IMPLANT` 层)。
- **多行高装箱**:`gapFillers()` 按 `implant → row_height → gap` 缓存,height-matched 装箱;`getShortestSite` 逐行取最短 site。
- **精确填 / 1-site 残隙规避**:`gapFillers()` 的 widest-first 装箱 + `have_filler1` / `gap-1` 判断,已是「避孤缝」雏形——正是 §5 fitGap 行为的基础,需扩展成完整回溯凑满。
- 删除/识别:`removeFillers()` / `isFiller()`(`CORE_SPACER` 且非 LOCKED)。

本步要在此底座上**新增**:
| 能力 | 现状 | 本步 |
|---|---|---|
| 消费 DRC marker + dirty 标记定位窗口 | ❌ 全 core 填白 | **新增** |
| `preserveUserOrder` | ❌ 总按宽度降序(`fillerPlacement` 105-107) | **新增 flag** |
| 精确填(fitGap 行为,固定开) | ⚠️ 雏形(widest-first + `gap-1`) | **升级成回溯凑满**(不暴露 flag) |
| 只动 dirty 窗口 + 无解回报 | ❌ 填不上即 `error` | **新增**(标记回报上游) |
| `check_signal_drc` | ✅ 本就不跑 | 接受 flag 作 no-op |
| ~~`avoid_abutment_patterns`~~ | ❌ 无 | **本期暂忽略** |

---

## 11. 开放项 & 历史

### 已定
- **fitGap**:语义=精确填/避孤缝;库无 1-site filler ⇒ 固定精确填,**接口不暴露 `-fitGap`**(见 §5)。
- **avoid_abutment_patterns**:**本期暂忽略**(见 §8)。

### 待敲定(会上 / man page)
- **Q-A** `{1:1}` 精确维度(若将来纳入):按 site 宽度 vs 按 cell-edge/implant 类型。
- **D1–D4** multi-height(见 §7)。
- **上游契约**:确认上游会把「需连带的」filler 也标 dirty(见 §4)。

### 历史(已被本版取代)
早先方向是**只读 repair-planning API**:自检 6 类违例(intra/inter MW、intra/inter MS、MinArea、MF),按 Zou2023 做 DP 近最优规划,**不改 DB**,实现于 `src/dpl/src/ImplantRepairPlanner.*`(独立 toy 测试 23/23)。
新需求改为:**消费上游 DRC marker + dirty 标记**、只修 **spacing + min-width**、**真删真建**、**只动 dirty** + Innovus flag 语义。
⟶ 旧 `ImplantRepairPlanner` 的违例检测/DP 不再是主路径;其「同 VT filler 扩展修 min-width」「重检不引入新违例」等局部逻辑可作为窗口内装箱/校验的参考再利用。**是否保留该文件待定。**
