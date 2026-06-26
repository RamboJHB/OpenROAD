# 功能规格 — Filler VT 修复(implant MW/MS,只换 VT)

状态:两条路径已实现并测试通过(Part A 15/15 · Part B 12/12)。最后更新 2026-06。

**一句话**:布线 / ECO 扰动后产生的 implant 层 **MW(最小宽)/ MS(最小间距)**
违例,本模块**只通过替换 filler 的 VT(implant 类型)**来修——不移动/删除 cell,
不移动 filler,不改占用(始终 100% 填满)。

---

## 1. 范围与不变量

- **做**:修 implant 层的 MW、MS 违例(含跨行),手段 = 替换 filler 的 VT
  (删旧 filler,在原位建同宽、新 VT 的 filler)。
- **不做**(→ 残留回报上游):移动/缩放/删除 cell;移动 filler 或改占用;min-area
  等其它规则;DRC 检查与违例标记本身。
- **不变量**:只动 filler;cell/macro/blockage 是固定边界;occupancy-preserving
  (不留空 site);目标是**最小化残留**违例(不是要么清零要么报错)。
- **前置门(FR-0)**:拿到 design 先查 **100% utility**(处理区域内无空 site)。
  不满足 → **发 warning、跳过修复、不动任何 filler**,标 `skipped_not_full_utility`,
  把「先填满」交回上游。

---

## 2. 两套实现路径

算法(决定换哪些 filler 的 VT)一套;违例评估(判 MW/MS)有两个后端。

| | **Part A — 自带 site-grid 评估器** | **Part B — 接 ImplantLayerChecker oracle** |
|---|---|---|
| 定位 | 原型 / 可移植 / 无依赖单测 | **生产路径(推荐)** |
| 违例评估 | 自己的 `countViolations`(MW/MS,含 case-B) | 上游 checker `checkPlace/checkDirect` |
| 规则覆盖 | MW/MS 的粗略子集 | P/N、PRL、abutment、containment… 全覆盖 |
| 代码 | `FillerVtRepair.{h,cpp}` · `FillerGrid.h` · `FakeFillerGrid.h` | `FillerVtRepairOracle.{h,cpp}` · `ImplantChecker.h` · `FakeImplantChecker.h` |
| 测试 | `filler_vt_repair_test.cpp`(15/15) | `filler_vt_repair_oracle_test.cpp`(12/12) |

两路径共用同一套决策(贪心 / 未来 DP),只是「问谁违例多少」不同。生产走 Part B;
Part A 是无 checker 时的可移植后备 + 模型参考 + 单测桩。

---

## 3. MW/MS 模型(site 网格)

- **site**:行 × 列的最小放置格;每个被占用 site(`Cell` 或 `CleanFiller`)带一个
  **VT**(= 其 master 的 implant 层)。`run` = 某方向上连续、同 VT 的一段 site。
- **MW(两类窄颈,均已实现)**:同 VT 区任何地方都要 ≥ ωMW 宽。
  - **(a) 行内/对角**:某 site 的同 VT 水平 run 与垂直 run **都** < ωMW → +1。
  - **(b) 跨行交叠颈(case B)**:相邻两行同 VT 区的**交叠列段**宽度 < ωMW,且两行
    各自的水平 run 都比它宽(真颈,非整块本就小)→ +1。
- **MS**:两个正交相邻(右 / 下)的被占用 site 若 VT 不同 → +1(ωMS ≥ 1:异 VT
  不得接触)。

---

## 4. 行为速查

记号:大写 = cell(固定),小写 = filler(可改),字母 = VT,`.` = 空;默认 ωMW=2、
ωMS=1。

| 场景(before) | 违例 | 动作 → 结果 |
|---|---|---|
| `行0: L h` / `行1: h L` | 对角 1 宽 filler:MW + MS | 两 `h`→`l` → 残留 0(T1) |
| `行0: L l l L` / `行1: L h h L` | 上下异 VT 相邻:MS×2 | 下行 `h`→`l` 合并 → 残留 0(T2) |
| `行0: L h L` | 夹在同 VT cell 间、VT 不对:MW+MS | `h`→`l` 延续 L → 残留 0 |
| `行0: L H` | 两 cell 异 VT:MS | 无 filler 可动 → 残留(case B,转上游) |

**case B(ωMW=3 示例)**:两行各自够宽但错开,交叠仅 2 列:

```
      0 1 2 3 4 5 6 7 8 9
行0:  . . L L L L L L . .      L = col2-7
行1:  . . . . x x L L L L      L = col6-9,col4-5 是异 VT filler x
                  ^ ^          交叠仅 col6-7(宽 2)< ωMW=3 → MW(b)
```

修:`x`→`l`(col4-5)→ 交叠变 col4-7(宽 4)≥ ωMW;前提 col4-5 是可改 filler 且库
有 L,否则残留。

---

## 5. 算法

**Part A — `FillerVtRepair`(贪心)**
1. 读网格 → `vt / present / filler` 三层;`countViolations` 算 MW/MS(含 case-B)。
2. 贪心坐标下降:逐 filler site 试库内每个 VT,取使违例最少者(合并项打破平台);
   迭代到不动点。
3. 落地:VT 变了的同 VT run 用库 master `exactFill` 精确平铺 → `clearSite` +
   `placeFiller`;铺不出来则回退(残留)。

**Part B — `FillerVtRepairOracle`(贪心 + checker oracle)**
1. `initialize` 后,`checkDirect` 取当前违例(定位 / 开窗)。
2. 逐个可改 filler run,对每个候选 VT 用 `checkPlace` 问代价(非提交),取最优;
   `commitPlace` 落地。迭代到不动点。
3. 决策层只依赖 `ImplantChecker` 5 方法,不碰 DB / 真实 checker。

> Tier-2(未来):窗口 DP(论文式最优,替换贪心,不动 oracle 接缝)。1D 单行 DP
> 多项式、最优、可实现;2D 跨行对行数指数,须靠 violation 的 `xWindow` 开小窗。

---

## 6. 接口

### 6.1 `FillerGrid`(Part A,6 方法;实现它 = 移植到一个新 DB)

| 方法 | 行为 | 新 DB 需暴露 |
|---|---|---|
| `numRows()` / `numCols(row)` | 行数 / 行内 site 数 | 行、行宽 / site 宽 |
| `kindAt(r,c)` | Empty/Cell/CleanFiller/Blocked | 覆盖该 site 的实例类别 |
| `vtAt(r,c)` | 被占用 site 的 VT | 实例 master 的 IMPLANT 层 → VT id |
| `clearSite(r,c)` | 删该 filler → 空 | 删实例能力 |
| `placeFiller(f)` | 建一个 VT 为 `f.vt` 的 filler | `(vt,宽,高)→master` + 建实例/坐标/朝向 |

坐标全是 site/行;DBU 几何、orientation、`(vt,w,h)→master` 由适配器掌管。
参考实现 `FakeFillerGrid.h`。

### 6.2 `ImplantChecker`(Part B,5 方法)↔ 真实 checker

| `ImplantChecker` | 绑定到 `ImplantLayerChecker` |
|---|---|
| `violations()` | `checkDirect(...)` 区域违例计数 |
| `changeableRuns()` | `placedInsts()`(`isFiller`)+ masters 推出可改 run |
| `candidateVts()` | `ImplantInput.masters`(`isFiller`)的 implant 层 |
| `evalReplaceRun(run, vt)` | `checkPlace(...)` 评估「该 run 改 vt」(不提交) |
| `commitReplaceRun(run, vt)` | `removeInstance(...)` + `commitPlace(...)` |

接真实 checker = 写一个实现这 5 方法的适配器;测试用 `FakeImplantChecker`。

---

## 7. 上游 checker(ecoPlace `ImplantLayerChecker`)对接结论

- **它是增量 check/commit oracle**(`initialize → checkPlace/checkDirect →
  commitPlace`),不是只给数据。我们把它当**代价 oracle**,不自造规则引擎。
- **数据对照**(他们的结构 → 我们用):

  | checker 结构 | 我们用到的 |
  |---|---|
  | `ImplantInput`(layers/rules/masters/placedInsts/rows/tracks/rowHeight/siteWidth) | `initialize` 全部输入;建窗 / 建库 |
  | `Rule`(primaryLayer, secondaryLayer, minValue, direction, prl, exceptAbutted…) | MW=单层宽;MS=两层间距;阈值 `minValue` |
  | `ImplantLayer`(id, name, **polarity**) | VT = LayerId;polarity = P/N |
  | `MasterInput`(width, height, shapes, **isFiller**) | filler 库 = isFiller;宽 = width/siteWidth |
  | `PlacedInst`(masterId, rowId, columnId, orientation, **isFiller**) | 当前布局;`kindAt/vtAt`;定位 filler |
  | `TrackPattern`(bandSlot, activeInterRowKind, adjacentSlots) | P/N band 与跨行邻接 |
  | `Violation`(instances, **xWindow**, measuredValue, requiredValue, primary/secondaryLayer) | 开窗(`xWindow`+涉及行)+ 代价 |

- **checker 覆盖、我们不必自造**:P/N(polarity+bandSlot)、PRL、abutment、
  containment、case-B(真实 shape 合并)。
- **我们拥有**:决策 / 搜索(换哪些 VT、怎么开窗)、filler 选择 + exact-fill、
  DB↔checker 同步、100% utility 前置门。
- **坐标**:他们 DBU/x、我们 site/行;`Violation.xWindow ÷ siteWidth` + 涉及行
  = 我们的窗口(再扩到最近 cell + 上下 1 行)。
- **`fixable_by_filler`**:checker 可给,但**必要非充分**——判据 = 「违例**窗口内**
  有没有可改 filler」(不能只看直接 participant,否则 case-B 等靠邻近 filler 修的会
  漏判)。`false` = 可靠跳过(case B);`true` = 值得试,真正可修由我们 `checkPlace`
  验证。

---

## 8. 需要上游 / RD 提供

1. **【阻塞】删除/替换已提交 filler 实例**:VT 替换 = 删旧 + 放新;`removeInstance`
   需公开,或 `commitPlace` 支持同 `instanceId` 覆盖。
2. **非提交的 `checkPlace`**:改动后重评估的能力(一次性违例 dump 不够——改一次就
   过期)。只有全量 `checkDirect` 也能用但慢。
3. 每个 `Violation` participant 的 **`isFiller`**(算 `fixable_by_filler`、定位 filler)。
4. **规则阈值**(换算成 site;逐 implant 层、必要时**逐 P/N band**:`(层,band)→(ωMW,
   ωMS)`)。
5. `checkDirect` 能对**任意区域**做真值扫描(取初始 / 窗口违例)。
6. `PhysOrientation` ↔ 行 R0/MX;filler 在某行的合法 orientation。
7. checker 是否假设 100% 填满 / 如何判空 site。

---

## 9. 验证

```
# Part A (15/15): T1 台阶 · T2 跨行 MS · T3 库缺 VT 残留 · T4 no-op · T5 评估器 · T6 case-B
g++ -std=c++17 -I dpl2/src dpl2/src/FillerVtRepair.cpp \
    dpl2/test/filler_vt_repair_test.cpp -o /tmp/vt && /tmp/vt
# Part B (12/12): O1 台阶 · O2 跨行 MS · O3 库缺 VT 残留 · O4 no-op(经 oracle)
g++ -std=c++17 -I dpl2/src dpl2/src/FillerVtRepair.cpp \
    dpl2/src/FillerVtRepairOracle.cpp \
    dpl2/test/filler_vt_repair_oracle_test.cpp -o /tmp/orc && /tmp/orc
```

---

## 10. 约束与假设

- site 网格 implant 模型,非多边形 DRC。
- 重铺改动 run 必须精确填满(无 1-site filler ⇒ 不留残缝);铺不出 → 回退(残留)。
- occupancy-preserving:只换 VT、不留空 site。仅重新切分 filler 宽度对 MW/MS 无影响
  (违例只看逐 site 的 VT)。「删+重插留空隙」能多修一部分(用 spacing 分开异 VT 区),
  但破坏 100% utility、引入别的 DRC → 仅作可选升级层,默认关。
- Part A 评估器:单一全局 ωMW/ωMS(逐 VT、P/N band、ωMS>1 跨空 site 未做)——这些
  在 Part B 由 checker 覆盖。原型重铺用 height-1 filler。

---

## 11. 待办

- **【首要】写 `ImplantChecker` → `ImplantLayerChecker` 适配器**;先解 §8 阻塞项。
- Tier-2 窗口 DP(替换贪心,不动接缝)。
- Part A 评估器:逐 P/N band、逐 VT 的 ωMW/ωMS、ωMS>1。
- 可选「删+重插」升级层(flow 允许留空且额外 DRC 建模时)。
- 落地支持多行高 filler 重铺(当前 height-1)。
