# 功能规格 — Filler VT 修复(implant MW/MS,只换 VT)

状态:操作契约 = **一次 fix · 不调 DRC · 修不了 mark unfixable**(§2)。修复算法有
三套候选 **Plan A / B / C**(§5)。**旧做法(贪心坐标下降 + 反复评估 / oracle
checkPlace 循环)已废弃**;现有代码 `FillerVtRepair` / `FillerVtRepairOracle` 视为
legacy,待按本规格重写。最后更新 2026-06。

**一句话**:布线 / ECO 扰动后产生的 implant 层 **MW(最小宽)/ MS(最小间距)**
违例,本模块**只替换 filler 的 VT**来修——不动 cell、不移动 filler、不改占用;
一次过,修不了的标 unfixable 回报上游。

---

## 1. 范围与不变量

- **做**:修 implant 层 MW、MS 违例(含跨行),手段 = 替换 filler 的 VT
  (删旧 filler、原位建同宽新 VT filler)。
- **不做**(→ mark unfixable 回报上游):移动/缩放/删除 cell;移动 filler 或改占用;
  min-area 等其它规则;DRC 检查本身。
- **不变量**:只动 filler;cell/macro/blockage 是固定边界;occupancy-preserving
  (不留空 site)。
- **前置门(FR-0)**:先查 **100% utility**(处理区域无空 site)。不满足 →
  warning + 跳过、不动任何 filler,标 `skipped_not_full_utility`,交回上游填满。

---

## 2. 操作契约(本轮定稿)

- **一次 fix**:读输入 → 一次 solve → 一次 apply。**不迭代、不「修完再查再修」**。
- **不调 DRC check**:全程不调用上游 checker 做评估。若算法内部需要判违例,用我们
  自己的 MW/MS 模型(§3)。
- **输入**:上游**一次性**给的 violation 列表(每条含 `xWindow` / `instances` /
  type / 两侧 VT…,见 §6)+ 当前 grid。
- **输出**:一次性应用的 filler VT 替换 + 一份 **unfixable 列表**(回报上游)。
- **废弃**:旧的贪心 + 反复 `countViolations`、oracle 的 `checkPlace` 循环——那依赖
  反复评估,违背「一次、不调 DRC」,不再用。(因此也**不再需要**上游提供非提交的
  `checkPlace`;checker 退化为**一次性数据源**,见 §6。)

---

## 3. MW/MS 模型(site 网格)

- **site**:行 × 列最小放置格;被占用 site(`Cell`/`CleanFiller`)带一个 **VT**
  (= 其 master 的 implant 层)。`run` = 某方向连续、同 VT 的一段 site。
- **MW(两类窄颈)**:同 VT 区任何地方要 ≥ ωMW 宽。
  - (a) **行内/对角**:某 site 同 VT 的水平 run 与垂直 run **都** < ωMW → +1。
  - (b) **跨行交叠颈(case B)**:相邻两行同 VT 区的**交叠列段** < ωMW,且两行各自
    更宽(真颈)→ +1。
- **MS**:两个正交相邻(右/下)的被占用 site 若 VT 不同 → +1(ωMS≥1:异 VT 不得接触)。
- ωMW/ωMS 来自 tech 规则,**可逐 implant 层、逐 P/N band 不同**(§6)。

---

## 4. 公共步骤(三套算法共用)

三套算法的差别只在 §5「窗内怎么定 VT」;开窗、合并、P/N 对齐、固定 cell、可实现性、
mark unfixable 都一样:

### 4.1 开窗(大小由 violation type + inter/intra 决定)
- **intra-row**:窗 = `xWindow` 那段列,**只本行**;横向向外扩到最近固定边界
  (cell/macro/空)。
- **inter-row**:横向同上;**纵向**含 `instances` 涉及行 + 相邻耦合行(±1)。
- 横向扩到固定边界,保证窗外邻居要么固定 cell、要么 VT 一致 → 改窗内不向窗外漏新违例。

### 4.2 合并重叠窗
所有窗先**合并**(重叠/相邻并成一个)再解,避免多条违例的窗互相打架。

### 4.3 P/N orientation + 两侧对齐
- 边界匹配**逐 band**:filler 要在 P band 与 N band 上都延续边界 cell 的 implant。
- 选的 master 必须符合**该行 orientation**(R0/MX 决定 P/N 上下);
  `(vt,w,h)→master` 映射带 orientation。无匹配 master → unfixable。

### 4.4 窗内有固定 cell(内部固定边界)
- 固定 cell **永不改**,钉死自己 VT,并约束相邻 filler(避免和它 MS)。
- 它把窗内可改 filler 切成若干**连通块**(filler 四连通,cell 是墙);每块对着它接触
  到的固定 cell + 外边界求解。
- 块接触**同一 VT** → 整块取它;接触**多种 VT** → 块内再拆贴合各自固定邻居;
  **拆不开**(如 1 个 filler 被左 L、上 H 两个固定邻居夹死)→ 该 filler **unfixable**。

### 4.5 可实现性
每段选定 VT 必须能被库 master `exactFill` 精确平铺;不行 → 换边界 VT / 调拆分点 /
该段 unfixable。无 1-site filler ⇒ 不留残缝。

### 4.6 mark unfixable
两侧/邻居约束冲突无解、库缺 VT、库铺不出、需动 cell 等 → 该 violation 标 unfixable
回报上游,**不动**对应 filler。

---

## 5. 三套 fix 算法(窗内如何定 VT)

### Plan A — 两侧边界驱动(简单、覆盖常见)
窗内 VT **照固定边界定**:
- 边界**单一 VT** → 窗内统一成它;
- 边界**两种 VT** → **L|R 拆分**(左段=左边界 VT、右段=右边界 VT,拆分点落可平铺处);
- **无边界**(孤立岛)→ 任选库可平铺且 ≥ωMW 的 VT。
本质 = 局部贴合;协调性复杂排布会过早判 unfixable(其实有解)——由 B/C 超越。

### Plan B — 窗口内精确最优
窗内把可改 filler 当变量、固定 cell 当边界,**枚举所有库可实现赋值,取残留 MW/MS 最少**:
- 窗小 → 穷举 `|VT|^F`;窗大 → 列 DP(状态 = 当前列各行 VT + run 长度封顶,
  宽度线性、行数指数)。
- 窗间被固定边界隔开 ⇒ **每窗最优 = 模型下全局最优**;覆盖 ≥ Plan A。
- 窗超阈值 → 回退 Plan A 并 log。

### Plan C — 图能量最小化 / min-cut(全局、二态精确)
把整片耦合区域建成一张图,**一次性全局求最优**(详见对话解释):
- 节点 = 可改 filler;标签 = 候选 VT;固定 cell = 把相邻节点钉到该 VT 的 terminal;
  相邻 filler 间的边 = 「异 VT 则计 MS」的代价。
- **两种 VT(P/N 两态)→ 化成 min-cut / max-flow,多项式时间求全局精确最优**,
  对整片区域一次解(无 Plan B 的行数指数问题)。
- **>2 种 VT** → 多标签 move-making(α-expansion:反复做二态 min-cut)给强近似全局最优。
- 自带一个小 max-flow(Dinic/BK,无外部依赖)。

### 建议
**Plan A 打底 + Plan B 兜上限**(A 判 unfixable / 双边界冲突的窗升级到 B 精确解);
VT 基本两态(P/N)且簇很大时上 **Plan C**。三者均:一次过、不调 DRC、修不了 mark
unfixable。共同上限 = **我们模型**下最优(P/N/PRL 偏差以事后一次 DRC 量残留为准)。

---

## 6. 输入 / 输出;上游 checker 的角色(数据源,非 oracle)

我们**只取一次** violation 列表 + grid,**不调 `checkPlace`**。violation 字段对照
真实 `ImplantLayerChecker::Violation`:

| 我们要的 | 真实 `Violation` 字段 | 用途 |
|---|---|---|
| 违例位置(x 线段) | `xWindow`(XInterval) | 开窗(÷siteWidth → 列) |
| 涉及哪几行 / 哪些实例 | `instances`(+ `PlacedInst.rowId`) | inter/intra 判定、纵向开窗、定位 filler |
| 两侧 VT | `primaryLayer` / `secondaryLayer` | 定目标 VT(MS=2,MW=1) |
| 类型 MW/MS | 由 `ruleId`/`relationship` 推 | 分流、开窗规则 |
| 每个 participant 是不是 filler | **需 RD 暴露** `PlacedInst.isFiller` | 判可改 / 定位 / mark unfixable |

输出:应用的替换 filler 列表 + unfixable 列表。

**需要 RD 提供**:
1. **删除/替换已提交 filler 实例**(落地用;`removeInstance` 公开 或 `commitPlace`
   同 id 覆盖)——阻塞项。
2. 每个 participant 的 **`isFiller`**。
3. **规则阈值**(换算成 site;逐 implant 层、逐 **P/N band**:`(层,band)→(ωMW,ωMS)`)。
4. 一次性 violation 列表(上面的字段)+ grid 快照。
   *(不再需要非提交的 `checkPlace` —— 我们不在修复中调 DRC。)*

---

## 7. 接口 `FillerGrid`(落地用,移植 = 实现这一个类)

| 方法 | 行为 | 新 DB 需暴露 |
|---|---|---|
| `numRows()` / `numCols(row)` | 行数 / 行内 site 数 | 行、行宽/site 宽 |
| `kindAt(r,c)` | Empty/Cell/CleanFiller/Blocked | 覆盖该 site 的实例类别 |
| `vtAt(r,c)` | 被占用 site 的 VT | master 的 IMPLANT 层 → VT |
| `clearSite(r,c)` | 删该 filler → 空 | 删实例 |
| `placeFiller(f)` | 建 VT 为 `f.vt` 的 filler | `(vt,w,h)→master` + 建实例/坐标/朝向 |

坐标全是 site/行;DBU 几何、orientation、`(vt,w,h)→master` 由适配器掌管。参考实现
`FakeFillerGrid.h`。

---

## 8. 行为速查(默认 ωMW=2、ωMS=1;大写=cell 固定,小写=filler 可改)

| 场景(before) | 违例 | 动作 → 结果 |
|---|---|---|
| `行0: L h` / `行1: h L` | 对角 1 宽 filler:MW+MS | 两 `h`→`l` → 残留 0 |
| `行0: L l l L` / `行1: L h h L` | 上下异 VT 相邻:MS×2 | 下行 `h`→`l` → 残留 0 |
| `行0: L h L` | 夹同 VT cell、VT 不对:MW+MS | `h`→`l` |
| `行0: L H` | 两 cell 异 VT、无 filler | **unfixable**(转上游) |

**case-B(ωMW=3)**:两行各自够宽但错开,交叠仅 2 列:
```
      0 1 2 3 4 5 6 7 8 9
行0:  . . L L L L L L . .      L = col2-7
行1:  . . . . x x L L L L      L = col6-9,col4-5 是异 VT filler x
                  ^ ^          交叠仅 col6-7(宽 2)< ωMW=3 → MW
```
修:`x`→`l`(col4-5)→ 交叠变 col4-7(宽 4)≥ ωMW;col4-5 须可改且库有 L,否则 unfixable。

---

## 9. 约束与假设

- site 网格 implant 模型,非多边形 DRC;「尽量多修」= 我们模型下最优。
- occupancy-preserving:只换 VT、不留空 site;仅重切 filler 宽度对 MW/MS 无影响
  (违例只看逐 site 的 VT)。
- 重铺改动段必须精确填满(无 1-site filler ⇒ 不留残缝);铺不出 → unfixable。
- 模型保真:case-B 已建模;**P/N band、PRL 是否建模待定**(影响一次 pass 的真实覆盖)。

---

## 10. 待办

- **实现 Plan A(+ B 兜底)**,按 §4 公共步骤 + §5;mark unfixable;一次 apply。
- 写 `FillerGrid` → 真实 DB / `ImplantLayerChecker` 适配器;解 §6 阻塞项(remove/replace)。
- 视需要建模 P/N band、PRL;评估是否上 Plan C(二态大簇)。
- 清理 legacy(`FillerVtRepair` 贪心 / `FillerVtRepairOracle`):保留模型函数
  (`countViolations` 含 case-B、`exactFill`)供 A/B 复用,移除 recheck 循环。
