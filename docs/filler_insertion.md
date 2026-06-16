# Filler Insertion —— DRC 修复目标 + API 接口提案

> **任务**: 在 OpenROAD(C++) 做一个**只读** filler insertion repair-planning API。输入一个完成 placement/routing 后的 design、implant DRC 规则/报告、VT cell/filler 信息，输出「在哪插哪种 VT filler 来修 implant DRC」+「哪些 min-area/min-width 违例必须移动单元或改 implant 才能修」。**API 本身不创建实例、不移动单元、不改布线**；它输出可执行的修复方案与不可修复原因。需支持 mixed-cell-height。
>
> **目标已确认**: filler insertion 的目的不是普通填白，也不是假设 design 已经 implant DRC clean；目标是修复 implant 相关 DRC，尤其是 **minimum implant area / minimum width** 类问题。spacing/MS 规则仍必须一起检查，因为错误的 filler VT 选择可能引入新的 spacing violation。
>
> 依据资料: LEF/DEF 5.8 `Layer (Implant)` 对 implant `WIDTH`、`SPACING`、`LEF58_AREA`、`LEF58_WIDTH` 的定义；Chen 2021 (TCAD, *Mixed-Cell-Height Detailed Placement Considering Complex Minimum-Implant-Area Constraints*); **Zou 2023 (DAC'23, *Toward Optimal Filler Cell Insertion with Complex Implant Layer Constraints* —— 本任务的技术蓝本)**。

---

## 1. 核心结论

- DAC'23 的 filler insertion problem 是: 在**不移动 placed cells** 的前提下，把不同 VT 的 fillers 插入 whitespace，使 implant layer violations 最小。
- 这对应到本任务就是 **implant DRC repair planning**: 用同 VT filler 把过窄/过小的 implant island 或 staircase 区域补宽、补面积，优先修 minimum implant area / minimum width 相关违例。
- LEF/DEF implant 规则也支持这个理解: implant layer 的 width/spacing/area 规则会影响合法 placement；某些 min-width/min-spacing 形态可通过插入合适 implant type 的 filler 修复，另一些则必须由 placer 避免，或通过移动单元/修改 implant 几何修复。
- **只读 API 的含义**是“不直接改 DB”，不是“不修 DRC”。它应该输出一个 filler repair plan；真正创建 filler instance 可以由后续 command 使用这些 suggestions 完成。

---

## 2. 已核实的模型

- **一个 cell = 一个 VT; 一个 filler = 一个 VT; filler type 按 VT 分**(LVT / SVT / HVT, 可扩展 ULVT)。一行内可有多种 VT。
- **没有 PMOS/NMOS 上下分带** —— 注入按 cell/site 的单一 VT 处理。
- **implant 约束 = 5 条**(Zou 的约束集 Ω):

  | 约束 | DRC 含义 | filler repair 直觉 |
  |---|---|---|
  | **intra-row MW** | 同一行里某段 same-VT implant width 太窄 | 在相邻 whitespace 插同 VT filler，合并成更宽 implant 区 |
  | **inter-row MW** | 相邻行 same-VT overlap 太窄，形成 staircase min-width 问题 | 选择相邻行/当前行 filler VT，增加垂直/水平 overlap |
  | **intra-row MS** | 同一行不同 implant 区间距太小 | 避免插入会制造过小间距的 VT；必要时标成不可修 |
  | **inter-row MS** | 相邻行 implant 关系导致 spacing 太小 | 插入方案必须同时检查上下行 window |
  | **MF** | 可用 filler 不能小于最小 filler 宽度 | 太小 gap 可能物理上不可填，需标注不可修 |

  - **MW = minimum width / MIA repair 的主要约束；MS = minimum spacing co-constraint。** 两者都分 intra-row 和 inter-row。
  - DRC deck/tech LEF 可能用 `WIDTH`、`LEF58_WIDTH`、`LEF58_AREA`、`SPACING` 等形式表达规则；实现时需要把这些规则统一转换成 site/DBU 级别的 Ω。
- **违例分两类**:
  - **fillable / repairable**: 只靠插合适 VT filler 可修。
  - **unfillable / unrepairable by filler**: 没有足够 whitespace、受 MF 限制、或需要移动单元/改 implant 几何。只读 API 要明确报告这类情况。

配图(`filler_mia_figures/`): `fig1` MIA 概念、`fig2`/`fig3` 同 VT filler 补足注入宽度、`fig7` intra-row MS(abut / >=间距 / 细缝违例)。

---

## 3. OpenROAD 现状(最新 master）

> 本分支已 rebase 到最新 `master`。最新 `src/dpl/src/FillerPlacement.cpp` 与旧基线不同——**filler 插入已经是 implant-aware**，dpl 也重构（`grid_` / `network_` / typed coords `GridX/GridY/DbuX/DbuY` / `Node` / `Pixel`）。

**调用链**：`filler_placement`(Tcl）→ `Opendp::fillerPlacement(filler_masters, prefix, verbose)`：

1. `filterFillerMasters()` —— 去掉 PAD / BLOCK 类 master。
2. `splitByImplant()` —— 按 implant 层把 filler 分组成 `MasterByImplant = map<dbTechLayer*, dbMasterSeq>`；implant 由 `getImplant(master)` 决定（取 master obstruction 里类型为 `IMPLANT` 的 tech layer）。
3. 每个 implant 组按宽度降序排序。
4. `initGrid()` + `setGridCells()`（把 cell 占的 pixel 标记占用）。
5. 逐行 `placeRowFillers(row, prefix, by_implant)`，最后统计/打印 filler 数。

**`placeRowFillers`（核心）**：沿一行扫 site，找连续空隙 `[j, k)`；**这段用哪种 implant 取左邻 cell（`j==0` 时取右邻），都没有则用任意一组** → 即「按邻居 cell 的 implant 选 filler」；`gapFillers(implant, gap, row_height, …)` 返回填充用的 master 序列，空 → `error`，否则用 `makeUniqueDbInst(..., physical_only=true)` **创建** filler 实例（设 orient/location/`PLACED`/`DIST`）。

**`gapFillers`**：按 `implant → row_height → gap` 三级缓存（`gap_fillers_`）；在**同 implant 且同 `row_height`** 的 filler 里 widest-first 贪心装箱；用 `have_filler1` / 避免 `gap-1` 规避「留下一个填不掉的 1-site」（最小 filler 宽度雏形）；height-matched（多行高只用匹配高度的 filler）。

**其它**：`removeFillers()`、`isFiller()`（`CORE_SPACER` 且非 `LOCKED`）、`isOneSiteCell()`。

**对本任务的意义**：

| | 最新 master 已有 | 本任务还要做 |
|---|---|---|
| implant 匹配 | ✅ 按邻居 cell 的 implant 选 filler（`getImplant` / `splitByImplant`） | **直接复用** |
| 多行高 | ✅ height-matched 装箱 | 复用 |
| 最小 filler 宽 | ⚠️ 雏形（`have_filler1` / `gap-1`） | 升级成完整 **MF** |
| **违例检测** | ❌ 不检测 MW/MS/MIA | **新增**（repair target） |
| **inter-row 协调** | ❌ 只逐行独立填 | **新增** |
| **只读 / repair-plan** | ❌ 直接 `create` 实例、填满所有 gap | **改成只读返回 `FillerRepairPlan`** |
| 可解/不可解 | ❌ 填不上就 `error` | **改成标 `remaining` + reason** |

一句话：master 已把「按 implant 选 filler + 多行高装箱」做好（正是要复用的底座）；本任务要加的是 **违例检测 + inter-row 协调 + 只读 repair-plan 输出 + 可解/不可解分类**。

- drt 的 `check_drc` marker：若要用 DRC 结果作为 repair target / ground truth，需外部 DRC report/marker 输入或 marker import（实现时确认当前 ODB `dbMarker` 的可用性）。

---

## 4. Repair API 接口提案

### 输入
| 输入 | 说明 |
|---|---|
| `odb::dbBlock* block` | 已 placement/routing 的 design: rows/sites、placed insts、orient/flip、blockage/macro/fixed cells |
| `FillerLibrary fillers` | 可用 filler 集合，每个含 `{VT, width_sites, dbMaster}` |
| `ImplantRules Ω` | 规则阈值 `{intraMW, intraMS, interMW, interMS, MF}`，单位需明确为 DBU 或 site |
| `cell -> VT` 映射 | 每个 placed master 属于哪个 VT / implant 类型 |
| implant layer/rule mapping | tech 里 VT implant 层名、implant group、WIDTH/AREA/SPACING rule 来源 |
| `DrcViolationReport`(可选但推荐) | DRC 已报出的 min-area/min-width/min-spacing implant violations，作为 repair target 与校验 ground truth |

### 输出
```cpp
struct FillerSpot {                 // 建议插入的 filler repair
  odb::dbMaster* filler;            // 选定 filler master(含 VT + 宽度)
  VtType vt;
  int x_dbu, y_dbu;
  int row;
  int width_sites;
  std::vector<int> repairs;         // 修复/缓解的 violation id
};

struct ImplantViolation {           // 检测到或由 DRC report 输入的违例
  enum Type { IntraMW, InterMW, IntraMS, InterMS, MinArea, MF } type;
  odb::Rect region;
  int row;
  VtType vt;
  bool repairable_by_filler;
  std::string reason;               // 例如 no whitespace / below MF / needs cell movement
};

struct FillerRepairPlan {
  std::vector<FillerSpot> suggestions;       // 可执行的 filler 插入建议
  std::vector<ImplantViolation> repaired;    // 被 suggestions 覆盖的违例
  std::vector<ImplantViolation> remaining;   // filler 无法修的违例
};
```

### 形态
- dpl 内只读方法，例如 `FillerRepairPlan planFillerInsertion(const FillerLibrary&, const ImplantRules&, const DrcViolationReport*)`，Tcl/Python 暴露查询。
- **只读**: 不 `create` filler instance、不动单元、不动布线；只返回 repair plan 和 remaining violations。

---

## 5. 建议的 repair flow

1. 从 `dbBlock` 和 Pixel grid 建 site table: placed cell / empty / blockage / fixed / row validity。
2. 用 `cell -> VT` 与 filler library 把 placed cells 和 fillable sites 标成 Zou 论文里的 `L_P` / `L_F` 标签。
3. 从 tech/DRC deck 读取或外部传入 Ω，并把 LEF/DRC rule 单位转成 site/DBU。
4. 对 DRC report 中的 violation window 做优先 repair；没有 report 时，用 Ω 扫描全局 window 识别 intra/inter-row MW/MS/MIA 风险。
5. 对每一行结合 adjacent rows 做 DP/near-optimal filler assignment，目标函数是最小化 remaining implant violations，同时满足 MF。
6. 对填不了的 violation 输出 `remaining` 和 reason，交给 placer/legalizer/post-process 做 cell movement 或 implant edit。

---

## 6. 需要跟「做 DRC 的人」要的数据

1. **implant / VT 层的规则值(最关键)**: minimum implant area、minimum width、intra-row/inter-row MW、intra-row/inter-row MS、minimum filler width MF；要确切数字和单位(site / DBU / micron)。
2. **DRC 违例报告/marker**: min-area/min-width/min-spacing implant violations 的位置、layer、rule name、severity；这是 repair target 和验收基准。
3. **implant 层的层名 / group / rule mapping**: tech 里 VT implant 是哪几层，是否有 implant group、`LEF58_WIDTH`、`LEF58_AREA`、`SPACING`、`CHECKIMPLANTGROUP` 等特殊规则。
4. **cell -> VT 映射**: 每个 cell master 属于哪个 VT，或它在 LEF/lib 的哪个属性 / 哪个 implant 层上。
5. **filler / decap 库清单**: 有哪些 filler master，各自的 **VT + 宽度**，以及最小可用 filler 宽度。
6. **placement blockage / macro / fixed cells**: 避让区域和不可移动对象，通常在 DEF/DB 中，但需要确认是否完整。
7. **验收口径**: repair plan 应该使哪些 DRC count 归零，哪些不可修情况允许上报给 placement/legalization。

---

## 7. 已对齐的边界

- 本任务目标是 **修 implant DRC min-area/min-width**，不是“DRC clean 后安全填白”。
- 如果输入 design 在目标 implant deck 下已经没有相关 violations，API 可以返回空 repair plan 或仅报告 safe filler choices；但主要使用场景是 DRC 已发现 implant violations，需要规划 filler-based repair。
- 如果某个 violation 类似 LEF/DEF 示例中的不可由 filler 修复形态，或受 whitespace/MF 限制，API 不应假装能修；应输出 `remaining`，要求移动单元、调整 placement，或由后处理修改 implant 几何。

---

## 8. 实现与测试（本分支已落地）

### 文件
- **核心（零依赖、已测）**：`src/dpl/src/ImplantRepairPlanner.h` / `.cpp` —— 5 类违例检测 + 只读 repair 规划 + 可解/不可解分类。不依赖 ODB/dpl，可独立编译与单测。
- **toy 测试（覆盖所有情况）**：`src/dpl/test/implant_repair_planner_test.cpp` —— 用 implant-layer 的 site-grid（ASCII）造出每一类违例与每种结局。

### 逻辑链（详见头文件注释）
1. `detect(layout)`：扫描「painted」site-grid，列出 6 类违例（intra/inter MW、intra/inter MS、MinArea、MF），各带 implant + 行/列窗口。
2. 对每个违例在工作副本上尝试**纯 filler 修复**：窄/小区域 → 用同 implant filler 向相邻空 site 扩展；两同 implant 区太近 → 填空隙合并；staircase overlap 太窄 → 在较短那行填空 site 加宽。修复须满足：有足够空白、扩展宽度可被 filler 平铺（每片 ≥ MF）、重检后该违例消失且不引入新违例。
3. 成功 → `suggestions` + `repaired`；否则 → `remaining` + 原因（no whitespace / below MF / needs cell movement）。**全程不改 DB。**

### 覆盖与测试结果
toy case 覆盖：干净；intra-row MW（可修 / 无空白 / blockage / 低于 MF 四种）；MinArea（可修）；intra-row MS（合并可修 / 间隙被占→remaining / 低于 MF→remaining）；inter-row MW（加宽可修）；inter-row MS（needs movement→remaining）。

独立编译运行（本沙箱已验证）：
```
g++ -std=c++17 -I src/dpl/src \
  src/dpl/src/ImplantRepairPlanner.cpp \
  src/dpl/test/implant_repair_planner_test.cpp -o /tmp/irp_test && /tmp/irp_test
# => ImplantRepairPlanner test: 23 checks passed, 0 failed.
```

### 接进 OpenROAD/dpl（集成层，需完整构建环境）
把核心接成与 `filler_placement` 同款的 Tcl 命令（如 `plan_filler_repair`），adapter 做三件事：
1. **Layout**：遍历 `grid_` 的 row×site（`getRowCount`/`getRowSiteCount`/`gridPixel`），`Pixel::cell` 有无 → Cell/Empty，blockage → Blocked；cell 的 implant **复用 dpl 现成的 `getImplant(master)`**（取 obstruction 里 IMPLANT 层）映射成 `ImplantId`。
2. **Rules Ω**：从 implant `dbTechLayer` 读 min width / spacing / area（`getWidth`/`getSpacing`/LEF58 area），intra/inter 拆分与 MF 由 DRC deck/配置给出（单位转 site）。
3. **Filler**：对 `filler_masters` 复用 `splitByImplant`，每个 `{getImplant, getWidth/site}` → `Filler`。

调用 `ImplantRepairPlanner::plan()`，把 `FillerRepairPlan` 经 Tcl/Python 返回（只读，不 `makeUniqueDbInst`）。

> ⚠️ **构建说明**：本沙箱**无法构建完整 OpenROAD**（缺 `swig`/`bazel`、无预构建二进制、依赖过重），所以上面的 dpl/Tcl 集成层与 LEF/DEF 回归**未在此环境编译运行**；**算法核心已用独立 toy 测试（23/23）在本沙箱验证**，覆盖全部违例与结局。集成层与 ODB 签名请在能构建的环境里接入校验。

---

## 9. 算法精修路线图（下一步 = 升级到论文二的近最优修复）

当前核心是「逐违例 + 贪心」修复；论文二（Zou 2023）是「**推断式检测 + DP 近最优插入 + 轮廓精修**」。按风险/收益排优先级：

### R1 —— 补齐修复能力（近期、低风险、沙箱可单测）
- **InterMS 跨行合并**：现在 InterMS 一律 `remaining`。精修：当两相邻行同 implant 区可由**填空 site 形成竖直 overlap**（两行分别朝对方列扩展直到共列）而合并时判为可修；桥接 site 须 EMPTY 且可平铺，否则仍 `remaining(needs movement)`。
- **MinArea 最优扩展方向**：现在只扩代表 run；改为在区域所有 run 的相邻空白里选「代价最小（占空白最少 / 不诱发新违例）」的方向扩。
- **修复顺序 / 回溯**：贪心顺序可能错失（修 A 用掉了 B 需要的空白）。加按「窗口重叠度 / 可解性」排序 + 小范围回溯；已有的「重检不引入新违例」保留为护栏。
- 验收：扩 toy 测试 —— InterMS 可修例 + 抢空白冲突例通过。

### R2 —— 对齐论文二（核心、收益最大、工作量最大）
- **推断式检测（prime rule set / window）**：用矩形窗口模式 + 规则推断一次性定位违例，替代逐行多遍扫描（可前移到 legalization 阶段提前预判）。
- **DP 近最优插入**：对每行结合相邻行上下文做 DP（状态 = 区间标签分布，目标 = 最小化 remaining 违例，约束 MF），论文二把复杂度优化到线性；替换当前逐违例贪心。
- **轮廓驱动精修（contour-driven）**：在 filler interval 间建依赖链，把违例「传递 / 转移」到可解处，减少人工修补。
- 验收：多 VT + mixed-height 合成大例上 remaining 数 ≤ 贪心版；复现论文二「几乎全解」趋势。

### R3 —— 目标函数 / 取舍
- cost = Σ(违例权重) + λ·filler 面积；支持 **filler type 优先级** 与 **VT 重指派**（论文二列的可扩展点）；支持 >3 VT（ULVT…）。

### R4 —— 集成与验证（落到 OpenROAD）
- 按 §8 把核心接成 `plan_filler_repair` 只读 Tcl 命令（adapter：`grid_`/`getImplant`→Layout，`dbTechLayer`→Rules）。
- 加**真实 LEF/DEF 回归**（含 IMPLANT 层 + 多 VT 库），用 `add-test` 在 CMake+Bazel 双注册；与 master `fillerPlacement` 结果对照。
- 性能：大设计下检测 / DP 的复杂度与运行时间基线。

### 取舍建议
先做 **R1**（直接提升修复率、风险低、沙箱可验证）→ 再做 **R4 集成**（让它在真 design 上可跑）→ 最后上 **R2** 的 DP/推断（收益最大但最重）。
