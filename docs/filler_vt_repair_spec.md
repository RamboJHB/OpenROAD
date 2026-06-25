# 功能规格 — Filler VT 修复(跨行 MW/MS,只替换 VT)

状态:原型已实现并测试通过(13/13)。最后更新 2026-06。
本文档范围:**只替换 filler VT** 这一套修复方案。它与数据库无关,移植时
只需实现一个接口(见 §7)。

---

## 1. 目的

布线后,优化(`opto`)和 ECO 合法化会扰动版图、破坏 filler,在相邻行之间
产生 **implant 层 DRC 违例**。本模块修复其中的 **跨行最小宽度(MW)** 和
**最小间距(MS)** 违例,手段是 **替换 filler 的 VT(implant 类型)**——
它**绝不**移动/缩放/删除标准 cell,也**绝不**移动 filler;只是把某个 filler
就地换成另一种 implant 类型的 filler。

核心直觉:**filler 站点 = 可自由改的 implant 变量,cell 站点 = 固定的
implant 边界**。在「cell 固定」的前提下,给每个 filler 站点选一个 implant
类型,使整张图的 MW/MS 违例数最少,然后用「删旧 filler + 在原位建新 VT 的
filler」把这个选择落地。

非目标:这**不是**foundry 级的多边形 DRC。它工作在 **站点网格(site-grid)
implant 模型** 上(每个被占用的站点带一个 VT);精确 DRC 在上游。

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
  含**跨行**情形,用站点网格建模。
- 修复动作:**替换 filler 的 VT**(删旧 filler,在相同站点建一个目标 implant
  类型的新 filler)。cell / macro / blockage 是固定边界。

不做(out of scope,→ 作为残留回报上游):
- 移动 / 缩放 / 删除标准 cell;
- 移动 filler 或改变「哪些站点被占用」这件事;
- min-area 等其它 implant 规则;
- DRC 检查与违例标记本身。

---

## 4. 输入 / 输出

本模块的输入来自四个来源:**数据库**、**filler 库**、**DRC checker / tech
规则**、**适配器(几何)**。下面 §4.1–§4.4 逐一说明;§4.5 是输出。

### 4.1 来自数据库(通过 `FillerGrid` 接口,见 §7)
- 行 / 站点结构:`numRows()`、`numCols(row)`。
- 每个站点的**类别** `kindAt`:`Cell` / `CleanFiller` / `Empty` / `Blocked`。
- 每个被占用站点的 **VT** `vtAt`:该站点 master 携带的 implant 层身份。

### 4.2 来自 filler 库
- 每个可用 filler master:`{vt, width(站点数), height(行数), name}`。
- 关键约束:某个 VT 在某段宽度上「可实现」,当且仅当库里能用该 VT 的 master
  **精确平铺**这段宽度。生产库通常**没有 1 站点宽的 filler**,所以必须精确
  填满,不能留残缝(原型用 height-1 的 filler 重铺改动的 run)。

### 4.3 来自 DRC checker / tech 规则(**本节是要和 DRC 团队对齐的接口**)

本模块是 filler DRC 检查的**下游**,需要从那边拿到下面这些数据:

1. **规则阈值(必需)** — 直接决定评估器判违例的标准:
   - `min_width`(ωMW):同一 implant 层允许的最小宽度(单位:**站点数**)。
   - `min_spacing`(ωMS):不同 implant 区之间的最小间距(单位:**站点数**;
     `≥1` 表示不同 VT 不得相邻接触)。
   - 若**每个 implant 层规则不同**,给一张表:`层名 → (ωMW, ωMS)`。
   - 这些值本质来自 tech LEF / rule deck,DRC checker 已在使用;本模块需要拿到
     **换算成站点数**后的值(或拿到原始 DBU 值 + 由适配器换算)。

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
     的形状不是矩形,只有逐行 `[col_lo, col_hi)` 才能精确圈出要动的 filler 站点。
   - `participants` 给出违例**两侧**各自的 owner 实例 + 类型,用来 ① 确认这是
     「filler 对 filler / filler 对 cell」(从而知道哪侧可动),② 在站点网格上
     **定位**要替换的 filler 实例。
   - `fixable_by_filler` 是 checker 的**粗筛**:它先判断「这条违例靠只动 filler
     能不能修」——可修的(对应行为 A/C/D/E,即本规格 §8 的行为 1–3 之类)交给
     本模块;只能靠**动 cell**才能修的(对应 case B,LEF Fig 3-1 的 "B" 情形)
     直接标 false,本模块跳过、不必重算,直接当残留转交需要动 cell 的上游。

3. **隐含契约**:违例已由 DRC checker 圈定。当前 Tier-1 实现是**整图扫描**,
   严格说只依赖第 (1) 项阈值;第 (2) 项违例列表是给**未来的定向修复模式 /
   加速**用的,但先写进接口,方便 DRC 团队一次对齐。

> 小结:**最小可用集 = 只要第 (1) 项规则阈值**(加上 §4.1 的网格)即可跑起来;
> 第 (2) 项违例列表让本模块能「只在违例附近动 filler」,规模大时更快、更可控。

### 4.4 来自适配器(几何,算法本身不碰)
- core 区原点、site 宽、行高(DBU ↔ 站点/行 的换算)。
- 每行 orientation(R0 / MX …),使新建 filler 的 rail / implant 对齐。
- `(vt, width, height) → master` 映射,供 `placeFiller` 建实例用。
- **VT ↔ implant 层映射**:把 implant 层名映射成 VT id,`vtAt` 和建库都用它。

### 4.5 输出 / 副作用
- 数据库被修改:被选中改 VT 的 filler 被删除,在原位重建为新 VT 的 filler
  (physical-only,orientation 跟随所在行)。
- 一条 `VtRepairResult` 记录:`violations_before`、`violations_after`、
  `replaced`(被重建的 filler 列表)、`unresolved`(= `violations_after`,
  即残留,回报上游)。

---

## 5. 功能需求

- **FR-1 只动 filler**:绝不移动/缩放/删除 cell、macro、blockage,也绝不移动
  filler。cell 是固定的 VT 边界;只有 filler 站点的 implant 类型可以改。
- **FR-2 MW/MS 目标**:选 filler 的 VT,使整张站点网格的 MW + MS 违例总数
  **最小**。
- **FR-3 最大化修复,而非要么全清要么失败**:目标是**最小化残留**违例,不是
  「必须清零否则报错」。残留(库里没有所需 VT、被 cell 卡住、或改了会制造新
  违例)→ 如实回报。
- **FR-4 可实现性**:给某段 filler run 选的 VT 必须能被库里的 master **精确
  平铺**;否则这段 run 回退到原 VT(计入残留)。不允许部分填 / 留残缝。

---

## 6. 算法 — `FillerVtRepair`

1. **读网格** 到三层站点网格:`vt[r][c]`、`present[r][c]`(`Cell | CleanFiller`)、
   `filler[r][c]`(可改 == `CleanFiller`)。另存 `orig` = 原始 VT。
2. **2D MW/MS 评估器** `countViolations(vt, present, rules)`:
   - **MW**:一个被占用站点「合格」当且仅当它所在的同 VT **水平 run 或
     垂直 run** ≥ ωMW;否则它是一个**窄颈** → +1(这能抓到「单行看都没事、
     实际是 1 宽跨行台阶」的情形)。
   - **MS**:两个正交相邻(右 / 下)的被占用站点若 **VT 不同** → +1(当
     ωMS ≥ 1,不同 implant 区不得接触)。只看右邻和下邻,避免重复计数。
3. **贪心坐标下降** 遍历 filler 站点:对每个 filler 站点,试遍库里所有 VT,
   保留使复合代价 `违例数*1000 − 同VT相邻数` 最小的那个。第二项(合并项)用来
   打破「单站点平台」——有些改动只有和邻居一起改才划算,合并项给它一个方向。
   迭代到不动点(有轮数上限)。
4. **落地**:对每段「VT 变了的」极大同 VT filler run,用该 VT 的库 master
   `exactFill` 精确平铺该宽度 → `clearSite` 删旧 filler、`placeFiller` 建新的。
   铺不出来的 run **回退**(计入残留)。
5. `violations_after` = 残留;回报。

> Tier-2(未来):列 DP(论文 "Toward Optimal Filler Cell Insertion" 的
> Algorithm 4,推广到几行)做局部最优分配。当前贪心已能覆盖台阶 MW 和跨行 MS。

---

## 7. 接口与数据模型(可移植)— 新数据库要提供什么

算法只通过抽象接口 `FillerGrid`(`dpl2/src/FillerGrid.h`)碰数据库。移植 =
针对新数据库实现这**一个类**。坐标全是**站点 / 行**,从不是 DBU;适配器掌管
DBU 几何和 `(vt, width, height) → master` 映射。

### 7.1 `FillerGrid` 接口(6 个方法)

```cpp
class FillerGrid {
 public:
  virtual ~FillerGrid() = default;
  // ---- 读 ----
  virtual int      numRows() const = 0;                  // 行数
  virtual int      numCols(int row) const = 0;           // 该行的站点数
  virtual SiteKind kindAt(int row, int col) const = 0;   // Empty/Cell/CleanFiller/Blocked
  virtual Vt       vtAt(int row, int col) const = 0;     // 被占用站点的 implant id
  // ---- 写 ----
  virtual void clearSite(int row, int col) = 0;          // 删这里的 filler -> 变 Empty
  virtual void placeFiller(const PlacedFiller& f) = 0;   // 建一个 filler 实例
};
```

每个方法要做什么、以及实现它需要新数据库暴露什么:

| 方法 | 返回 / 行为 | 实现它需要新 DB 暴露的数据 |
|---|---|---|
| `numRows()` | 行数 | core 区的 placement 行 |
| `numCols(row)` | 该行站点数 | 行宽 / site 宽 |
| `kindAt(r,c)` | 站点分类 | 覆盖该站点的实例:是 **filler**(等价 CORE SPACER)、**cell**、**macro/blockage**,还是站点**空**着 |
| `vtAt(r,c)` | 被占用站点的 implant id | 该实例 master 携带的 **IMPLANT 类型层** → 映射成 VT id(字符串) |
| `clearSite(r,c)` | 删 `(r,c)` 处的 filler,变空 | **删(filler)实例**的能力 |
| `placeFiller(f)` | 在 `f.row/col/width/height` 建一个 VT 为 `f.vt` 的 filler | `(vt,宽,高) → master` 查表,加 **建实例 + 设坐标 + 设朝向**(朝向跟随行);以 physical-only 放置 |

### 7.2 调用方还要给的其它输入
- **filler 库**:`std::vector<Filler>`,每个 `{vt, width(站点), height(行),
  name}`,覆盖所有可用 filler master(就是 `placeFiller` 能实例化的那些)。
  宽度种类要够把 run 精确平铺(没有 1 站点 filler ⇒ 必须精确填满)。
- **`VtRules`**:`min_width`(ωMW)、`min_spacing`(ωMS),单位**站点**
  (来自 §4.3 DRC checker / tech 规则)。
- **VT ↔ implant 层映射**:见 §4.4,`vtAt` 和建库都用。
- (可选,定向模式)逐违例 DRC 数据:见 §4.3 第 (2) 项。

### 7.3 适配器掌管的几何(算法不碰)
- core 区原点、site 宽、行高(DBU ↔ 站点/行);
- 每行 orientation(R0 / MX …),使新建 filler 对齐 rail / implant;
- `placeFiller` 用的 `(vt, width, height) → master` 映射。

参考实现是 `dpl2/src/FakeFillerGrid.h`(纯内存),单测用它;移植时照它的形状,
把每个方法接到真 DB 即可。

---

## 8. 基础行为详解(站点网格图示)

下面用站点网格说明前 4 种典型行为。图例:
- `C:L` = 一个 **cell**,VT 为 L(**固定**,算法不能改)。
- `f:H` = 一个 **filler**,VT 为 H(**可改**,算法可换它的 VT)。
- `.` = 空站点(没有 implant)。
- 设 ωMW = 2(同 VT 区至少 2 站点宽)、ωMS = 1(不同 VT 不得相邻接触)。

---

### 行为 1 — 跨行 MW 台阶(1 宽对角的同 VT filler)

**场景**:两行,filler 的 VT 是 H,排成对角:

```
        列0     列1
行0:   C:L     f:H
行1:   f:H     C:L
```

**为什么是违例(按评估器)**:看 `行0 列1` 这个 `f:H`。
- 它的**水平**同 VT(H)run:左邻是 `C:L`(不同 VT),右边没有 → run 长度 = 1。
- 它的**垂直**同 VT(H)run:上下都不是 H(行1 列1 是 `C:L`)→ run 长度 = 1。
- 水平和垂直 run 都 `< ωMW(2)` → 这是个**窄颈**,MW +1。`行1 列0` 的 `f:H`
  同理,再 +1。另外两个 H 还和相邻的 L cell 接触 → 同时有 MS。
- 直观理解:两块 H implant 各自只有 1 个站点大,像对角线上两个孤立小点,
  既不够宽(MW),又贴着 L(MS)。

**算法怎么修**:把两个 `f:H` 都改成 `f:L`。

```
        列0     列1
行0:   C:L     f:L      ← 行0 现在是 L L,水平 run = 2 ≥ ωMW,合格
行1:   f:L     C:L      ← 行1 现在是 L L,水平 run = 2 ≥ ωMW,合格
```

- 改成 L 后,每行的 filler 和旁边的 L cell **并成一条 ≥2 宽的 L 区** → MW 消失;
- 全图只剩 L,没有异 VT 相邻 → MS 也消失。**残留 0**。
- cell 始终没动(仍是 L)。这正是单测 **T1** 的结果。

---

### 行为 2 — 跨行 MS(上下两行 filler 的 VT 不同,垂直相邻)

**场景**:两行 filler,被左右的 L cell 夹住;上行是 L、下行是 H:

```
        列0     列1     列2     列3
行0:   C:L     f:L     f:L     C:L
行1:   C:L     f:H     f:H     C:L
```

**为什么是违例**:看 `行0 列1` 的 `f:L` 和它**正下方** `行1 列1` 的 `f:H`。
- 两者**垂直相邻**,但 VT 不同(L vs H)→ MS +1。`列2` 同理再 +1。
- 直观理解:上面一整条 L、下面一整条 H,两条**贴着**,中间这道「L 碰 H」的
  水平边界就是间距违例(不同 implant 区不许接触)。
- (此例里每条 run 水平都有 2 宽,所以 MW 没问题,违例纯是 MS。)

**算法怎么修**:把下行的两个 `f:H` 改成 `f:L`(因为上下都成 L 就不再有边界),
即「向上合并」:

```
        列0     列1     列2     列3
行0:   C:L     f:L     f:L     C:L
行1:   C:L     f:L     f:L     C:L     ← H→L,上下同 VT,边界消失
```

- 上下两行都是 L,垂直相邻处 VT 相同 → MS 消失。**残留 0**。这是单测 **T2**。
- 注意算法也可以反过来把上行改成 H;选哪边由代价函数定(这里改一行就够,
  两种都是 1 步,合并项让它落到一个稳定解)。

---

### 行为 3 — filler 夹在两个**同 VT** 的 cell 中间,但 filler VT 不对

**场景**:一行,中间的 filler 是 H,但左右两个 cell 都是 L:

```
        列0     列1     列2
行0:   C:L     f:H     C:L
```

**为什么是违例**:中间这块 H 只有 1 站点宽(水平 run = 1 `< ωMW`)→ MW 窄颈;
而且它左右各贴着一个 L cell(VT 不同、相邻)→ 两个 MS。一块「夹在 L 中间的
孤立 H」同时犯 MW 和 MS。

**算法怎么修**:把 `f:H` 改成 `f:L`,让它**延续**左右 cell 的 L implant:

```
        列0     列1     列2
行0:   C:L     f:L     C:L     ← 整行连成一条 3 宽的 L 区
```

- 现在 `列0..列2` 是一整条 ≥2 宽的 L → MW 合格;全 L → 无 MS。**残留 0**。
- 关键点:cell 的 VT 是**固定边界**;算法不改 cell,而是改中间那个 filler 去
  **迁就**两边固定的 L,从而恢复 implant 连续。

---

### 行为 4 — 固定边界:只有动 cell 才能修 → 判残留,回报上游

**场景**:相邻两个 cell 的 VT 本身就不同(中间没有 filler 可调,或即使有
filler 也消不掉这道 cell-cell 边界):

```
        列0     列1
行0:   C:L     C:H      ← 两个 cell 直接相邻,L 碰 H
```

**为什么是违例**:`C:L` 和 `C:H` 水平相邻、VT 不同 → MS +1。

**算法怎么做**:这道违例的两边**都是 cell**(固定边界),按 FR-1 算法**不能动
cell**,而它**没有 filler 站点**可改 → 无能为力。于是它**不修**,把这条违例
计入 `unresolved` **回报上游**(由上游决定是否移动 cell / 改 implant)。

- 这就是「最大化修复、而非强行清零」(FR-3):能用换 filler VT 修的就修
  (行为 1–3),修不了的(行为 4 / 库里缺 VT)如实上报,不会瞎改 cell。
- 推广:即使中间夹了一个 filler,如果无论给它哪种 VT 都至少会留下一处违例
  (例如它一侧是 L cell、另一侧是 H cell,filler 选 L 则右边 MS、选 H 则左边
  MS),代价函数会选「违例最少」的那个,剩下的同样作为残留上报。

---

### 其余情形一览(站点网格)

| # | 情形 | 动作 |
|---|---|---|
| 5 | 库里没有所需 VT | 该 run 回退 → 残留 → 上游(单测 T3) |
| 6 | 某个 VT 改动会制造**新**违例 | 被代价函数拒绝 → 保持原样 |
| 7 | 必须移动 cell 才能修 | 超范围 → 残留 → 上游(同行为 4) |
| 8 | 本来就干净 | 空操作(改前 0、改后 0;单测 T4) |

---

## 9. 约束与假设

- implant 模型是**站点网格**,不是多边形 DRC。
- 重铺改动的 run 时必须精确填满(没有 1 站点 filler ⇒ 不留残缝);铺不出来的
  run 回退,而不是留一半。
- 单一全局 `min_width` / `min_spacing`(逐 VT 的 ωMW/ωMS、以及 ωMS > 1 跨空站点
  的情形是未来工作)。
- 原型用 **height-1** 的 filler 重铺改动的 run。

---

## 10. 验证

- `dpl2/test/filler_vt_repair_test.cpp`:**13/13** —
  T1 台阶 MW+MS 全修(filler H→L、cell 不动),
  T2 跨行 MS 合并,T3 库缺 VT → 残留(零替换),
  T4 干净空操作,T5 评估器自检(标出 L 叠 H、纯 L 块判干净)。

构建 / 运行(无数据库、无 Phase I 依赖):
```
g++ -std=c++17 -I dpl2/src dpl2/src/FillerVtRepair.cpp \
    dpl2/test/filler_vt_repair_test.cpp -o /tmp/vt && /tmp/vt
```

---

## 11. 待办

- 在新数据库上写适配器(实现 §7.1)+ 一个端到端测试。
- Tier-2 列 DP 做局部最优 MW/MS。
- 评估器支持逐 VT 的 ωMW/ωMS,以及 ωMS > 1(跨空站点的间距)。
- 定向修复模式(消费 §4.3 第 (2) 项 DRC 数据),取代整图扫描。
- 落地时支持多行高 filler 重铺(当前是 height-1)。
