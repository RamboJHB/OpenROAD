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

## 2. 输入 / 输出

### 输入
| 输入 | 说明 |
|---|---|
| `odb::dbBlock* block` | 已完成 placement/routing 的 design:rows/sites、placed insts、orient/flip、blockage/macro/fixed |
| **DRC markers** | 上游 filler DRC check 产出的 `spacing` / `min-width` 违例(位置、layer、rule) |
| **dirty 标记** | 触发违例的 filler 已被标 dirty(本步据此定位要删/要重填的对象) |
| **filler 库 + 顺序** | **(确认输入)** 可用 filler master 集合,每个含 `{VT/implant, width(site), dbMaster}`,**含 1-site filler**;并带**用户给定的顺序**(`setFillerMode -core {…}` 的列表序),`preserveUserOrder` 决定装箱是否按此序优先选用 |
| 选项 | `avoid_abutment_patterns {1:1}`、`fitGap false`、`check_signal_drc false`、`preserveUserOrder true` |

### 输出 / 副作用
- **删除** 被标 dirty 的 filler 实例。
- **创建** 修复后的 filler 实例(physical-only,orient 跟随行,标 PLACED)。
- 对窗口内**仍无法合法修复**的违例 → 不强行修,**报告为无解**回报上游。

---

## 3. 支持的违例类型(当前仅两类)

| 类型 | 含义 | filler 修复直觉 |
|---|---|---|
| **min-width** | implant/base-layer 某段 same-VT 条带宽度 < 最小宽度(常因 dirty filler VT 选错,形成过窄独立 implant 条) | 把 dirty filler 换成**正确 VT** 的 filler,使其 implant 与邻居并成一片;或在 footprint 内用更宽 filler 取代两窄(受 `{1:1}` 约束) |
| **spacing** | 相邻 implant 区间距 < 最小间距 | 选合适 VT/宽度,避免制造过近的 implant 边界 |

> 其余类型(**min-area** 等)**当前不支持**。这与 `avoid_abutment_patterns {1:1}` 自洽:`{1:1}` 防的正是「两窄 filler 相邻 → implant min-width/spacing」这一族成因。

---

## 4. 重填粒度 —— 严格「只动 dirty」(已定)⭐

- **重填窗口 = dirty filler 的 footprint**(相邻 dirty 合并成一段)。
- **绝不删/移 clean filler**。窗口边界(clean filler 或真实 cell)= **固定约束**:`{1:1}`、VT 连续等约束要**对这些固定边界成立**,但不能改它们。
- 窗口内换不出合法解 → **判无解**,回报上游(不扩窗去动 clean)。

### 上游契约(依赖,需与 DRC-check 人确认)
若某处修复需要连带相邻 filler(例:合并两颗 1-site 才能解 min-width,而其中一颗是 clean),**上游必须把那颗也 mark 成 dirty**,使「只动 dirty」即可自洽求解。否则该类违例会落到无解出口。

---

## 5. 选项语义(对齐 Innovus `setFillerMode` / `addFiller`)

| 选项 | 语义 | 本步行为 |
|---|---|---|
| `setFillerMode -core {cells}` | 指定 core filler 集合 | = 传入的 filler 库 |
| `-preserveUserOrder true` | 保持用户给的 filler 顺序,不按宽度重排 | 装箱时按传入顺序作选用优先级 |
| `-fitGap false` | 关掉「凑满 / 避残缝」优化(有 1-site filler 兜底时该优化多余;参 Innovus warning **IMPSP-5186**:`-fitGap` 与 `no_diffusion_one_site_filler` 冲突,有 1-site filler 建议关) | 不做精确凑满优化,直接按约束放 |
| `-avoid_abutment_patterns {1:1}` | 禁止特定相邻模式;解读为**禁「1-site : 1-site」相邻**(按左宽:右宽,site) | 装箱/边界检查中禁止两个 1-site 相邻(含与固定边界 clean 的相邻) |
| `-check_signal_drc false` | 插 filler 时不做 signal DRC | no-op(filler 为 physical-only,本就不跑) |

> ⚠️ `{1:1}` 精确维度(按 **site 宽度** vs 按 **cell-edge/implant 类型**)与 `fitGap` 默认值,仍建议以 `man setFillerMode` / `man addFiller` 原文坐实(见 §11 开放项 Q-A)。

---

## 6. 功能流程(multi-height 感知)

| # | 阶段 | 动作 | multi-height 考量 |
|---|---|---|---|
| 0 | 输入 | design + DRC markers + dirty 标记 + filler 库/顺序 + flags | filler 库须覆盖区域内每种行高的 VT |
| 1 | 建上下文 | 枚举受影响行;每行 site 列/行高/orient(R0,MX)/N-P band;标 fixed/macro/cell/clean-filler 占位 | multi-height cell 跨行占位;区域内行高可不一致 |
| 2 | 定窗口 | 由 dirty 标记取 footprint,相邻 dirty 合并成段;边界 clean/cell 记为固定 | 窗口可能跨多行(若 dirty 跨行) |
| 3 | 定 VT 上下文 | 由窗口左右/上下固定邻的 VT 决定目标 VT,保 implant 连续;对齐该行 N/P band | 跨行窗口须各行 VT 连续 |
| 4 | 删 dirty | 删除窗口内 dirty filler 实例 | — |
| 5 | 重填装箱 | 在窗口内选 filler 组合:选序=preserveUserOrder?用户序:宽度降序;约束=≤窗口宽 + `{1:1}`(含对固定边界)+ VT 连续 | 单高逐行 vs 多高跨行 = 决策 D1 |
| 6 | 校验 | 重检该窗口:目标违例消失、不引入新 spacing/min-width;`check_signal_drc=false`→跳过 signal DRC | 跨行 filler 上下边界连续性一并检 |
| 7 | 落子 / 回报 | 解出 → 建 filler 实例(orient 跟行,physical-only);无解 → 标记回报上游 | 多高 filler 跨行落一个实例 |

---

## 7. multi-height 处理(开放,待会上 D1–D5)

- **D1 粒度**:只做单高逐行填(简单稳)还是加多高跨行 filler(QoR 更好、PG/implant 更连续,但 2D 装箱)。
- **D2 VT 连续**:多高 filler 跨行各行 VT band 全连续是硬约束还是允许 fallback。
- **D3 orientation**:R0/MX 逐行交替下多高 filler 的 flip/配对规则。
- **D4 残隙合法性**:`fitGap false` 留隙时多高行不得制造非法 1-site gap / 卡死邻居。
- **D5 `{1:1}` 维度**:multi-height 下是否从「行内水平」扩成含垂直的 2D 相邻禁止。

---

## 8. `avoid_abutment_patterns {1:1}` 说明

- 记法 `{左宽 : 右宽}`,单位 = site。`{1:1}` = 禁「1-site 宽」紧挨「1-site 宽」。
- 根因:两颗最小宽 filler 拼接处的 implant 条带过窄/有缝 → 正是 min-width / spacing 违例来源。
- 修复倾向:同一空隙优先用**一颗 2-site** 取代 filler(1)+filler(1);`1:2` 等不在禁止表则允许。
- 与「只动 dirty」交互:若窗口边界是**固定 clean 1-site**,重填的边缘 filler 不能也用 1-site(否则与固定 clean 形成 1:1);避不开 → 判无解。
- (图解见 `filler_mia_figures/` 或随附 abut 图。)

---

## 9. 出 scope(上游 / 不做)

solver 可解性判定 · dirty 标记 · decap / M2 · trim-spacing 感知 · signal DRC 实跑 · min-area 等其它违例类型。

---

## 10. OpenROAD 落点与可复用底座

落点:`src/dpl/src/FillerPlacement.cpp` + `src/dpl/src/Opendp.tcl`(命令 `filler_placement`)。

最新 master 已具备、**直接复用**的底座:
- **按 implant/VT 选 filler**:`splitByImplant()` / `getImplant(master)`(取 master obstruction 里 `IMPLANT` 层)。
- **多行高装箱**:`gapFillers()` 按 `implant → row_height → gap` 缓存,height-matched 装箱;`getShortestSite` 逐行取最短 site。
- **1-site 残隙规避雏形**:`have_filler1` / `gap-1` 判断(与 `{1:1}` 相关但不同,需扩展)。
- 删除/识别:`removeFillers()` / `isFiller()`(`CORE_SPACER` 且非 LOCKED)。

本步要在此底座上**新增**:
| 能力 | 现状 | 本步 |
|---|---|---|
| 消费 DRC marker + dirty 标记定位窗口 | ❌ 全 core 填白 | **新增** |
| `preserveUserOrder` | ❌ 总按宽度降序(`fillerPlacement` 105-107) | **新增 flag** |
| `avoid_abutment_patterns {1:1}` | ❌ 无相邻禁止 | **新增**(装箱 + 固定边界) |
| `fitGap` 开关 | ❌ 无 | **新增**(关凑满优化) |
| 只动 dirty 窗口 + 无解回报 | ❌ 填不上即 `error` | **新增**(标记回报上游) |
| `check_signal_drc` | ✅ 本就不跑 | 接受 flag 作 no-op |

---

## 11. 开放项 & 历史

### 待敲定(会上 / man page)
- **Q-A** `{1:1}` 精确维度:按 site 宽度 vs 按 cell-edge/implant 类型(影响要不要 edge-type 元数据)。
- **D1–D5** multi-height(见 §7)。
- **上游契约**:确认上游会把「需连带的」filler 也标 dirty(见 §4)。

### 历史(已被本版取代)
早先方向是**只读 repair-planning API**:自检 6 类违例(intra/inter MW、intra/inter MS、MinArea、MF),按 Zou2023 做 DP 近最优规划,**不改 DB**,实现于 `src/dpl/src/ImplantRepairPlanner.*`(独立 toy 测试 23/23)。
新需求改为:**消费上游 DRC marker + dirty 标记**、只修 **spacing + min-width**、**真删真建**、**只动 dirty** + Innovus flag 语义。
⟶ 旧 `ImplantRepairPlanner` 的违例检测/DP 不再是主路径;其「同 VT filler 扩展修 min-width」「重检不引入新违例」等局部逻辑可作为窗口内装箱/校验的参考再利用。**是否保留该文件待定。**
