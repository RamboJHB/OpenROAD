# 功能规格 — Filler VT 修复(权重法,单次,不调 DRC)

状态:已实现并测试(`dpl2/` 标准库,12/12)。分支基于 **2023 master**(`68fc7ad3`)。
最后更新 2026-06。

**一句话**:上游 checker 给一批 implant **MW/MS 违例**;本模块对每条违例,**只替换
一个相邻 filler 的 VT(implant 类型)**来修;一次过、不调 DRC、修不了的标 **unfixable**
回报上游。

---

## 1. 前提与不变量(全部已定稿的结论)

- **100% utility**:design 已填满,每个 site 不是 filler 就是 cell,**没有空 site**。
  不满足 → 发 warning、不修(交回上游填满)。
- **只换 VT,其它属性全不动**:filler 的 width/height/位置不变;一次 relabel = 换成
  **同 (width,height)、目标 VT** 的 master,orientation 随行(P/N 对齐)。不能改宽、
  不能拆、不能合并、不 re-tile。
- **单次 fix**:读违例 → 一次决策 → 一次落地。**不迭代、不重跑 DRC**。
- **不调 DRC check**:违例由上游一次性给;我们不自己跑 DRC,也不回查。
- **修不了就 mark unfixable** 回报上游(例如必须动 cell),绝不动 cell。

---

## 2. 违例模型(只用于理解;检测是上游的事)

- **MW(最小宽)**:一片**连通同 VT** 区的最窄处 < ωMW。两种形态:① 孤岛(四周异
  VT,本身就窄);② 连片区的细脖子(含上下行交叠只有 1 的 case-B)。
- **MS(最小间距)**:两个不同 VT 区相邻 / 太近。
- 我们**不重新检测** MW/MS——上游 checker 给 `Violation`(含位置);我们只对每条违例
  看它邻域的 filler 并选一个改 VT。

---

## 3. 算法 — 权重法(单次)

对每条违例:

1. **取候选**:违例 anchor site 及其**上下左右 4 个邻居**里,凡是 filler 的 → 候选
   (去重)。「4 邻居」天然覆盖了孤岛、脖子、相邻 MS 三类(要改的那个一定在邻域内)。
2. **给每个候选 filler 打权重**(看它矩形的 4 条边贴着什么 VT):
   - **边上有非-filler cell(固定边界)→ 权重 = 0**(锚定在 cell 上,不动它)。
   - **四周都没有同 VT(孤岛)→ 权重 = MAX**(最该改)。
   - 否则 **权重 = 异 VT 相邻数**:左/右各最多 +1,上/下每个异 VT 邻居 +1(+x)。
   - **按 VT 类型分别计票** `tally[t]`:决定改成哪种 type = 票最高、且**库有同尺寸
     master** 的那个 VT。
3. **选权重最高、且目标 VT 可实现**的候选 → `replaceFillerVt`(同尺寸换 master,
   orientation 随行)。
   - **平票规则(已定稿)**,依次比较:
     1. 权重高者胜;
     2. 权重相同 → **更窄**的 filler(更像孤岛、改动更小);
     3. 还相同 → **同 VT 邻居数 y 更小**者(y = 该 filler 四边相邻同 VT 的格数;
        y 越小越孤立、越该改);
     4. 权重、宽度、y **都相同** → **判 unfixable**(无法决定改哪个,回报上游)。
     实现:`VtRepair::chooseCandidate`(返回 -1 即第 4 种平票 → unfixable)。
4. **没有可用候选** → `reportUnfixable`,原因之一:
   - 候选全贴 cell;② 需要的 VT 没有同尺寸 master;③ 邻域无 filler。

> 这是 Plan A(边界/邻居驱动)的权重投票实现。更优的全局解(min-cut / DP)可在此
> 接缝之上替换决策,不动数据接口——见 §7 局限。

---

## 4. 处理流程(从拿到 violation 到输出 unfixable)

```
readViolations()                      # 上游一次性给
对每条 violation:
  收集 4 邻居中的 filler 候选
  for 候选: weighCandidate()          # 贴 cell→0 / 孤岛→MAX / 否则异VT计数 + 按type计票
  选 max-weight 且目标可实现的候选
    ├─ 有 → replaceFillerVt(同尺寸换 VT,orient 随行)   # FIXED
    └─ 无 → reportUnfixable(reason)                      # UNFIXABLE
```
全程 `[vtr]` debug 输出:读到几条、每条的候选、每个候选的权重/目标、最终动作。

---

## 5. 代码与数据接口(移植只改这一层)

| 文件 | 作用 |
|---|---|
| `dpl2/src/VtRepair.h` | 类型 + **`DesignIO` 接口(PORTING SEAM)** + `VtRepair` 算法声明 |
| `dpl2/src/VtRepair.cpp` | 权重计算 + 单次流程 + debug |
| `dpl2/src/FakeDesign.h` | `DesignIO` 的内存参考实现(测试 + 移植样板) |
| `dpl2/test/vt_repair_test.cpp` | 12/12 |

**`DesignIO`** 把所有「读/写 data」隔离成函数,移植 = 只重写这些:
- 读:`readViolations / numRows / numCols / kindAt / vtAt / fillerIdAt /
  fillerBox / candidateVts / masterExists`
- 写:`replaceFillerVt(同尺寸换 VT,orient 随行) / reportUnfixable`

坐标全是 **site/行**(整数),DBU 几何、orientation、`(vt,w,h)→master` 由实现方掌管。

构建 / 运行(无依赖):
```
g++ -std=c++17 -I dpl2/src dpl2/src/VtRepair.cpp \
    dpl2/test/vt_repair_test.cpp -o /tmp/vtr && /tmp/vtr
```

---

## 6. 给「移植 AI agent」的 prompt(可直接粘贴)

```
你的任务:把 dpl2/src/VtRepair.{h,cpp} 的 filler-VT 修复算法接到 <目标数据库/EDA>。

算法本身不要改。只实现 dpl2/src/VtRepair.h 里 `DesignIO` 抽象类的全部方法(那是
唯一的数据接缝),参照 dpl2/src/FakeDesign.h 的内存实现照抄结构、把每个方法接到真实
数据库:

READ:
- readViolations(): 从上游 implant checker 取违例列表;每条给 {id, row, col, type}。
  row/col 是 site 网格坐标(把 DRC 的 DBU/xWindow 用 siteWidth/rowHeight + core 原点
  换算成 site 列、行)。type 填 "MW"/"MS"(仅信息)。
- numRows()/numCols(row): core 区行数 / 每行 site 数。
- kindAt(r,c): 返回 Empty/Filler/Cell/Blocked(filler=可改;cell/macro=固定)。
- vtAt(r,c): 该 site 占用者 master 的 IMPLANT 层 → 你的 VT id(字符串)。
- fillerIdAt(r,c): 覆盖该 site 的 filler 实例 id(非 filler 返回 NO_FILLER)。
- fillerBox(id): 该 filler 的矩形 {row,col,width,height,vt}(width/height 以 site/行)。
- candidateVts(): 库里所有可用 VT。
- masterExists(vt,w,h): 库里有没有「正好 w×h、VT=vt」的 master(我们绝不改尺寸)。

WRITE:
- replaceFillerVt(id,newVt): 删掉该 filler 实例,在**同位置**新建一个**同 (w,h)**、
  VT=newVt 的 master;orientation 设成该行的 orientation(让 P/N 对齐)。其它不变。
- reportUnfixable(v,reason): 把修不了的违例回报上游。

约束(务必遵守):
- 只换 VT;不改 width/height/位置;不拆不合并不 re-tile。
- 不在修复过程中调用 DRC checker。
- 全部坐标用 site/行整数;DBU 换算只在你的实现里做。
- 先确认 design 是 100% utility(无空 site);否则不要跑修复。

验收:用 FakeDesign 的 12 个单测思路在你的环境复现(孤岛→改 VT、贴 cell→unfixable、
缺同尺寸 master→unfixable、4 邻居覆盖脖子)。开 verbose 看 [vtr] debug 链确认逻辑。
```

---

## 7. 局限与后续

- **平票已定稿**(§3 步骤 3):权重 → 更窄 → 同 VT 邻居数 y 更小 → 全相同则 unfixable。
- **贪心、单次、不回查**:按原始状态打分一次性落地,改一个会影响邻居但不重算 → 可能
  次优,甚至「把违例挪个位置」(尤其两个 ≥2 宽区之间的 MS:单次换一个 filler 只能移动
  边界,消不掉)。这类 VT-only 本就难单次清零 → 该残留如实标。
- **贴 cell 即权重 0** 是保守规则:cell 边上的 filler 不翻 → 一些「其实改成 cell 的 VT
  就能修」的会被判 unfixable。是有意的稳妥取舍,可后续放宽(只在「会和 cell 冲突」时归零)。
- **更优解**:在同一 `DesignIO` 接缝上,把决策换成 **min-cut(二态全局精确)** 或
  **窗口 DP**,可提高单次覆盖;数据接口不变。
- P/N band、PRL 等精细规则由上游 checker 体现在违例里;本模块不建模。
