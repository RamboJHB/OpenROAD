# Filler Insertion —— 对话总结与结论(写 code 前的基线)

> 目的:把调研对话中得出的**内容与结论**汇总成一份共识基线,作为**开始写 code 前**的起点。
> 分支:`claude/filler-insertion-impl`(基于 `2023-base`,干净分支,只含 filler 相关内容)。

## 0. 配套文档(本分支 `docs/`)

| 文件 | 内容 |
|---|---|
| `filler_insertion_summary.md`(本文) | 对话总结 + 结论 + 写 code 的准备 |
| `filler_insertion_study_report.md` | 完整调研报告(任务/背景/论文/代码现状/待澄清/**§7 MIA 机制图解**) |
| `filler_insertion_explained.md` | 现有 filler 代码逐函数讲解 |
| `filler_mia_figures/` | 配套示意图(fig1/2/3 行内、fig5 跨行、fig6 上下分 Vt)+ 生成脚本 + README |

---

## 1. 任务(一句话)

在 **OpenROAD(C++)** 写一个**只读 API**:输入一个跑完 P&R 的 design,输出 filler 应插在**哪些位置** + 每处插**哪种 type**;**不真正创建实例、不改动布线**;需支持 **mixed-cell-height(多行高)**。

---

## 2. 关键认识(我们达成的结论)

### 2.1 现有 filler 插入是"纯宽度"的,完全不懂注入层
`src/dpl/src/FillerPlacement.cpp`:`placeRowFillers` 逐行找空隙 + `gapFillers` 按宽度贪心装箱。**不看 Vt / implant** —— 正是论文所说的"朴素基线"。

### 2.2 MIA 是核心约束,作用在"连续注入区"上,不是按单元算
相邻同 Vt 单元的注入区连成一片,**整片**要满足最小面积/最小宽度(`Wmin`)。孤立的窄 Vt 区即违例。→ `fig1/2/3`。(注入层还有**最小间距 `Smin`** 约束,与 MIA 并列,见 §2.8。)

### 2.3 注入其实是"上下两条带":PMOS 上 / NMOS 下,MIA 分带各算
标准单元 PMOS 在上(N-well,近 VDD)、NMOS 在下(近 VSS),各有独立 Vt 注入 → 每个单元天生有**上带 + 下带**,MIA 对两带分别评估。→ `fig6`。

### 2.4 存在"上 LVT / 下 HVT"的非对称 Vt 单元,要用上下分 Vt 的 filler 匹配
只给瓶颈一侧提速、另一侧省漏电。补这种单元要用**同样上下分 Vt 的 filler**,一个 filler 一次补好两带。→ `fig6`。

### 2.5 跨行耦合(inter-row):镜像行共享电源轨 → PMOS 上带跨 2 行
- 2 行高的窄 Vt 区会**横跨两行**一起违例。
- **只修一行反而引发新的跨行违例**(论文一的核心难点)。
- 正确做法:**两行对齐一起补**(或 2 行高 filler),并避免留 `< Smin` 的注入缝隙(**implant spacing** 同时满足)。→ `fig5`。

### 2.6 "一个位置需要两种不同 filler"真正发生在跨行,不是单行
单行里上下分 Vt 单元用**一个**上下分 Vt filler 即可;**两种不同 filler** 出现在跨行——相邻行补一个完成 PMOS 上带、本行补另一个延展 NMOS 下带。

### 2.7 DRC marker 不持久化(drt)
`check_drc` 的 marker 跑完即销毁,此版本 odb 无 `dbMarker`。C++ 想拿违例,只能:(A) 改 `checkDRC` 暴露;(B) 写文件再解析;(C) 自跑检测并保活。另注:`check_drc` 的 min-width 在**金属层**,filler 改不了;filler 能修的是**注入/放置层**。

### 2.8 Spacing 与 MIA 是**并列**的注入层约束(co-constraint)
filler 不只要"够大"(MIA),还要满足**间距**:
- **注入层最小间距 `Smin`**:两块**同层(同 Vt)**注入区太近(`< Smin`)→ spacing 违例;异 Vt 注入边界、以及狭窄的 **notch/凹口/细缝**同样受约束。
- **Spacing 实际以横向(同行)为主**:`Smin` 形式上是 2D DRC 规则,但单元在电源轨处上下对接、注入无缝,**纵向 row-to-row 的 `Smin` 缝隙一般不出现**;**跨行真正的约束是 MIA 连片(§2.5 / `fig5`),不是纵向 spacing**。
- **实战推论**:补 filler 时要么**完全贴合(abut)**邻居,要么与异 Vt 注入区**留 ≥ `Smin`**;**绝不能留 `< Smin` 的细缝**。→ `fig7`。
- **反过来限制 filler**:放错 Vt 的 filler 会把**另一种 Vt** 注入区挤出过窄段(犯 MIA)或距离 `< Smin`(犯 spacing)→ "放不放 / 放哪种 Vt / 放多宽"都被 spacing 牵制。
- **well / 放置层间距**:filler 自带阱区,受**阱最小宽度/间距**约束;还要避让 **placement blockage、macro halo、固定对象**(dpl 的 `is_valid` 网格覆盖大部分,macro 周边间距要留意)。
- 现有 `gapFillers` 只有**宽度**层面的"最小 filler 宽度",**无注入间距感知**——论文二 "complex implant layer constraints" = 最小面积 + 最小宽度 + **最小间距**,三者缺一不可。

---

## 3. 唯一阻塞:API 的"目的"未与 leader 锁定

可能性:**(I)** 修 DRC 违例 / **(II)** 标准填充(电源轨/密度)/ **(III)** 密度/DFM / **(IV)** 仅输出给下游。
**"不真插"暗示这是分析步骤,目的多半藏在输出的下游用途里。**

**给 leader 的 6 个问题**(详见 study report §5.3):
1. API 目的是 (I)/(II)/(III)/(IV)?**输出给谁、拿去做什么?**
2. (若修 DRC)min-width 在金属层还是注入/放置层?
3. 违例/空隙怎么进 API:check_drc / 文件 / 自检测?
4. 多行高要"几何正确"还是"论文的跨行 MIA"?
5. 可用 filler 库有哪些?是否含多行高 / 上下分 Vt filler?
6. 怎么算做对?设计规模多大?

---

## 4. 两条路的工作量

| 假设 | 工作量 |
|---|---|
| **(II) 标准填充** | **高度复用现有代码**:把 `placeRowFillers` 里 `dbInst::create(...)` 换成把 `(type, x, y, row)` 收进输出列表返回;空隙枚举与 `gapFillers` 直接复用。 |
| **(I) 修 DRC** | 需先解决 §2.7 的 marker 获取 + implant-aware 选型,**接近复现论文**(行内聚类 + 跨行协调),工作量显著更大。 |

---

## 5. 写 code 的准备(下一步落地建议)

### 5.1 推荐先做:只读"违例/缺口检测原型"(两条路都用得上、与最终目的解耦)
扫描逻辑(对应 §2 的结论,**MIA 与 spacing 一起查**):
1. 枚举单元行空隙与占用 —— **复用 dpl 的 Pixel 网格**(`initGrid`/`gridPixel`/`row_site_count_`)。
2. **MIA**:对每个 **Vt 注入层**、**分上/下带**求连续区,标出宽度 `< Wmin` / 面积 `< Amin` 的段。
3. **Spacing**:标相邻注入区**横向**间距 `< Smin`(同行左右、placement gap 处)及 `< Smin` 的 notch/细缝。(纵向 row-to-row 一般因对接无缝而不触发,跨行交给下一步的 MIA。)
4. 把**相邻镜像行的上带视为同一片**(跨行合并),对 **MIA** 再判一次(此即跨行的主约束)。
5. 输出违例区 + **候选 filler 位置**;候选位置须满足 **abut 或 ≥ `Smin`**,且**补完不新生** MIA/spacing 违例(并避让 blockage/macro)。

### 5.2 数据结构草案
```cpp
struct FillerSpot {            // 输出:该放什么、放哪
  odb::dbMaster* type;         // filler 的 master(含 Vt / 行高)
  int x, y;                    // 位置(DBU)
  int row;                     // 起始基础行
  int width_sites;             // 宽度(site 数)
};
struct ImplantViol {           // 检测原型的中间产物
  odb::dbTechLayer* implant;   // 注入层
  odb::Rect region;            // 连续注入区(spacing 类则为相邻两区/缝隙)
  enum Kind { MinWidth, MinArea, MinSpacing } kind;  // MIA(宽/面积) 或 间距
  int value;                   // 实测宽度 / 面积 / 间距
  int limit;                   // 对应的 Wmin / Amin / Smin
  bool inter_row;              // 是否跨行
};
```

### 5.3 入口形态
dpl 内新增**只读**方法,如 `planFillers(filler_masters) -> std::vector<FillerSpot>`,经 Tcl/Python 暴露查询;不调用任何 `create`。

### 5.4 验证
先造**小测例**(自写 LEF/DEF,含 sub-`Wmin` 的注入带 + 一个上下分 Vt 单元 + 一处跨行窄区),在其上跑检测原型,核对违例与建议位置。

---

## 6. 分支与边界

- **本分支** `claude/filler-insertion-impl`(基于 `2023-base`):只放 filler 相关文档/代码。
- **DRC 相关工作不在本分支**:保留在 `claude/drt-check-drc-pg-option-ij3eH`。
- **下一步**:① 和 leader 对齐 §3 的 6 个问题;② 一旦目的确认,按 §5 在本分支开写(标准填充 → 改造现有 filler 为只读返回清单;修 DRC → 先搭检测原型)。
