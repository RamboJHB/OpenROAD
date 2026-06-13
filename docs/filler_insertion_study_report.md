# Filler Insertion API —— 调研报告(Study Report)

> 阶段:需求调研(study),尚未进入编码。
> 目的:把任务背景、相关概念、论文、OpenROAD 代码现状、需求理解与待澄清点,梳理成一份可执行的基线。

---

## 1. 任务概述

被分配的工作:在 **OpenROAD(C++)** 里做一个 API ——

- **输入**:一个跑完 P&R、做过 DRC 的 design。
- **输出**:filler 应该插在**哪些位置**、每个位置插**哪种 type**。
- **行为**:**只读分析,不真正创建 filler 实例;不改动已有布线。**
- **范围**:需考虑 **mixed-cell-height(多行高)**。

当前最大的不确定:**API 的最终目的**尚未和 leader 锁定(见 §5)。本报告以两种可能为主线:**(I) 修 DRC 违例** 与 **(II) 标准填充**。

---

## 2. 背景概念

| 概念 | 一句话解释 |
|---|---|
| **Filler(填充单元)** | 不实现逻辑的填充单元;插进单元行的空隙,接续电源轨(VDD/VSS)、保证阱区连续、补足图形密度。 |
| **Site / Row** | Row = 芯片被横切成的等高行;Site = 行内最小宽度网格。单元宽度 = site 整数倍,高度 = row 整数倍。 |
| **Multi-Vt** | 同功能单元有 LVT/SVT/HVT 多版本,靠不同**注入层**实现,平衡速度与功耗。不同 Vt 的注入区不能随意连片。 |
| **Implant layer(注入层)** | 决定掺杂区域的掩膜层;按"区域多边形"制造,相邻同型单元的注入区会连成一片。 |
| **MIA(Min Implant Area)** | 工艺规则:每片连续注入区必须满足**最小面积/最小宽度**,否则 DRC 违例、做不出来。 |
| **Min-width** | 某层形状宽度低于该层最小宽度规则 → min-width 违例。 |
| **Mixed-cell-height(多行高)** | 单元高度不统一(1×/2×/3× 行高);高单元同时占据多个基础行,使行间耦合。 |
| **Detailed Placement (DP)** | 详细布局:把全局布局后的单元挪到完全合法的位置。 |
| **DP constraint vs objective** | **约束**=必须满足的硬规则(on-site、不重叠、朝向对齐、MIA/min-width…);**目标**=尽量优化的软指标(线长、面积)。filler/MIA 是一种约束。 |

---

## 3. 相关论文

### 3.1 论文一(TCAD 2020)
**Mixed-Cell-Height Detailed Placement Considering Complex Minimum-Implant-Area Constraints**

- **问题**:在 **多行高** 设计的详细布局中,同时满足 **行内 + 行间 MIA** 约束。难点:① 纯插 filler 面积/线长代价大;② 移动多行高单元会引发跨行 MIA 违例。
- **方法(三步)**:
  1. **行内**:把同 Vt 的违例单元聚类,在簇内重排;特例用 **DP 求最优**,一般用 **Algorithm DLX**(精确覆盖),有常数近似比保证。
  2. **网络流**:把剩余违例单元分配到合适的 filler 位置,修违例并最小化面积。
  3. **行间**:最小位移平移修跨行违例。
- **结果**:解决全部 MIA 违例,**零额外面积**。

### 3.2 论文二(DAC 2023)
**Toward Optimal Filler Cell Insertion with Complex Implant Layer Constraints**

- **问题**:multi-Vt 设计中,纯做 **filler insertion** 满足复杂注入层规则 + 最小 filler 宽度。**不动功能单元**,更聚焦、更工程化。
- **方法(三组件)**:① **推断式违例检测**(插之前预判冲突);② **DP 插入**(放 filler 减少注入违例);③ **轮廓驱动精修**。并把识别器前移到 legalization 阶段提前避冲突。
- **结果**:违例数显著少于 SOTA;工业案例几乎全解。

### 3.3 关系与演进
两篇同主题(注入层/MIA 约束下的填充)。论文一在**布局阶段动单元**(网络流为主);论文二收窄到 **filler insertion 本身**(DP + 推断检测,更快更优、可工业化)。**演进 = 从"动布局修 MIA"走向"专精 filler、提前预判"。**

---

## 4. OpenROAD 代码现状(基线)

### 4.1 现有 filler insertion —— `src/dpl/src/FillerPlacement.cpp`
- `placeRowFillers`(:81-135):逐行扫描,找连续空隙(Pixel 网格 `cell==nullptr && is_valid`),量出 gap。
- `gapFillers`(:154-179):**纯按宽度**的贪心装箱(从宽到窄)+ 按 gap 缓存;内置"最小 filler 宽度"逻辑(避免留下填不了的 1-site 空隙)。
- **完全不考虑注入层 / Vt** —— 这正是论文所说的"朴素基线"。

### 4.2 DRC / min-width 的获取 —— drt
- `check_drc` 在 `TritonRoute::checkDRC`(`TritonRoute.cpp`)内检测,min-width 检测在 `FlexGC_main.cpp::checkMetalShape_minWidth`(比较形状宽度与 `layer->getMinWidth()`)。
- 违例存为 **`frMarker`**(`frMarker.h`):带 bbox、layerNum、constraint(min-width 的 `getViolName()` = `"Min Width"`)。
- **关键限制**:marker **不持久化**,`check_drc` 跑完即销毁;此版本 **odb 无 `dbMarker` 类**。要在 C++ 拿到,只能:(A) 改 `checkDRC` 暴露 marker;(B) 写文件再解析;(C) 自跑检测并保活列表。
- ⚠️ **概念注意**:`check_drc` 的 min-width 在**金属/布线层**;**filler 是放置对象,改不了金属线太细**。filler 能修的是**注入/放置层**的 min-width/MIA。两者是否同一回事,必须澄清。

### 4.3 多行高现状 —— dpl 已支持(仅几何层面)
- `dbToOpendp.cpp:113-116`:高度为基础行高整数倍的 master 标记 `is_multi_row`;`have_multi_row_cells_` 跟踪。
- 网格把高单元覆盖的**所有基础行**标记为占用 → `placeRowFillers` 逐基础行扫空隙时**高单元会在每行正确挡位,filler 不会重叠**。
- **已具备**:多行高的几何/占位正确。**未具备**:跨行注入协调、多行高 filler 选择、任何 MIA 逻辑。

### 4.4 可复用件与入口
- **空隙枚举**:dpl 的 Pixel 网格(`gridPixel`、`row_site_count_`、`initGrid`/`visitCellPixels`)。注意绑在 `Opendp` 类,外部使用需进 dpl 或搬初始化代码。
- **入口**:`ord::OpenRoad::openRoad()` → `getDb()/getTritonRoute()/getOpendp()`;`block->getInsts()`、`block->getRows()`、`dbTechLayer::getMinWidth()` 均为标准 odb API。

---

## 5. 需求理解与待澄清

### 5.1 已确定
输入=跑完 P&R 的 design;输出=filler **type + 位置**;**只读、不真插、不动走线**;OpenROAD **C++**;需支持 **多行高**。

### 5.2 关键未决(按优先级)
1. **API 的目的到底是什么?**(最根本)
   - (I) 修 DRC 违例(min-width/MIA);(II) 标准填充(电源轨/密度);(III) 密度/DFM;(IV) 只输出给下游。
   - **输出最终给谁、拿去做什么** —— "不真插"暗示它是分析步骤,目的多半藏在下游用途里。
2. **若是 (I)**:min-width 在**哪一层**?金属层(`check_drc` 报,filler 修不了)vs 注入/放置层(filler 能修)。
3. **违例/空隙信息怎么进 API**:check_drc 结果 / DRC 文件 / 自检测。
4. **多行高要哪种**:(a) 仅几何正确(现有基本够)vs (b) 论文的跨行 MIA 协调(等于把注入约束/修 DRC 拉回来)。
5. **filler 库**有哪些(决定 "type";是否含多行高 filler)。
6. **验收标准 + 设计规模**。

### 5.3 给 leader 的精简问题
> 1. API 目的是 (I) 修 DRC / (II) 标准填充 / (III) DFM / (IV) 输出给下游?**输出给谁用?**
> 2. (若修 DRC)min-width 在金属层还是注入/放置层?
> 3. 违例怎么进 API:check_drc / 文件 / 自检测?
> 4. 多行高要"几何正确"还是"论文的跨行 MIA"?
> 5. 可用 filler 库?是否含多行高 filler?
> 6. 怎么算做对、规模多大?

---

## 6. 初步方案设想(假定 = 标准填充 + 多行高几何)

若 leader 确认为 **(II) 标准填充、非修 DRC**,则任务高度复用现有代码:

- **核心改造**:把 `placeRowFillers` 里 `dbInst::create(...)` 那一步,换成把 `(master/type, x, y, row)` **收集进输出列表返回**,不创建实例。空隙枚举与 `gapFillers` 直接复用。
- **多行高**:逐基础行填(现有逻辑即正确);可选优化 = 若 filler 库含多行高 filler,对整块多行空隙用高 filler 一次填满。
- **输出数据结构(草案)**:`vector<FillerSpot{ dbMaster* type; int x, y; int row; int width_sites; }>`。
- **入口形态**:dpl 内新增只读方法(如 `planFillers(filler_masters) -> vector<FillerSpot>`),Tcl/Python 暴露查询。

若 leader 确认为 **(I) 修 DRC**,则需先解决 §4.2 的概念问题(哪一层)与 marker 获取路径,工作量显著更大(接近复现论文)。

---

## 7. 结论 / 下一步

1. **概念与基线已清楚**:filler 的作用、两篇论文、OpenROAD 现状(width-based、不 implant-aware;多行高仅几何支持;DRC marker 不持久化)。
2. **唯一阻塞**:API **目的 + 输出用途** 未定(§5.2 第 1 条)。
3. **下一步**:拿 §5.3 六个问题和 leader 对齐;一旦确认"标准填充 + 多行高几何",即可按 §6 在本分支开始编码 —— 本质是把现有 filler 插入逻辑改写为"返回位置清单"的只读 API。
