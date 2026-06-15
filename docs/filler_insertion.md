# Filler Insertion —— 结论 + API 接口提案

> **任务**:在 OpenROAD(C++)做一个**只读**分析 API。输入一个跑完 P&R、DRC 干净的 design,输出「在哪插哪种 filler」+「哪些违例必须动单元」。**不创建实例、不动单元、不动布线。** 需支持 mixed-cell-height。
>
> 依据论文:Chen 2021 (TCAD, *Mixed-Cell-Height Detailed Placement Considering Complex MIA*);**Zou 2023 (DAC'23, *Toward Optimal Filler Cell Insertion with Complex Implant Layer Constraints* —— 本任务的技术蓝本)**。

---

## 1. 已核实的模型

- **一个 cell = 一个 VT;一个 filler = 一个 VT;filler type 就是按 VT 分**(LVT / SVT / HVT,可扩展 ULVT)。一行内可有多种 VT。
- **没有 PMOS/NMOS 上下分带** —— 注入按 cell/site 的单一 VT 处理(两篇论文均无分带模型)。
- **implant 约束 = 5 条**(Zou 的约束集 Ω):

  | 约束 | 含义 |
  |---|---|
  | **intra-row MW** | 同一行里某段注入宽度 < 阈值 |
  | **inter-row MW** | 相邻两行**同 VT** 的水平搭接宽度 < 阈值(staircase) |
  | **intra-row MS** | 同一行里两块注入区的间距 < 阈值 |
  | **inter-row MS** | 相邻两行 cell 的间距 < 阈值 |
  | **MF** | filler 不能比最小 filler 宽度窄 → 有些空隙**填不了** |

  - **MW = 最小注入宽度,MS = 最小间距;两者都分 intra-row(同行)和 inter-row(相邻行)。即 spacing 是二维的,同行和上下行都查。**
- **违例分两类**:**可解**(插 filler 即可修)/ **不可解**(必须移动单元,filler 阶段修不了)。本任务只读 → 修可解、**标注**不可解。

配图(`filler_mia_figures/`):`fig1` MIA 概念、`fig2`/`fig3` 同 VT filler 补足注入宽度、`fig7` intra-row MS(abut / ≥间距 / 细缝违例)。

---

## 2. OpenROAD 现状(基线)

- 现有 filler 插入 `src/dpl/src/FillerPlacement.cpp`(`placeRowFillers` / `gapFillers`):**纯按宽度贪心装箱,完全不懂 VT / implant** —— 即论文所说的朴素基线。
- dpl 的 **Pixel 网格**(`initGrid` / `gridPixel` / `row_site_count_`)可复用来枚举 whitespace 与占用;mixed-cell-height 已有几何占位支持。
- drt 的 `check_drc` marker 不持久化(此版本 odb 无 `dbMarker`)。

---

## 3. Insertion API 接口提案

### 输入
| 输入 | 说明 |
|---|---|
| `odb::dbBlock* block` | 已布局布线的 design:rows/sites、placed insts、orient/flip、blockage/macro |
| `FillerLibrary fillers` | 可用 filler 集合,每个含 `{VT, width_sites}` |
| `ImplantRules Ω` | 5 个阈值 `{intraMW, interMW, intraMS, interMS, MF}`(来自 tech/DRC) |
| `cell → VT` 映射 | 每个 placed master 属于哪个 VT / implant 类型 |
| implant 层标识 | tech 里哪些层是 VT implant 层 |

### 输出
```cpp
struct FillerSpot {                 // 建议插入的 filler
  odb::dbMaster* filler;            // 选定 filler(含 VT + 宽度)
  int x, y, row, width_sites;
};
struct ImplantViol {                // 检测到的违例
  enum Type { IntraMW, InterMW, IntraMS, InterMS, MF } type;
  odb::Rect region; int row; VtType vt;
  bool solvable;                   // true = 可插 filler 解决;false = 须动单元
};
struct Result {
  std::vector<FillerSpot> suggestions;   // 针对可解违例的 filler 建议(位置 + VT)
  std::vector<ImplantViol> violations;   // 全部违例(含可解 / 不可解标记)
};
```

### 形态
- dpl 内只读方法,例如 `Result planFillerInsertion(const FillerLibrary&, const ImplantRules&)`,Tcl/Python 暴露查询。
- **只读:不 `create` 实例、不动单元、不动布线。**

---

## 4. 需要跟「做 DRC 的人」要的 data

设计虽然「DRC 干净 + PR 跑完」,但 DEF/DB 本身**不一定带全**做 implant 检测所需的信息。需要:

1. **implant / VT 层的规则值(最关键)**:`intra-row MW、inter-row MW、intra-row MS、inter-row MS、minimum filler width MF`,要确切数字(site / DBU)。在 tech LEF / DRC deck 里。
2. **cell → VT 映射**:每个 cell master 属于哪个 VT,或它在 LEF/lib 的哪个属性 / 哪个 implant 层上。
3. **filler / decap 库清单**:有哪些 filler master,各自的 **VT + 宽度**(决定我能插什么)。
4. **implant 层的层名 / 定义**:tech 里 VT implant 是哪几层、叫什么(用于定位注入几何)。
5. **(若有)implant 相关的 DRC 违例报告**:本次 DRC 跑出的 implant MW/MS 违例(位置 + 类型),可当 ground truth 校验检测器。
6. **placement blockage / macro / fixed cells**:避让区域(通常在 DEF,确认一下)。

---

## 5. 一个必须先对齐的边界问题

设计是「**DRC 干净**」的,所以要先确认 **DRC deck 是否已包含 implant MW/MS 规则**:

- **若已包含、且已干净** → 当前没有残留 implant 违例 → 这个 API 的目标是「**在不引入违例的前提下合法填充 whitespace**」。
- **若不包含** → implant 检测就由这个 API 来做 → 目标是「**检测 + 修可解违例**」。

这直接决定 API 是「修违例」还是「合法填充」,需与 DRC / leader 确认。第 4 节第 1、5 项的回答基本能判定这一点。
