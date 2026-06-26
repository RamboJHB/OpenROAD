# 功能规格 — Filler VT 修复(跨行 MW/MS,只替换 VT)

状态:两条路径均已实现并测试通过(Part A 标准库 15/15 · Part B oracle 12/12)。
最后更新 2026-06。

核心一句话:布线/ECO 扰动后产生的 **implant 层 MW/MS 违例**,本模块**只通过替换
filler 的 VT(implant 类型)**来修——不移动/删除 cell,不移动 filler,不改占用。

---

## 0. 文档导航 —— 两套实现路径

本模块的**算法(决定换哪些 filler 的 VT)**是一套;**违例评估(判 MW/MS)**有两种
后端,因此有两条实现路径。共享部分(§1–§5)先读,再按需读 Part A 或 Part B。

| | **Part A — 自带 site-grid 评估器** | **Part B — 接 ImplantLayerChecker oracle** |
|---|---|---|
| 定位 | 原型 / 可移植 / 无依赖单测 | **生产路径(推荐)** |
| 违例评估 | 自己的 `countViolations`(site 网格,MW/MS,含 case-B) | 上游 checker 的 `checkPlace/checkDirect` |
| 覆盖 | MW/MS 的粗略子集 | P/N、PRL、abutment、containment… 全覆盖 |
| 代码 | `FillerVtRepair.{h,cpp}` + `FillerGrid.h` + `FakeFillerGrid.h` | `FillerVtRepairOracle.{h,cpp}` + `ImplantChecker.h` + `FakeImplantChecker.h` |
| 测试 | `filler_vt_repair_test.cpp`(15/15) | `filler_vt_repair_oracle_test.cpp`(12/12) |
| 章节 | §6 算法 · §7 `FillerGrid` 接口 · §8 行为详解 · §12 DP 附录 | §13 全节(13.1 架构 · 13.3 数据对照 · 13.7 接口绑定 · 13.8 RD 问题) |

> 两条路径**共用同一套决策思路**(贪心 / 未来 DP),只是「问谁违例多少」不同。
> 生产上线走 Part B;Part A 是没有 checker 时的可移植后备,也是 case-B 等模型的
> 参考实现与单测桩。
>
> **章节归属**:§1–§5 共享;**Part A** = §6 §7 §8 §12;**Part B** = §13;
> §9 约束 / §10 验证 / §11 待办 为两者**共用**。

---

## 1. 目的(共享)

布线后,优化(`opto`)和 ECO 合法化会扰动版图、破坏 filler,在相邻行之间
产生 **implant 层 DRC 违例**。本模块修复其中的 **跨行最小宽度(MW)** 和
**最小间距(MS)** 违例,手段是 **替换 filler 的 VT(implant 类型)**——
它**绝不**移动/缩放/删除标准 cell,也**绝不**移动 filler;只是把某个 filler
就地换成另一种 implant 类型的 filler。

核心直觉:**filler site = 可自由改的 implant 变量,cell site = 固定的
implant 边界**。在「cell 固定」的前提下,给每个 filler site 选一个 implant
类型,使整张图的 MW/MS 违例数最少,然后用「删旧 filler + 在原位建新 VT 的
filler」把这个选择落地。

非目标:这**不是**foundry 级的多边形 DRC。它工作在 **site 网格(site-grid)
implant 模型** 上(每个被占用的 site 带一个 VT);精确 DRC 在上游。

---

## 2. 上下文(模块位置)

```
… route → opto → ECO 合法化 → filler DRC 检查 → 标记违例 → 【本模块】
                  └──── 扰动源 ────┘   └──── 上游检测/标记 ────┘
```

DRC 检查和违例标记都在**上游**。本模块消费「网格 + 规则值(+ 违例 marker)」,
编辑 filler,把自己修不了的部分作为**残留**回报上游(上游再决定是否动 cell /
改 implant——那些不在本模块范围内)。

---

## 3. 范围

做(in scope):
- 违例类型:implant 层的 **最小宽度(MW)** 与 **最小间距(MS)**,
  含**跨行**情形,用 site 网格建模。
- 修复动作:**替换 filler 的 VT**(删旧 filler,在相同 site 建一个目标 implant
  类型的新 filler)。cell / macro / blockage 是固定边界。

不做(out of scope,→ 作为残留回报上游):
- 移动 / 缩放 / 删除标准 cell;
- 移动 filler 或改变「哪些 site 被占用」这件事;
- min-area 等其它 implant 规则;
- DRC 检查与违例标记本身。

### 3.1 前置条件:100% utility 检查(否则只告警、不修复)

**拿到 design 后,先检查它是不是 100% utility(每个可放置 site 都被占满)**——
即在要处理的区域内,**没有空 site**(每个 site 要么是 `Cell`、要么是 filler;
`Blocked`/macro 不算空隙)。只有满足这个前提,本模块才进行修复。

- **为什么必须 100% 填满**:本模块的 implant 模型把「被占用 site 的 VT」当成
  连续的 implant 画面来算 MW/MS,并且靠**替换 filler 的 VT** 来调整这幅画面。
  如果区域里还有**空 site**,说明 filler 还没插满(filler insertion 没跑完、
  或存在真实空隙):此时 ① implant 画面有真实断口,MW/MS 的语义会变
  (空隙引入的间距/断宽不是本模块该处理的);② 没有可替换的 filler 去覆盖这些
  空隙——只换 VT 修不了空洞。继续修复只会得到误导性的结果。
- **行为**:若检测到不是 100% utility → **issue 一个 warning,并直接返回、
  不做任何修复**(不删、不建 filler)。结果里标明「因未满 100% utility 跳过」。
  把「先把 design 填满」这件事交回上游(filler insertion / 合法化)。
- **检查方式**:遍历区域内所有 site,只要存在 `kindAt(r,c) == Empty` 即判定
  不满足。可只在要处理的区域(整图 / 或 §4.3 给的违例 region)内检查。
- 这条前提与论文式最优 DP(§12)也吻合:DP 假设填充连续、无空隙,正好就是
  100% utility。

---

## 4. 输入 / 输出

本模块的输入来自四个来源:**数据库**、**filler 库**、**DRC checker / tech
规则**、**适配器(几何)**。下面 §4.1–§4.4 逐一说明;§4.5 是输出。

### 4.1 来自数据库(通过 `FillerGrid` 接口,见 §7)
- 行 / site 结构:`numRows()`、`numCols(row)`。
- 每个 site 的**类别** `kindAt`:`Cell` / `CleanFiller` / `Empty` / `Blocked`。
- 每个被占用 site 的 **VT** `vtAt`:该 site master 携带的 implant 层身份。

### 4.2 来自 filler 库
- 每个可用 filler master:`{vt, width(site 数), height(行数), name}`。
- 关键约束:某个 VT 在某段宽度上「可实现」,当且仅当库里能用该 VT 的 master
  **精确平铺**这段宽度。生产库通常**没有 1 site 宽的 filler**,所以必须精确
  填满,不能留残缝(原型用 height-1 的 filler 重铺改动的 run)。

### 4.3 来自 DRC checker / tech 规则(**本节是要和 DRC 团队对齐的接口**)

> **更新**:上游 checker 的真实类(`ImplantLayerChecker`)已给出,接口比本节
> 当初设想的更丰富,且它本身是个**增量 check/commit oracle**。请以 **§13** 为准;
> 本节(§4.3)保留作「最小概念需求」的说明。`type/rule_value/vts/region/
> participants` 这些字段在真实接口里对应 `Rule` 与 `Violation`(见 §13.3)。

本模块是 filler DRC 检查的**下游**,需要从那边拿到下面这些数据:

1. **规则阈值(必需)** — 直接决定评估器判违例的标准:
   - `min_width`(ωMW):同一 implant 层允许的最小宽度(单位:**site 数**)。
   - `min_spacing`(ωMS):不同 implant 区之间的最小间距(单位:**site 数**;
     `≥1` 表示不同 VT 不得相邻接触)。
   - 若**每个 implant 层规则不同**,给一张表:`层名 → (ωMW, ωMS)`。
   - **P/N band 可能不同(待考虑)**:同一 VT 的 implant 在一行的 **P band**(PMOS
     侧)和 **N band**(NMOS 侧)上,**ωMS / ωMW 可能不一样**。所以更完备的形式是
     `(层名, band∈{P,N}) → (ωMW, ωMS)`;评估器要知道每个 site 落在 P 还是 N band
     (由行的 N/P 划分或 row orientation 推出,适配器提供)。当前模型按**单一**
     ωMW/ωMS 处理,P/N 区分留作后续(见 §9、§11)。
   - 这些值本质来自 tech LEF / rule deck,DRC checker 已在使用;本模块需要拿到
     **换算成 site 数**后的值(或拿到原始 DBU 值 + 由适配器换算)。

2. **违例 marker 列表(可选,启用「定向修复」模式时需要)** — 每条违例一行:

   | 字段 | 内容 | 为什么需要 |
   |---|---|---|
   | `type` | `MW` \| `MS` | 分流处理 |
   | `rule_value` | ωMW(最小宽)或 ωMS(最小间距),单位 site / DBU | 求解的硬阈值 |
   | `vts` | MW = 1 个 VT;MS = 2 个 VT | 知道改成 / 避开哪个 VT |
   | `region` | **逐行**的列区间:`[(row, col_lo, col_hi), …]`(不是单个 bbox) | 跨行形状不规则,必须逐行给列范围 |
   | `participants` | 形成违例**每一侧**的 owner 实例 id + 类型(`filler` / `cell` / `blockage`) | ① 判断是否 filler-filler ② 定位要替换的 filler |
   | `fixable_by_filler` | bool(可选但强烈建议) | checker 先粗筛 A/C/D/E vs B,省我们重算 |

   字段说明:
   - `vts` 用计数语义把两类违例分清:**MW 给 1 个 VT**(哪条同 VT 区太窄,要把
     它加宽);**MS 给 2 个 VT**(哪两个 implant 区贴在一起,要把其中一侧改成
     另一个、或都改成同一个来消除边界)。
   - `region` 必须是**逐行列区间**而不是一个 bbox:跨行违例(台阶 / 不规则块)
     的形状不是矩形,只有逐行 `[col_lo, col_hi)` 才能精确圈出要动的 filler site 。
   - `participants` 给出违例**两侧**各自的 owner 实例 + 类型,用来 ① 确认这是
     「filler 对 filler / filler 对 cell」(从而知道哪侧可动),② 在 site 网格上
     **定位**要替换的 filler 实例。
   - `fixable_by_filler` 是 checker 的**粗筛**,但**必须理解为必要非充分**(详见
     §13.10):它只可靠判断「违例**有没有可改 filler 参与**」——`false`(两侧都是
     cell,case B / LEF Fig 3-1 "B")可放心跳过转上游;`true` 只表示**可能**可修,
     真正能不能修好仍由我们用 `checkPlace` 搜索验证(checker 不知道我们的库可实现
     性,也没做候选搜索)。

3. **隐含契约**:违例已由 DRC checker 圈定。当前 Tier-1 实现是**整图扫描**,
   严格说只依赖第 (1) 项阈值;第 (2) 项违例列表是给**未来的定向修复模式 /
   加速**用的,但先写进接口,方便 DRC 团队一次对齐。

> 小结:**最小可用集 = 只要第 (1) 项规则阈值**(加上 §4.1 的网格)即可跑起来;
> 第 (2) 项违例列表让本模块能「只在违例附近动 filler」,规模大时更快、更可控。

### 4.4 来自适配器(几何,算法本身不碰)
- core 区原点、 site 宽、行高(DBU ↔ site/行 的换算)。
- 每行 orientation(R0 / MX …),使新建 filler 的 rail / implant 对齐。
- `(vt, width, height) → master` 映射,供 `placeFiller` 建实例用。
- **VT ↔ implant 层映射**:把 implant 层名映射成 VT id,`vtAt` 和建库都用它。

### 4.5 输出 / 副作用
- 数据库被修改:被选中改 VT 的 filler 被删除,在原位重建为新 VT 的 filler
  (physical-only,orientation 跟随所在行)。
- 一条 `VtRepairResult` 记录:`violations_before`、`violations_after`、
  `replaced`(被重建的 filler 列表)、`unresolved`(= `violations_after`,
  即残留,回报上游)。
- 若 §3.1 的 100% utility 前置检查不通过:**发 warning 并跳过修复**,结果记录
  标明 `skipped_not_full_utility`(不删不建任何 filler)。

---

## 5. 功能需求

- **FR-0 100% utility 前置门**:修复前先检查 design 是否 100% 填满(无空 site,
  见 §3.1)。不满足 → 发 warning、直接返回、不做任何修复。
- **FR-1 只动 filler**:绝不移动/缩放/删除 cell、macro、blockage,也绝不移动
  filler。cell 是固定的 VT 边界;只有 filler site 的 implant 类型可以改。
- **FR-2 MW/MS 目标**:选 filler 的 VT,使整张 site 网格的 MW + MS 违例总数
  **最小**。
- **FR-3 最大化修复,而非要么全清要么失败**:目标是**最小化残留**违例,不是
  「必须清零否则报错」。残留(库里没有所需 VT、被 cell 卡住、或改了会制造新
  违例)→ 如实回报。
- **FR-4 可实现性**:给某段 filler run 选的 VT 必须能被库里的 master **精确
  平铺**;否则这段 run 回退到原 VT(计入残留)。不允许部分填 / 留残缝。

---

> ━━━ **Part A — 自带 site-grid 评估器(原型 / 可移植)** ━━━ (§6 §7 §8 §12)

## 6. 算法 — `FillerVtRepair`(Part A)

1. **读网格** 到三层 site 网格:`vt[r][c]`、`present[r][c]`(`Cell | CleanFiller`)、
   `filler[r][c]`(可改 == `CleanFiller`)。另存 `orig` = 原始 VT。
2. **2D MW/MS 评估器** `countViolations(vt, present, rules)`:
   > **生产中由 checker 取代**:真实集成把违例评估交给上游 `ImplantLayerChecker`
   > 的 `checkPlace / checkDirect`(见 §13),本函数只留作无依赖单测的桩。因此
   > case-B、P/N、PRL 等由 checker 处理;下面的 site 网格模型是其**粗略子集**。
   > **run = 某个方向上「连续、同一 VT、且都被占用」的一段 site**(以及它的长度)。
   > 例如某行 `… L L L …` 里这 3 个 L 是一条**水平 run**,长度 3;`run = 1` 就是
   > 这个方向上孤零零一个 site,旁边不是同 VT。MW 就是在量「同 VT 区有多窄」,
   > 量的就是 run / 交叠的长度。
   - **MW(两类窄颈)**:同一 VT 的 implant 区任何地方都要 ≥ ωMW 宽。site 网格上
     有两种窄颈:
     - **(a) 行内/对角窄条**:一个 site 的同 VT **水平 run 与垂直 run 都** < ωMW
       (例如 1 宽的孤立点或对角台阶)→ +1。
     - **(b) 跨行交叠颈(case B)**:上下相邻两行的同 VT 区**只在很少几列上重叠**
       ——两行各自很宽,但错开,连接处(交叠列)太细。判法:对每条相邻行边界
       `(r, r+1)`,取「两行同 VT 且都 present」的极大连续列段;若该列段宽度
       < ωMW(且它是连接两侧更大区域的**颈**,而非整块本就小)→ +1。
       **已实现**:`countViolations` 现已包含 (b)(单测 T6:错开 vs 对齐)。判定
       要求「桥窄于 ωMW **且**两行的同 VT 水平 run 都比桥宽」,从而只抓 (a) 漏掉的
       真颈、不与 (a) 重复计。
   - **MS**:两个正交相邻(右 / 下)的被占用 site 若 **VT 不同** → +1(当
     ωMS ≥ 1,不同 implant 区不得接触)。只看右邻和下邻,避免重复计数。
3. **贪心坐标下降** 遍历 filler site:对每个 filler site,试遍库里所有 VT,
   保留使复合代价 `违例数*1000 − 同VT相邻数` 最小的那个。第二项(合并项)用来
   打破「单 site 平台」——有些改动只有和邻居一起改才划算,合并项给它一个方向。
   迭代到不动点(有轮数上限)。
4. **落地**:对每段「VT 变了的」极大同 VT filler run,用该 VT 的库 master
   `exactFill` 精确平铺该宽度 → `clearSite` 删旧 filler、`placeFiller` 建新的。
   铺不出来的 run **回退**(计入残留)。
5. `violations_after` = 残留;回报。

> Tier-2(未来):列 DP(论文式最优分配)。当前贪心已能覆盖台阶 MW 和跨行 MS。
> 论文算法的详细解释、可实现性评估,以及它需要的**最完备上游输出**见 **§12**。

---

## 7. 接口与数据模型(可移植)— 新数据库要提供什么

算法只通过抽象接口 `FillerGrid`(`dpl2/src/FillerGrid.h`)碰数据库。移植 =
针对新数据库实现这**一个类**。坐标全是**site / 行**,从不是 DBU;适配器掌管
DBU 几何和 `(vt, width, height) → master` 映射。

### 7.1 `FillerGrid` 接口(6 个方法)

```cpp
class FillerGrid {
 public:
  virtual ~FillerGrid() = default;
  // ---- 读 ----
  virtual int      numRows() const = 0;                  // 行数
  virtual int      numCols(int row) const = 0;           // 该行的 site 数
  virtual SiteKind kindAt(int row, int col) const = 0;   // Empty/Cell/CleanFiller/Blocked
  virtual Vt       vtAt(int row, int col) const = 0;     // 被占用 site 的 implant id
  // ---- 写 ----
  virtual void clearSite(int row, int col) = 0;          // 删这里的 filler -> 变 Empty
  virtual void placeFiller(const PlacedFiller& f) = 0;   // 建一个 filler 实例
};
```

每个方法要做什么、以及实现它需要新数据库暴露什么:

| 方法 | 返回 / 行为 | 实现它需要新 DB 暴露的数据 |
|---|---|---|
| `numRows()` | 行数 | core 区的 placement 行 |
| `numCols(row)` | 该行 site 数 | 行宽 / site 宽 |
| `kindAt(r,c)` | site 分类 | 覆盖该 site 的实例:是 **filler**(等价 CORE SPACER)、**cell**、**macro/blockage**,还是 site**空**着 |
| `vtAt(r,c)` | 被占用 site 的 implant id | 该实例 master 携带的 **IMPLANT 类型层** → 映射成 VT id(字符串) |
| `clearSite(r,c)` | 删 `(r,c)` 处的 filler,变空 | **删(filler)实例**的能力 |
| `placeFiller(f)` | 在 `f.row/col/width/height` 建一个 VT 为 `f.vt` 的 filler | `(vt,宽,高) → master` 查表,加 **建实例 + 设坐标 + 设朝向**(朝向跟随行);以 physical-only 放置 |

### 7.2 调用方还要给的其它输入
- **filler 库**:`std::vector<Filler>`,每个 `{vt, width(site), height(行),
  name}`,覆盖所有可用 filler master(就是 `placeFiller` 能实例化的那些)。
  宽度种类要够把 run 精确平铺(没有 1 site filler ⇒ 必须精确填满)。
- **`VtRules`**:`min_width`(ωMW)、`min_spacing`(ωMS),单位**site**
  (来自 §4.3 DRC checker / tech 规则)。
- **VT ↔ implant 层映射**:见 §4.4,`vtAt` 和建库都用。
- (可选,定向模式)逐违例 DRC 数据:见 §4.3 第 (2) 项。

### 7.3 适配器掌管的几何(算法不碰)
- core 区原点、 site 宽、行高(DBU ↔ site/行);
- 每行 orientation(R0 / MX …),使新建 filler 对齐 rail / implant;
- `placeFiller` 用的 `(vt, width, height) → master` 映射。

参考实现是 `dpl2/src/FakeFillerGrid.h`(纯内存),单测用它;移植时照它的形状,
把每个方法接到真 DB 即可。

---

## 8. 基础行为详解(site 网格图示)

下面用 site 网格说明前 4 种典型行为。

**记号**(单字符,方便逐列对齐):
- **大写字母 = cell**(固定,算法不能改),字母即 VT:`L` `H`。
- **小写字母 = filler**(可改,算法可换 VT),字母即 VT:`l` `h` `x`。
- `.` = 空 site;`^` 指向出问题的列;行首 `行N:` 对齐到第 0 列。
- 除非特别说明,设 **ωMW = 2**(同 VT 区至少 2 site 宽)、**ωMS = 1**(不同 VT
  不得相邻接触)。

---

### 行为 1 — 跨行 MW 台阶(1 宽对角的同 VT filler)

**场景**:两行,中间 filler 的 VT 是 H(`h`),和 L cell(`L`)排成对角:

```
行0:  L h
行1:  h L
      ^ ^   两个 h(行1 col0、行0 col1)各 1 宽、对角 → MW;且贴 L → MS
```

**为什么是违例(按评估器)**:看 `行0 col1` 的 `h`。
- **水平**同 VT(H)run:左邻 `L`(cell,异 VT)、右边没有 → run = 1。
- **垂直**同 VT(H)run:`行1 col1` 是 `L` → run = 1。
- 两向 run 都 < ωMW(2)→ **窄颈**,MW +1;`行1 col0` 的 `h` 同理再 +1。两个 `h`
  还各自贴着 `L` → MS。直观:两块 H 各只 1 site 大,对角两个孤立点,既不够宽
  (MW)又贴 L(MS)。

**算法怎么修**:两个 `h` 都换成 `l`:

```
行0:  L l
行1:  l L
```

- 每行的 filler 和旁边的 `L` cell **并成一条 2 宽的 L 区** → MW 消失;全图皆 L、
  无异 VT 相邻 → MS 消失。**残留 0**。cell 没动。对应单测 **T1**。

---

### 行为 1B — 跨行交叠颈 MW(case B:上下行同 VT 区交叠太小)

**场景**(本例取 **ωMW = 3** 以显示交叠颈):上下两行各自的 L 区都够宽,但**错开**,
只重叠少数几列。`行0` 的 L = col2-7(宽 6);`行1` 的 L = col6-9(宽 4);
col4-5 是别的 VT 的 filler `x`:

```
      0 1 2 3 4 5 6 7 8 9   <- col
行0:  . . L L L L L L . .
行1:  . . . . x x L L L L
                  ^ ^   上下仅在 col6-7 交叠(桥宽 2)< ωMW=3 → MW(case B)
```

**为什么是违例**:两行的 L 各自水平都够宽,**单看每行都没事**;但作为**同一块
implant**(union),连接上半与下半的「桥」只有 col6-7 两列宽。2 < ωMW(3)→ MW。
- 这正是原模型**漏掉**的:col6-7 在 `行0` 里属于一条 6 宽的水平 run,被「水平
  run ≥ ωMW」误判通过。正确判法看**相邻行的交叠列段宽度**(见 §6 (b))。

**算法怎么修**:把 `行1` 挡在中间的 `x`(col4-5)换成 L(`l`),**加宽交叠**:

```
      0 1 2 3 4 5 6 7 8 9   <- col
行0:  . . L L L L L L . .
行1:  . . . . l l L L L L
              ^ ^ ^ ^   交叠变 col4-7(宽 4)≥ ωMW → 颈消失
```

- `x`→`l` 后 `行1` 的 L 左移到 col4,和 `行0`(col2-7)交叠成 col4-7 共 4 列 ≥ ωMW
  → 跨行颈消失。前提是 col4-5 是**可改 filler** 且库里有 L;若是**固定 cell** 或
  库缺 L → 改不动 → 残留回报上游(同行为 4)。

---

### 行为 2 — 跨行 MS(上下两行 filler 的 VT 不同,垂直相邻)

**场景**:两行 filler 被左右 `L` cell 夹住;上行 `l`、下行 `h`:

```
行0:  L l l L
行1:  L h h L
        ^ ^   行0 的 l 压在 行1 的 h 上(col1、col2)→ 垂直 MS ×2
```

**为什么是违例**:`行0 col1` 的 `l` 与正下方 `行1 col1` 的 `h` 垂直相邻、异 VT
→ MS +1;col2 同理再 +1。直观:上面一整条 L、下面一整条 H,两条**贴着**,中间
「L 碰 H」的水平边界就是间距违例。两条 run 水平都 2 宽,MW 没问题,违例纯是 MS。

**算法怎么修**:下行两个 `h` 换成 `l`(上下都 L,边界消失),即「向上合并」:

```
行0:  L l l L
行1:  L l l L
```

- 垂直相邻处同 VT → MS 消失。**残留 0**。对应单测 **T2**。(也可反过来改上行;
  选哪边由代价函数定,两种都是 1 步。)

---

### 行为 3 — filler 夹在两个**同 VT** 的 cell 中间,但 filler VT 不对

**场景**:一行,中间 filler 是 `h`,左右两个 cell 都是 `L`:

```
行0:  L h L
        ^   col1 的 h:水平 run=1 < ωMW → MW;两侧贴 L → MS ×2
```

**为什么是违例**:中间 `h` 只 1 site 宽(水平 run=1 < ωMW)→ MW 窄颈;左右各贴
一个 `L` cell(异 VT、相邻)→ 2 个 MS。

**算法怎么修**:`h`→`l`,延续左右 cell 的 L:

```
行0:  L l L
```

- 现在 col0-2 连成一条 3 宽 L → MW 合格;全 L → 无 MS。**残留 0**。cell 不动,
  改中间 filler 去**迁就**两边固定的 L,恢复 implant 连续。

---

### 行为 4 — 固定边界:只有动 cell 才能修 → 判残留,回报上游

**场景**:相邻两个 cell 本身就异 VT(中间没有 filler 可调):

```
行0:  L H
      ^ ^   col0(cell L)与 col1(cell H)相邻、异 VT → MS;两者皆 cell → 残留
```

**为什么是违例**:`L`(cell)和 `H`(cell)水平相邻、VT 不同 → MS +1。

**算法怎么做**:这道违例的两边**都是 cell**(固定边界),按 FR-1 算法**不能动
cell**,而它**没有 filler site**可改 → 无能为力。于是它**不修**,把这条违例
计入 `unresolved` **回报上游**(由上游决定是否移动 cell / 改 implant)。

- 这就是「最大化修复、而非强行清零」(FR-3):能用换 filler VT 修的就修
  (行为 1–3),修不了的(行为 4 / 库里缺 VT)如实上报,不会瞎改 cell。
- 推广:即使中间夹了一个 filler,如果无论给它哪种 VT 都至少会留下一处违例
  (例如它一侧是 L cell、另一侧是 H cell,filler 选 L 则右边 MS、选 H 则左边
  MS),代价函数会选「违例最少」的那个,剩下的同样作为残留上报。

---

### 其余情形一览(site 网格)

| # | 情形 | 动作 |
|---|---|---|
| 1B | 跨行交叠颈 MW(上下行同 VT 区交叠 < ωMW) | 改中间 filler 加宽交叠到 ≥ ωMW(见行为 1B;**代码评估器待补**) |
| 5 | 库里没有所需 VT | 该 run 回退 → 残留 → 上游(单测 T3) |
| 6 | 某个 VT 改动会制造**新**违例 | 被代价函数拒绝 → 保持原样 |
| 7 | 必须移动 cell 才能修 | 超范围 → 残留 → 上游(同行为 4) |
| 8 | 本来就干净 | 空操作(改前 0、改后 0;单测 T4) |

---

## 9. 约束与假设

- implant 模型是**site 网格**,不是多边形 DRC。
- **只改 VT、不改占用(occupancy-preserving)**:本方案保持「哪些 site 是 filler」
  不变(始终 100% 填满),只换 filler 的 VT。落地时虽然在实例层 `clearSite`+
  `placeFiller`(删旧建新),但**不留空 site、不动 cell**。
  - 为什么不靠「删 filler 留空隙」来修:① 留空破坏 §3.1 的 100% utility,且会引入
    别的 DRC(base/OD min-area、PG/rail 连续、density);② 单纯**重新切分宽度**
    (同 VT、仍填满)对 MW/MS **毫无影响**——违例只看**逐 site 的 VT**,不看 filler
    实例边界。所以"删+重插"唯一能多修的,是**用空隙间距(spacing)当工具**
    分开两个异 VT 区,这能多修一部分 VT 换不动的 MS/交叠,但前提是 flow 允许留空
    且额外 DRC 被建模——属于**可选的、更强但更冒险的升级层**,不是免费收益
    (见 §11)。
- 重铺改动的 run 时必须精确填满(没有 1 site filler ⇒ 不留残缝);铺不出来的
  run 回退,而不是留一半。
- 单一全局 `min_width` / `min_spacing`(逐 VT 的 ωMW/ωMS、**同一 VT 的 P band 与
  N band 可能不同的 ωMS/ωMW**、以及 ωMS > 1 跨空 site 的情形,都是未来工作)。
  注:这些**在 oracle 模式下由上游 checker 处理**(P/N 经 `ImplantLayer.polarity`
  + `bandSlot`,见 §13.4);此条只针对我们 standalone 的 site 网格评估器。
- MW 评估器**已实现**行内/对角窄条 (a) **和**跨行交叠颈 (b / case B)
  (见 §6 第 2 点、行为 1B、单测 T6)。
- 原型用 **height-1** 的 filler 重铺改动的 run。

---

## 10. 验证(两路径)

**Part A** — `dpl2/test/filler_vt_repair_test.cpp`:**15/15** —
T1 台阶 MW+MS 全修(filler H→L、cell 不动),T2 跨行 MS 合并,T3 库缺 VT → 残留
(零替换),T4 干净空操作,T5 评估器自检,**T6 case-B 交叠颈(错开 vs 对齐)**。

**Part B** — `dpl2/test/filler_vt_repair_oracle_test.cpp`:**12/12** —
O1 台阶 MW+MS 经 oracle 全修,O2 跨行 MS 合并,O3 库缺 VT → 残留(零替换),
O4 干净空操作。用 `FakeImplantChecker` 作内存 oracle。

构建 / 运行(无数据库、无依赖):
```
# Part A
g++ -std=c++17 -I dpl2/src dpl2/src/FillerVtRepair.cpp \
    dpl2/test/filler_vt_repair_test.cpp -o /tmp/vt && /tmp/vt
# Part B
g++ -std=c++17 -I dpl2/src dpl2/src/FillerVtRepair.cpp \
    dpl2/src/FillerVtRepairOracle.cpp \
    dpl2/test/filler_vt_repair_oracle_test.cpp -o /tmp/orc && /tmp/orc
```

---

## 11. 待办(两路径共用)

已完成(本轮):**case-B 评估器**(§6 (b),单测 T6)· **oracle 路径骨架**
(`ImplantChecker` + `FillerVtRepairOracle` + `FakeImplantChecker`,12/12)。

- **【首要】把 `ImplantChecker` 接到真实 `ImplantLayerChecker`**:实现一个适配器
  把 §13.7 的方法绑到 `checkDirect/checkPlace/commitPlace`。先和 RD 敲定 §13.8 的
  7 个问题(尤其**如何删除/替换已提交 filler 实例**——阻塞项)。
- standalone(Part A)评估器仍缺、但 oracle 模式由 checker 覆盖的:
  - **P/N band 各异的 ωMS/ωMW**:规则改成 `(层, band) → (ωMW, ωMS)`,按 site 的
    P/N band 取阈值。
  - 逐 VT 的 ωMW/ωMS、ωMS > 1(跨空 site)。
- Tier-2 列 DP 做局部最优 MW/MS(替换 §6 / §13.2 的贪心,不动 oracle 接缝)。
- **可选「删+重插」升级层**:对 VT 换不动的残留 MS/交叠,尝试用空隙间距分开异 VT
  区(carve 一个 ≥ ωMS 的空 gap)。仅在 flow 允许留空、且额外 DRC(min-area/PG/
  density)被建模时启用;否则保持 occupancy-preserving(见 §9)。
- 定向修复模式(消费 §4.3 第 (2) 项 DRC 数据),取代整图扫描。
- 落地时支持多行高 filler 重铺(当前是 height-1)。

---

## 12. 附录:论文式最优算法(DP)、可实现性、最完备上游输出

**DP = 动态规划(dynamic programming)**:一种求**全局最优**的算法范式——把问题
沿一个方向(这里是「逐列」)拆成子问题,每步只记住一个紧凑「状态」下的最优代价
并递推,最后回溯出全局最优。和 §6 的**贪心**(走一步看一步、可能卡局部最优)相对:
DP 系统地枚举所有状态,保证不漏掉更优解。**当前代码用的是贪心,不是 DP**;本节
是把 DP 作为未来的「最优」升级方向来说明。

> 诚实声明:本节描述的是这类「最优 filler/VT 插入」论文(我引用的
> "Toward Optimal Filler Cell Insertion" 一类)所采用的**动态规划(DP)**思路,
> 是按问题结构重建的、可落地的表述;**不是**逐字复述某篇论文的公式/定理编号。
> 若你给我论文 PDF,我会把下面的状态/转移与论文的 Algorithm 4 逐条对齐、
> 修正任何差异。当前代码(§6 的贪心)**并未**实现本节的 DP。

### 12.1 问题(与 §6 同一目标,但要求全局最优)

在「cell 位置与 VT 固定、空隙必须用库里 filler 填满」的前提下,给每个 filler
site 选一个 VT(并选库里能精确平铺的宽度),**最小化** implant 的 MW/MS 代价:
- **MW**:每条极大同 VT 区(水平或垂直)的宽度 ≥ ωMW;
- **MS**:不同 VT 区之间满足 ωMS(本模型里 = 不相邻接触);
- **可实现**:每段 filler run 的宽度能被该 VT 的库 master 精确平铺。
贪心(§6)只能到局部最优;DP 的价值是在一个**窗口**内给出**全局最优**的 VT 标注。

### 12.2 单行 DP(1D,严格最优、明确可实现)

把一行从左到右扫,在每个 site 决定它的 VT。
- **状态** `dp[c][v][k]`:处理到第 `c` 列、该列 VT = `v`、当前同 VT 连续 run 已达
  长度 `k`(`k` 在 `min(实际, ωMW)` 处封顶,因为超过 ωMW 后再长对 MW 无差别)。
- **转移**:`c → c+1`。若 `c+1` 是 cell,VT 被钉死(只能取该 cell 的 VT);若是
  filler,可取库里任一 VT。同 VT 则 `k+1`(封顶);换 VT 时:对刚结束的那条 run
  检查 `k ≥ ωMW`(否则计 MW 罚分),并对这条「VT 边界」计 MS 罚分。
- **可实现性约束**:只允许在「两侧宽度都能被库精确平铺」的列上发生 VT 边界
  (可预计算每个 VT 的可平铺宽度集合,转移时查表)。
- **复杂度**:`O(C · |VT| · ωMW)`,一行线性,**最优**。这部分实现起来没有难点。

### 12.3 多行 DP(2D,跨行 MW/MS——本模块真正要的)

跨行 MW(垂直 run)和跨行 MS(上下相邻异 VT)把各行耦合起来,所以要按**列**推进、
状态带上**整列的 VT 切片**:
- **状态** `dp[c][(v_0,…,v_{h-1})][(k_0,…,k_{h-1})]`:第 `c` 列上,`h` 行各自的 VT
  和各自的「水平 run 长度」。
- **转移** `c → c+1`:对每一行按 1D 规则更新水平 run;**列内**相邻两行 VT 不同 →
  记一次跨行 MS;垂直 MW 用「跨列保持的垂直 run」另行累计(可在状态里再带一个
  每行的垂直 run 计数,或在窗口内后验)。
- **复杂度**:`O(C · |VT|^h · …)`——对**行数 `h` 指数**。所以只能用于**小窗口**
  (几行 × 违例邻域那么宽),这正是「推广到几行」的含义,也是为什么需要 §4.3 的
  **逐行 region** 把 DP 限制在违例附近。
- 全片任意高度做精确 DP 不可行 → 必须开窗(window),窗口边界用固定 cell/已定
  filler 作为 DP 的边界条件。

### 12.4 可实现性结论

- **1D 单行 DP**:可实现、低成本、最优。**建议先做**,直接替换 §6 贪心在单行上的部分。
- **2D 小窗口 DP**:可实现,但状态随行数指数增长。对 `h ≤ 3~4`、VT 种类不多时实用;
  关键是**靠 §4.3 的违例 region 开小窗**,而不是整图跑。需要解决三个工程点:
  ① 把「库可平铺宽度」折进转移;② 垂直 run 的 MW 计数方式(状态 vs 后验);
  ③ 窗口边界条件(把窗口外的固定 VT 作为转移约束)。
- 这些都不依赖改动现有接口(§7);DP 仍然只通过 `FillerGrid` 读、用
  `clearSite`/`placeFiller` 落地。**所以是纯算法升级,接口不变。**

### 12.5 DP 要的「最完备上游输出」(比 §4.3 更严格)

贪心(§6)最少只要规则阈值 + 网格;但要把 DP 喂成**全局最优**,上游(DB + DRC
checker)最好给齐下面这些——这是「最完备」清单:

1. **逐 site 的精确标注**(来自 DB,§4.1):每个 site 的 kind 和 VT,且保证
   **100% utility**(§3.1;DP 假设无空隙,否则状态要再加「空」一档并定义跨空
   MS)。
2. **完整规则**:`min_width`、`min_spacing`,且**逐 implant 层**给(`层 →
   (ωMW, ωMS)`);若 `ωMS > 1`(隔空也要间距),DP 必须建模空隙 → 见第 1 点。
3. **每个 VT 的 filler 库宽度集合**(来自库,§4.2):用于在转移里强制「VT 边界
   只发生在可精确平铺处」。没有这个,DP 的最优解可能落不了地。
4. **DP 窗口范围**(来自 DRC region,§4.3 第 (2) 项):违例的**逐行列区间**
   `[(row, col_lo, col_hi)…]` + 一圈 margin,决定对哪几行哪几列联合优化。这是把
   指数级 2D DP 压到可行的关键输入。
5. **窗口边界条件**:窗口外紧邻的固定 VT(cell 或已定 filler)——作为 DP 两端的
   钉死状态,保证窗口内最优解和窗口外拼接后仍合法。
6. **(可选)代价权重 / violation 软硬**:若某些违例是硬约束(必须清)、某些是软
   (尽量),给每类一个权重,DP 的目标函数据此加权;否则默认全部等权计数。
7. **`fixable_by_filler` 粗筛**(§4.3):标 false 的(要动 cell,case B)直接排除出
   DP 窗口,避免在无解区域上浪费指数级搜索。

> 一句话:**1D 最优现在就能做;2D 最优能做、但必须靠 DRC 给的逐行 region 开小窗,
> 并把「库可平铺宽度 + 逐层规则 + 窗口边界」都喂齐**,否则只能退回 §6 的贪心近似。

---

> ━━━ **Part B — 生产集成(接 ImplantLayerChecker oracle,推荐路径)** ━━━

## 13. 对接上游 checker(ecoPlace Implant Layer Checker)(Part B)

> 依据 checker RD 给的类图(`ImplantLayerCheckerHelper.h` + `ImplantLayerChecker.h`)。
> 它**不只是给数据**——本身是一个**增量 check / commit 引擎**,这改变了我们的
> 集成方式:不必自造 MW/MS 评估器,把它当**代价 oracle**用。
>
> **本节已落代码(骨架)**:抽象接口 `dpl2/src/ImplantChecker.h`(把下面的 API
> 抽象成 portable seam)+ 决策层 `dpl2/src/FillerVtRepairOracle.{h,cpp}`(贪心搜索)
> + 内存桩 `dpl2/src/FakeImplantChecker.h` + 测试 `filler_vt_repair_oracle_test.cpp`
> (12/12)。接真实 checker = 实现 `ImplantChecker` 的一个适配器(§13.7)。

### 13.1 它是什么 —— 增量放置合法性 oracle

`ImplantLayerChecker` 的公开 API:
- `initialize(ImplantInput): bool` —— 从 `rules / layers / groups / masters /
  placedInsts / rows / tracks / rowHeight / siteWidth` 建内部索引。
- `checkPlace(CheckRequest): CheckResult` —— **假设**把某 `masterId` 放到
  `(rowId, x, orientation)`,返回 `{isLegal, violations[], diagnostics[]}`,**不提交**。
- `checkDirect(CheckRequest): CheckResult` —— 直接扫描版(ground truth,较慢)。
- `commitPlace(CommitRequest): UpdateResult` —— **真正提交**放置,更新内部状态。
- 辅助:`dump/load(filePath)`(序列化)、`placedInsts()`、`siteWidth()`、
  `mergedShapeCount()`、`initDiagnostics()`。
- 内部流程(类图标注):`initialize → checkPlace / checkDirect → commitPlace`。

### 13.2 集成架构 —— checker 当 oracle,我们当 decision / search 层

```
initialize(input)                          // 一次
cur = checkDirect(region) / initDiagnostics()   // 当前违例(含 xWindow)
对每个违例窗口(xWindow × 涉及 rows):
  对窗口内每个候选 filler-master 替换:
     res = checkPlace(candidate)           // 合法? 违例多少?(oracle)
  以 res.violations 作代价,用贪心 / DP 选最优组合
  commitPlace(best)                        // 落地;并镜像到真实 DB
```

- 我们的 `countViolations`(site 网格启发式)在**生产中由 `checkPlace` /
  `checkDirect` 取代**,只保留作无依赖单测的桩。
- 我们的核心价值从「评估违例」变成「**决定换哪些 VT/master、怎么搜、怎么开窗**」。

### 13.3 数据模型对照(他们的结构 → 我们怎么用)

| checker 结构 | 关键字段 | 我们怎么用 |
|---|---|---|
| `ImplantInput` | layers/rules/groups/masters/placedInsts/rows/tracks/rowHeight/siteWidth | `initialize` 的全部输入;我们建窗口/库都从这里取 |
| `Rule` | ruleId, source, primaryLayer, **secondaryLayer**, **minValue**, **direction**, prl, zeroPrl, exceptAbutted, exceptCornerTouch, checkGroup, intersectLayers, containment… | MW = 单层(只有 primaryLayer)宽度;MS = primaryLayer↔secondaryLayer 间距;`minValue` = 阈值(DBU);其余(prl/abut/containment)是我们 site 模型没有的更细规则 → **交给 oracle** |
| `ImplantLayer` | id, name, family, **polarity** | VT = LayerId;`polarity` 即 P/N → 解决我们的 P/N spacing 顾虑(§13.4) |
| `MasterInput` | masterId, width, height, **shapes**, **isFiller** | filler 库 = `isFiller==true` 的 master;宽度 = width/siteWidth;implant 几何 = shapes |
| `MasterShape` | masterId, shapeId, layer, **rect** | 一个 master 可在多层有多块 implant rect(比"每 filler 一个 VT"更细) |
| `PlacedInst` | instanceId, masterId, rowId, columnId, orientation, isFiller | 当前布局;我们的 `kindAt/vtAt` 从它 + masters 推 |
| `TrackPattern` | layerBySlot, **activeKindByBoundary**, layerForSlot, **adjacentSlots**, **activeInterRowKind** | band/slot 模型 + 跨行相邻种类 → P/N band 与跨行邻接都在这 |
| `Violation` | ruleId, primaryLayer, secondaryLayer, **instances**, shapeIds, mergedShapeIds, **measuredValue**, **requiredValue**, **xWindow**, relationship, status | 每条违例:`xWindow`(XInterval)+ `instances`(涉及实例/行)就是**开窗依据**;measured/required 给代价 |
| `CheckResult` / `UpdateResult` | isLegal, violations, diagnostics / success, diagnostics | checkPlace 的返回 = 我们的代价 oracle |

### 13.4 这些"待办"现在由 checker 解决(oracle 模式下)

- **P/N spacing 不同**(我们 §4.3/§9 的顾虑):`ImplantLayer.polarity` + interval 的
  `bandSlot` + `TrackPattern.activeInterRowKind / adjacentSlots`。规则按 layer
  (带 polarity)定,P/N 自动区分 → **不必我们自己建模**。
- **case-B 跨行交叠颈**(我们 §6(b) 的代码缺口):checker 用真实 shape 合并
  (`MergedShape`)+ `Rule.direction/minValue` 判 min-width,**天然含跨行交叠** →
  oracle 模式下不再是问题(仅我们 standalone 启发式评估器仍缺这条)。
- **PRL / exceptAbutted / exceptCornerTouch / containment / intersectLayers**:都是
  `Rule` 字段,oracle 内部处理;我们的 site 网格模型只是其粗略子集。

### 13.5 我们仍然拥有的(checker 不替我们做)

- **搜索 / 决策**:试哪些 VT/master、贪心还是 DP、怎么开窗——核心价值。
- **filler master 选择 + exact-fill**:在 `isFiller` master 里选,凑满宽度。
- **DB ↔ checker 状态同步**:`commitPlace` 后同步真实 DB(反之亦然)。
- **100% utility 前置门**(§3.1)与可选"删+重插"升级层(§9/§11)。

### 13.6 坐标 / 单位映射(他们 DBU·x → 我们 site·row)

- `siteWidth` / `rowHeight` 已在 `ImplantInput`;site 列 `c` ↔ `x = 行原点 +
  c*siteWidth`;`rowId` ↔ 我们的行号。
- **窗口**:`Violation.xWindow`(XInterval, DBU)→ 列范围 = `xWindow / siteWidth`;
  涉及行从 `Violation.instances` 的 `rowId` 得到。→ **取代**我之前建议的「逐行列
  区间」字段:直接用 `xWindow + 涉及 rows`(再按 §"开窗"扩到最近 cell + 上下 1 行)。
- 所有 `DbCoord` = DBU;`measuredValue/requiredValue/minValue` 都是 DBU。

### 13.7 接口绑定:我们的 `ImplantChecker` ↔ checker API

我们已把 oracle 抽象成 `dpl2/src/ImplantChecker.h` 的 5 个方法;接真实 checker =
实现一个适配器把这 5 个方法绑到 `ImplantLayerChecker`:

| `ImplantChecker`(我们的接口) | 绑定到真实 checker |
|---|---|
| `violations()` | `checkDirect(...)` 在管辖区域上的违例计数 |
| `changeableRuns()` | 从 `placedInsts()`(`isFiller`)+ `masters` 推出可改 filler run |
| `candidateVts()` | 从 `ImplantInput.masters`(`isFiller`)的 implant 层推出 |
| `evalReplaceRun(run, vt)` | `checkPlace(CheckRequest{...})` 评估「该 run 改 vt」(不提交) |
| `commitReplaceRun(run, vt)` | `removeInstance(...)` + `commitPlace(CommitRequest{...})` |

> 决策层 `FillerVtRepairOracle` 只依赖上面 5 个方法,完全不碰 DB / 真实 checker;
> 换后端只换一个 `ImplantChecker` 实现(测试用 `FakeImplantChecker`,生产用真实
> 适配器)。`commitReplaceRun` 依赖**公开的 remove**——见 §13.8 第 1 条(阻塞项)。

### 13.8 要和 checker RD 确认的点

1. **如何删除 / 替换已提交的 filler 实例**?VT 替换 = 删旧 master + 放新 master;
   `removeInstance` 在类图里是 private——需要公开 remove,或 `commitPlace` 支持
   同 `instanceId` 覆盖。**这是阻塞我们落地的第一问题。**
2. `checkPlace` 返回的 `violations` 是「放置后该实例牵涉的**全部**违例」还是仅
   「**新增**违例」?我们要按**净违例变化**做代价,需明确语义。
3. `checkDirect` 能否对**任意区域**做全量真值扫描(我们初始拿全图/窗口违例用)?
4. `Violation.relationship` / `status` 的取值集合;`xWindow` 是否已含 PRL 投影。
5. `PhysOrientation` 与我们行 R0/MX 的对应;filler master 在某行的合法 orientation。
6. **占用 / 100% utility**:checker 是否假设满填?留空 site 它如何判(回应我们
   §3.1 与"删+重插"升级层)。
7. `dump/load` 是否够我们做"试多个候选→回滚"的快照,还是必须靠 `checkPlace`
   的非提交语义(更可取)。

### 13.9 「只拿一次违例数据」够不够?要哪种调用能力?

这是和 RD 对齐时最该说清的一点:**一次性的全部 violation 数据,不足以驱动修复。**

- violation 快照(`xWindow + instances + layer + measured/required`)够**定位 /
  开窗**(哪里坏、动哪些 filler、窗口多大),但它只描述**当前**状态。我们每改一个
  filler 的 VT,违例就变,快照立刻**过期**。修复是搜索:每个候选改动都要问「改完
  还剩几个违例、会不会造新违例」——静态快照答不了。
- 「改后重评估」只有两条路:
  1. **再调用他们的 checker**(本节推荐):`checkPlace`(假设放置→违例,**不提交**,
     增量、快)最理想;只有全量 `checkDirect`(重扫整图)也能用,但每个候选都要
     重扫一遍,慢。
  2. 自己重写评估器(Part A)——MW/MS 的**粗略子集**,漏 P/N、PRL、abutment、
     containment。
- 因此**「我们就调用他们的 checker」正是 Part B 的做法,且比自评估准**。能否「只
  调用」取决于 checker 提供哪种能力:

  | checker 能提供 | 够不够 | 说明 |
  |---|---|---|
  | `checkPlace` 增量假设评估(放一下→违例,不提交) | ✅ 最佳 | 每候选 O(局部);我们 oracle 直接用 |
  | 仅全量 `checkDirect` 重扫整图 | ⚠️ 能用但慢 | 每候选重扫一次 → 必须控候选数 / 开小窗 |
  | 仅一次性违例 dump、之后不能再调 | ❌ 不够搜索 | 只能退回 Part A 自评估 |

- 结论:**请 RD 提供「改动后可重新评估」的调用**(最好是非提交的 `checkPlace`),
  而不是一次性导出违例;落地仍需 commit + remove / replace(§13.8 第 1 条)。

### 13.10 能否让 checker 判「一条违例能否靠替换 filler VT 修」?

把「filler-VT 可修」拆成必要 / 充分两层:

- **必要条件 —— checker 能直接给**:违例里**需要改动的那一侧,至少有一个 filler**
  (可改),而非两侧全是 cell/blockage。这从 `Violation.instances` + 每个
  `PlacedInst.isFiller` 一步得到;checker 已有全部信息,能**可靠排除**「两侧都是
  cell → 必须动 cell(case B / LEF Fig 3-1 "B")」那类我们修不了的。
  - MS:两个相碰 implant 区,至少一侧 owner 是 filler → 可能可修;两侧都 cell →
    不可修。
  - MW / case-B 交叠颈:要加宽的窄区 / 要补的交叠列里**含可改 filler** → 可能可修;
    全是某 cell 的 implant → 不可修。
- **充分条件 —— checker 单独给不了**:保证「**存在**一个 VT 赋值清掉违例、**且不造
  新违例、且库可精确平铺**」做不到,因为 ① checker 不知道我们的 filler 库可实现性
  (exact-tiling、无 1-site filler);② 「清掉且不造新违例」是对候选的**搜索**,
  不是当前违例的静态属性。只能靠**逐候选 `checkPlace` 重评估 + 我们的库检查**来定。
- **结论 / 分工**:
  - 让 checker 给一个 sound+complete 的「可修」布尔**不现实**;它能给的是**必要
    条件粗筛**。
  - **checker**:给 `fixable_by_filler`(**定义为必要非充分**:有可改 filler 参与),
    或直接暴露每个 participant 的 `isFiller`,我们自己算 —— 便宜地砍掉 case B。
  - **我们**:用 `checkPlace` 搜索**证明并达成**真正可修(找一个合法且库可实现的
    VT 赋值)。
- **给 RD**:别要万能「可修」标志;要 ① 每个 participant 的 `isFiller` ② 非提交的
  `checkPlace`。

#### 13.10.1 接口确认(本轮结论)

「在 `Violation` 加 `bool fixable_by_filler` + 全量违例传给我们」——方向正确,
落实时收紧三点:

1. **判据按「窗口」而非「直接 participant」**:`fixable_by_filler` 应判「违例窗口
   (`xWindow` + 上下相邻行那一圈)内**有没有可改 filler**」,不能只看 participant
   列表——否则要靠**邻近** filler 才能修的会被误判 `false`(漏修)。最典型是 case-B
   交叠颈:要改的中间 filler 常不在 participant 里,却在窗口里。
   - 退一步最稳:checker 把**每个 participant 的 `isFiller`** 都给我们(它本就有),
     这个 bool 可给可不给,我们能自己按窗口算。
2. **必要非充分**:`true` = 「值得试」,不是「一定修得好」;`false` 才是可靠的
   「跳过(case B)」。真正可修性由我们 `checkPlace` 验证。
3. **全量 violation 是起点、不是全部**:全量传用于定位 / 开窗;但快照一改就过期
   (§13.9),接口还必须有**非提交的 `checkPlace`** 供搜索时重评估。两者缺一不可。
