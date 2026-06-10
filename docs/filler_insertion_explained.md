# Filler Insertion(填充单元插入)详解

> 本文针对 OpenROAD 中 **filler insertion（填充单元插入）** 的代码做逐行讲解，并结合芯片物理设计的实际技术概念，从零开始解释。
> 对应代码分支基于 `claude/drt-check-drc-pg-option-ij3eH`(几年前的版本)。
> 核心代码文件:`src/dpl/src/FillerPlacement.cpp`(`dpl` = Detailed Placement,详细布局模块)。

---

## 目录

1. [什么是 Filler?为什么需要它?(技术概念)](#1-什么是-filler为什么需要它技术概念)
2. [先理解几个物理设计的基础概念](#2-先理解几个物理设计的基础概念)
3. [Filler 插入在整个流程中的位置](#3-filler-插入在整个流程中的位置)
4. [用户怎么调用:Tcl 命令层](#4-用户怎么调用tcl-命令层)
5. [核心算法:`FillerPlacement.cpp` 逐函数讲解](#5-核心算法fillerplacementcpp-逐函数讲解)
6. [移除 Filler 与判定 Filler](#6-移除-filler-与判定-filler)
7. [一个完整的例子走一遍](#7-一个完整的例子走一遍)
8. [常见问题 FAQ](#8-常见问题-faq)

---

## 1. 什么是 Filler?为什么需要它?(技术概念)

**Filler cell(填充单元)**,也叫 **filler / fill cell / spacer cell**,是芯片里一种**不实现任何逻辑功能**的"假"标准单元。它里面没有晶体管逻辑(或者只有保证工艺连续性的虚拟结构),它的唯一作用是**把布局中标准单元之间的空隙填满**。

### 为什么布局后会有空隙?

芯片的逻辑部分由许多 **标准单元(standard cell)** 组成,比如与门、或门、触发器等。布局工具(placement)会把这些单元摆放到芯片的 **行(row)** 里。但是单元的数量和宽度不可能正好把每一行 100% 填满,所以行里总会剩下一些**空位(gap)**。

这些空位如果不处理,会带来几个严重的实际问题:

| 问题 | 解释 |
|------|------|
| **电源/地轨断裂** | 每一行的顶部和底部都有 **Power(VDD)** 和 **Ground(VSS)** 的金属轨(rail)。标准单元内部会把这两条轨接通延续过去。如果中间有空隙,电源轨就**断了**,右边的单元就没法稳定供电。Filler 内部也有这两条轨,插进去就能把电源轨**接续起来**。这是 filler 最核心的作用。|
| **N 阱(N-well)不连续** | 标准单元底层有连续的 N-well/衬底注入区。空隙会让阱区断开,导致 **DRC(设计规则检查)** 报错,甚至影响器件正常工作。Filler 保证阱区连续。|
| **制造良率(DFM)** | 现代工艺要求每一层金属/多晶硅都有最低密度,不能有大片空白,否则刻蚀、CMP(化学机械抛光)会出问题。Filler 提供了基础的图形密度。|
| **后续 ECO 空间** | 有一类特殊的 filler 叫 **filler cell with spare logic / decap cell**,可以在流片后做小修改。本文的 filler 是最基础的纯填充型。|

> 一句话总结:**Filler 不干活,但它把每一行的电源轨、阱区、衬底连续地"焊"起来,让芯片能正常通电、满足工艺规则。** 它是布局完成后、布线之前必须做的一步。

### Decap cell 是 filler 的"升级版"

你可能还会听到 **decap cell(去耦电容单元)**。它本质也是一种 filler(同样不实现逻辑、同样用来填空),但它内部额外放了一个电容,挂在 VDD 和 VSS 之间,用来**稳定局部电压、抑制电源噪声**。在很多流程里,会先用 decap 填一部分空隙,再用纯 filler 填剩下的。OpenROAD 这段代码处理的是**通用的填充逻辑**,你给它什么 master(包括 decap)它就用什么。

---

## 2. 先理解几个物理设计的基础概念

读代码前,先把这几个词搞清楚,后面就顺了:

- **Standard cell(标准单元)**:实现逻辑功能的基本积木(门电路、触发器)。所有标准单元的**高度相同**(等于一个 row 的高度),宽度是 **site 宽度的整数倍**。

- **Site(站点)**:布局网格里最小的水平单位格子。可以把一行想象成由许多等宽的小格子拼成,每个格子就是一个 site。单元的宽度永远是 site 宽度的整数倍。代码里的 `site_width_` 就是一个 site 的宽度(单位 dbu)。

- **Row(行)**:芯片 core 区域被水平切成一条条等高的"行",标准单元就坐在行里,像书架上的书一排排放。`row_height_` 是行高,`row_count_` 是总行数。

- **dbu(database unit,数据库单位)**:芯片里所有坐标和尺寸用的整数单位。比如 1 微米可能 = 2000 dbu。用整数避免浮点误差。

- **Core area(核心区)**:芯片中放标准单元的矩形区域。代码里 `core_.xMin()`、`core_.yMin()` 是它的左下角坐标。

- **Master(主单元 / cell master)**:一种单元的"模板/定义"(它的宽度、高度、管脚、图形)。比如 `FILLCELL_X8` 这个 master 定义了一个 8 个 site 宽的 filler。

- **Instance(实例 / inst)**:把一个 master "实例化"放到芯片某个具体坐标上,就成了一个 instance。一个 master 可以被实例化成成千上万个 instance。

- **Orientation(朝向 / orient)**:单元可以正放、翻转放(镜像)。相邻两行通常上下镜像,以便共享电源轨。Filler 必须和它所在行的朝向一致,电源轨才能对齐。

理解这张图:

```
  Core area(核心区)
  ┌──────────────────────────────────────────────┐
  │ row 2 │[AND ][OR ]░░░░░[DFF    ]░░░░░░░░░░░░░░░│  ░ = 空隙(gap)
  │ row 1 │░░░[INV][BUF ]░░░░░░░[NAND ]░░░░░░░░░░░░│
  │ row 0 │[XOR  ]░░░░░░░░░░░[MUX   ]░░░[AND ]░░░░░│
  └──────────────────────────────────────────────┘
            ↑ 每行被切成很多等宽 site
```

Filler insertion 要做的就是:**把上图里所有的 ░(空隙)用 filler 单元填满。**

---

## 3. Filler 插入在整个流程中的位置

OpenROAD 的典型数字后端流程(简化):

```
综合(synth)→ 布图规划(floorplan)→ 电源网络(PDN)
   → 全局布局(global placement)→ 详细布局(detailed placement)
   → ★ Filler 插入 ★ → 时钟树(CTS)→ 全局/详细布线(routing)→ ...
```

Filler 插入在 **详细布局之后、布线之前** 做。原因:
- 必须等所有真正的逻辑单元都摆好、不再移动了(详细布局完成),才能知道剩下的空隙在哪。
- 必须在布线前做,因为电源轨连续性、阱区连续性是布线和后续步骤的前提。

代码放在 `dpl`(detailed placement)模块里,正是因为它紧跟在详细布局之后。

---

## 4. 用户怎么调用:Tcl 命令层

文件:`src/dpl/src/Opendp.tcl`

用户在脚本里这样调用:

```tcl
filler_placement {FILLCELL_X1 FILLCELL_X2 FILLCELL_X4 FILLCELL_X8 FILLCELL_X16 FILLCELL_X32}
# 或者用通配符:
filler_placement FILL*
```

对应的 Tcl 代码:

```tcl
sta::define_cmd_args "filler_placement" { [-prefix prefix] filler_masters }

proc filler_placement { args } {
  sta::parse_key_args "filler_placement" args \
    keys {-prefix} flags {}

  set prefix "FILLER_"                      ;# 默认生成的实例名前缀
  if { [info exists keys(-prefix)] } {
    set prefix $keys(-prefix)               ;# 用户可用 -prefix 改前缀
  }

  sta::check_argc_eq1 "filler_placement" $args
  set filler_masters [dpl::get_masters_arg "filler_masters" [lindex $args 0]]
  dpl::filler_placement_cmd $filler_masters $prefix   ;# 调到 C++
}
```

要点:
- **`filler_masters`**:你提供的一组 filler 单元类型(可用正则/通配符匹配)。通常会提供**多种宽度**(X1、X2、X4、X8...),这样大空隙用宽的填、小空隙用窄的填,效率最高。
- **`-prefix`**:生成出来的 filler 实例的名字前缀,默认 `FILLER_`。
- `get_masters_arg` 把名字(支持正则)展开成真正的 master 对象列表;一个都没匹配上会 warn(`DPL 28`)。
- 最后调用 C++ 函数 `dpl::filler_placement_cmd`,进入核心算法。

---

## 5. 核心算法:`FillerPlacement.cpp` 逐函数讲解

整个算法的思路非常直接:

> **逐行扫描每一行,找到每一段连续的空隙,然后用尽量少、尽量宽的 filler 把这段空隙正好填满。**

下面按函数拆解。

### 5.1 `fillerPlacement` —— 总入口

```cpp
void Opendp::fillerPlacement(dbMasterSeq* filler_masters, const char* prefix)
{
  if (cells_.empty()) {
    importDb();                 // (1) 如果还没把数据库里的单元读进来,先读进来
  }

  std::sort(filler_masters->begin(),
            filler_masters->end(),
            [](dbMaster* master1, dbMaster* master2) {
              return master1->getWidth() > master2->getWidth();   // (2) 按宽度从大到小排序
            });

  gap_fillers_.clear();         // (3) 清空缓存
  filler_count_ = 0;
  initGrid();                   // (4) 建立"行 × site"的二维网格
  setGridCells();               // (5) 把已有单元标到网格上(占格)

  for (int row = 0; row < row_count_; row++) {
    placeRowFillers(row, prefix, filler_masters);   // (6) 一行一行地填
  }

  logger_->info(DPL, 1, "Placed {} filler instances.", filler_count_);  // (7) 报告填了多少个
}
```

**关键点解释:**

- **(1) `importDb()`**:把 OpenDB 数据库里的单元、行、site 信息读到内存里(`cells_` 等结构)。如果之前已经做过详细布局,通常已经读过了。
- **(2) 按宽度从大到小排序 filler_masters**:这是**贪心算法(greedy)** 的关键。填空隙时优先用最宽的 filler,能用最少的实例把空隙填满,既快又干净。
- **(4) `initGrid()`**:建立一个 `row_count_ × row_site_count_` 的二维网格(`Pixel**`)。**每个格子(Pixel)代表一行里的一个 site。** 这是整个算法的核心数据结构。
- **(5) `setGridCells()`**:见 5.2,把已经摆好的真实单元"盖章"到网格上,标记哪些 site 已经被占用了。
- **(6)** 对每一行调用 `placeRowFillers`,这是真正干活的地方。
- **(7)** 打印 `DPL 1`:`Placed N filler instances.`,告诉你一共插了多少个 filler。

#### Pixel 网格长什么样?

`Pixel` 结构(`src/dpl/include/dpl/Opendp.h`):

```cpp
struct Pixel
{
  Cell* cell;          // 这个 site 上坐着哪个单元;nullptr 表示空着
  Group* group_;
  double util;
  dbOrientType orient_;// 这个位置的朝向(决定 filler 怎么放)
  bool is_valid;       // 这个 site 是不是合法可放区域(false=不能放,比如挡块/阻塞)
  bool is_hopeless;
};
```

你可以把整个 core 想象成一张方格纸:
- **横向**:`row_site_count_` 个 site(列)。
- **纵向**:`row_count_` 行。
- 每个格子要么 **被某个单元占着**(`cell != nullptr`),要么 **空着**(`cell == nullptr`),要么 **不可用**(`is_valid == false`,比如那里有 macro 挡块)。

Filler 要填的,就是那些 **空着 + 合法(`cell == nullptr && is_valid`)** 的连续格子。

### 5.2 `setGridCells` —— 把已有单元"盖章"到网格上

```cpp
void Opendp::setGridCells()
{
  for (Cell& cell : cells_) {
    visitCellPixels(
        cell, false, [&](Pixel* pixel) { setGridCell(cell, pixel); });
  }
}
```

- 遍历所有已经布局好的单元 `cells_`。
- `visitCellPixels` 会算出每个单元覆盖了哪些 site(它可能跨好几个 site),对每个被覆盖的 pixel 执行回调。
- `setGridCell(cell, pixel)` 把该 pixel 的 `cell` 指针指向这个单元,等于**标记"这格被占了"**。

做完之后,网格上就清楚地记录了:哪里有单元、哪里是空的。

### 5.3 `placeRowFillers` —— 单行填充(核心)

这是整个 filler 插入最核心的函数。它处理**一行**:

```cpp
void Opendp::placeRowFillers(int row, const char* prefix, dbMasterSeq* filler_masters)
{
  int j = 0;
  while (j < row_site_count_) {                 // 从左到右扫这一行
    Pixel* pixel = gridPixel(j, row);
    const dbOrientType orient = pixel->orient_;

    // —— (A) 找到一个空的、合法的起点 ——
    if (pixel->cell == nullptr && pixel->is_valid) {

      // —— (B) 向右扩展,量出这段连续空隙有多长 ——
      int k = j;
      while (k < row_site_count_
             && gridPixel(k, row)->cell == nullptr
             && gridPixel(k, row)->is_valid) {
        k++;
      }
      int gap = k - j;                          // gap = 这段空隙有几个 site 宽

      // —— (C) 求出"用哪些 filler 正好填满这个 gap" ——
      dbMasterSeq& fillers = gapFillers(gap, filler_masters);

      if (fillers.empty()) {
        // 填不满(凑不出正好等于 gap 的组合),报错
        int x = core_.xMin() + j * site_width_;
        int y = core_.yMin() + row * row_height_;
        logger_->error(DPL, 2,
            "could not fill gap of size {} at {},{} dbu between {} and {}",
            gap, x, y, gridInstName(row, j - 1), gridInstName(row, k + 1));
      } else {
        // —— (D) 真的把 filler 一个个实例化、摆进去 ——
        k = j;
        for (dbMaster* master : fillers) {
          string inst_name = prefix + to_string(row) + "_" + to_string(k);
          dbInst* inst = dbInst::create(block_, master, inst_name.c_str(),
                                        /* physical_only */ true);
          int x = core_.xMin() + k * site_width_;
          int y = core_.yMin() + row * row_height_;
          inst->setOrient(orient);                                 // 朝向跟这行一致
          inst->setLocation(x, y);                                 // 放到正确坐标
          inst->setPlacementStatus(dbPlacementStatus::PLACED);     // 标记为已布局
          inst->setSourceType(odb::dbSourceType::DIST);            // 来源=工具生成
          filler_count_++;
          k += master->getWidth() / site_width_;                   // 指针前移这个 filler 的宽度
        }
        j += gap;                               // 跳过整段已填的空隙
      }
    } else {
      j++;                                       // 这格被占着或不合法,往右走一格
    }
  }
}
```

我们把它拆成四步:

**(A) 找空隙起点**:`j` 从左往右扫,遇到一个**空且合法**的格子(`cell==nullptr && is_valid`)就认为找到了空隙的左边界。

**(B) 量空隙长度**:用 `k` 继续往右走,只要还是空且合法就一直走,直到撞到一个被占的/不合法的格子。这时 `gap = k - j` 就是这段连续空隙的宽度(以 site 数计)。

> 注意:它只在**连续**的空隙里填。中间一旦碰到真实单元,就把它当作两段独立的空隙分别处理。这保证 filler 不会盖到真实单元上。

**(C) 凑出填充组合**:调用 `gapFillers(gap, ...)`(见 5.4),返回"用哪几个 filler master 排起来正好等于 gap"。如果凑不出来,`fillers` 为空 → 报 **`DPL 2` error**,告诉你在哪一行、哪个坐标、夹在哪两个单元之间填不上。
- *什么时候会凑不出来?* 如果你提供的 filler 里**没有 1 个 site 宽的最小 filler(X1)**,那遇到某些奇数尺寸的 gap 就可能凑不出整数组合。所以实践中**强烈建议在 filler 列表里一定包含 X1(单 site)filler**。

**(D) 实例化并摆放**:把 `gapFillers` 返回的每个 filler master,逐个:
- `dbInst::create(..., physical_only=true)`:在数据库里创建一个 filler 实例。`physical_only=true` 表示**它只是物理填充,不参与逻辑网表/连线**(它不在 Verilog 网表里)。
- 命名:`前缀 + 行号 + "_" + 列号`,例如 `FILLER_5_120`。
- `setOrient(orient)`:朝向必须和这一行一致,否则电源轨对不齐。
- `setLocation(x, y)`:算出真实坐标 = core 左下角 + 偏移。`x = core_.xMin() + k*site_width_`,`y = core_.yMin() + row*row_height_`。
- `setPlacementStatus(PLACED)`:标记为已放置。
- `setSourceType(DIST)`:标记这个单元是**工具自动生成/派生**的(DIST),区别于用户网表里的单元。
- `k += master->getWidth() / site_width_`:把游标按这个 filler 的宽度往前推,准备放下一个。
- 全部填完后 `j += gap`,跳过整段空隙,继续找下一段。

### 5.4 `gapFillers` —— 凑出"正好填满 gap"的 filler 组合(贪心)

这是算法里最有"算法味"的部分:**给定一个宽度 gap(site 数),用提供的 filler 拼出正好等于 gap 的一串。**

```cpp
// 返回填满 gap 所需的 master 列表(以 site 宽为单位)
dbMasterSeq& Opendp::gapFillers(int gap, dbMasterSeq* filler_masters)
{
  if (gap_fillers_.size() < gap + 1) {
    gap_fillers_.resize(gap + 1);        // gap_fillers_ 是缓存:gap -> 组合
  }
  dbMasterSeq& fillers = gap_fillers_[gap];

  if (fillers.empty()) {                 // 这个 gap 还没算过,现算
    int width = 0;
    dbMaster* smallest_filler = (*filler_masters)[filler_masters->size() - 1];
    bool have_filler1 = smallest_filler->getWidth() == site_width_;  // 是否有 1-site filler

    for (dbMaster* filler_master : *filler_masters) {     // 已按宽度从大到小排好
      int filler_width = filler_master->getWidth() / site_width_;
      while ((width + filler_width) <= gap
             && (have_filler1 || (width + filler_width) != gap - 1)) {
        fillers.push_back(filler_master);  // 反复使用当前这个(最宽能放下的)filler
        width += filler_width;
        if (width == gap) {
          return fillers;                  // 正好填满,成功
        }
      }
    }
    fillers.clear();                       // 没凑出来 → 返回空,代表失败
  }
  return fillers;
}
```

**思路(贪心 + 缓存):**

1. **缓存 `gap_fillers_`**:同一个芯片里,相同宽度的空隙会出现很多次。所以第一次算出某个 `gap` 的组合后就缓存起来(`gap_fillers_[gap]`),下次同样的 gap 直接复用,不用重算。这是个典型的空间换时间优化。

2. **贪心填充**:因为 `filler_masters` 已经按宽度**从大到小**排好,所以外层循环先拿最宽的 filler,内层 `while` 反复塞这个 filler,直到再塞就超过 gap 了,再换下一个更窄的。这样会**优先用最宽的单元**,用到的实例最少。

3. **那个看着奇怪的条件 `(have_filler1 || (width + filler_width) != gap - 1)`**:
   - 它在避免一个**死胡同**:如果你的 filler 里**没有 1-site 的最小 filler**(`have_filler1 == false`),那就绝对不能让剩余空隙变成**恰好 1 个 site**——因为剩 1 个 site 时你没有任何 filler 能填它,整段就废了。
   - 所以这个条件的意思是:"**如果没有 1-site filler,就不允许走到'填完当前这个后只剩 1 个 site'的局面**",提前避开这个无解状态。
   - 如果你**有** 1-site filler(`have_filler1 == true`),那任何剩余都能用 1-site 收尾,这个限制就不需要了。

4. **成功**:`width == gap` 时返回组合。**失败**:循环结束还没凑满 → `fillers.clear()` 返回空 → 上层报 `DPL 2`。

> **实践建议**:filler 列表里**务必包含一个 1-site 的 filler(如 `FILLCELL_X1`)**。有了它,任意整数 gap 都一定能填满,永远不会出现 `DPL 2` "could not fill gap" 的错误。

### 5.5 `gridInstName` —— 报错时告诉你空隙夹在谁和谁之间

```cpp
const char* Opendp::gridInstName(int row, int col)
{
  if (col < 0)                 return "core_left";    // 空隙顶到了行最左
  if (col > row_site_count_)   return "core_right";   // 顶到了行最右
  const Cell* cell = gridPixel(col, row)->cell;
  if (cell) return cell->db_inst_->getConstName();    // 返回相邻单元的名字
  return "?";
}
```

纯粹是为了让 `DPL 2` 的报错信息更友好:告诉你填不上的那段空隙,左右两边分别是哪个实例(或者是不是顶到了 core 边界)。方便你定位问题。

---

## 6. 移除 Filler 与判定 Filler

### 6.1 `removeFillers` —— 删掉所有 filler

```cpp
void Opendp::removeFillers()
{
  block_ = db_->getChip()->getBlock();
  for (odb::dbInst* db_inst : block_->getInsts()) {
    if (isFiller(db_inst)) {
      odb::dbInst::destroy(db_inst);     // 从数据库里删除
    }
  }
}
```

对应 Tcl 命令 `remove_fillers`。**为什么需要删 filler?**
- 做 **ECO(工程变更)** 或想重新跑布局时,得先把旧的 filler 清掉,改完逻辑后再重新插。
- Filler 是纯物理、可丢弃的,删掉不影响逻辑功能。

### 6.2 `isFiller` —— 怎么判断一个实例是不是 filler

```cpp
bool Opendp::isFiller(odb::dbInst* db_inst)
{
  dbMaster* db_master = db_inst->getMaster();
  return db_master->getType() == odb::dbMasterType::CORE_SPACER
         // 排除被当作 tap cell(抽头单元)用的 spacer
         && db_inst->getPlacementStatus() != odb::dbPlacementStatus::LOCKED;
}
```

- 判据 1:master 类型是 **`CORE_SPACER`**。在 LEF 里,filler 单元的类型就是 `CORE SPACER`,这是行业标准标记。
- 判据 2:**排除 `LOCKED` 状态的**。因为有些 spacer 被当作 **tap cell(衬底抽头单元,用来防闩锁 latch-up)**,这种是有用的、被锁定的,不能当普通 filler 删掉。

### 6.3 `isOneSiteCell` —— 判断是不是单 site filler

```cpp
bool Opendp::isOneSiteCell(odb::dbMaster* db_master) const
{
  return db_master->getType() == odb::dbMasterType::CORE_SPACER
         && db_master->getWidth() == site_width_;
}
```

判断一个 master 是不是"1 个 site 宽的 spacer",也就是上面反复强调的那个**最小 filler(X1)**。

---

## 7. 一个完整的例子走一遍

假设:
- `site_width_ = 200 dbu`,某一行有 20 个 site。
- 详细布局后,这一行 site 0–4 被一个单元占了,site 5–14 空着,site 15–19 又被一个单元占了。
- 提供的 filler:`FILLCELL_X8`(8 site)、`FILLCELL_X4`(4)、`FILLCELL_X2`(2)、`FILLCELL_X1`(1)。排序后从大到小:X8, X4, X2, X1。

执行 `placeRowFillers`:

1. `j=0`:被占 → `j++` 一直走到 `j=5`。
2. `j=5`:空且合法 → 找起点。向右扩展到 `k=15`(site 15 被占),所以 **gap = 15 − 5 = 10**。
3. 调 `gapFillers(10, ...)`:贪心从大到小——
   - 放 X8 → width=8。再放 X8 会到 16 > 10,换 X4 → 8+4=12>10,换 X2 → 8+2=10 == gap,成功!
   - 组合 = `[X8, X2]`。
4. 实例化:
   - 在 site 5 放 `FILLER_<row>_5`(X8),游标 `k = 5+8 = 13`。
   - 在 site 13 放 `FILLER_<row>_13`(X2),游标 `k = 13+2 = 15`,正好填满。
5. `j += 10` → `j=15`,继续;15–19 被占 → 走到行尾结束。

结果:这一行的 10-site 空隙被 1 个 X8 + 1 个 X2 = 2 个 filler 正好填满,电源轨接续完整。

```
site:  0    5              15       20
      [=占用=][  X8 filler  ][X2][=占用=]
              ↑5             ↑13  ↑15
```

---

## 8. 常见问题 FAQ

**Q1:Filler 会连到电路上吗?会影响时序/功能吗?**
不会改变逻辑功能。它是 `physical_only` 实例,不进逻辑网表;它的 VDD/VSS 轨会跟着行的电源网络连上,但不参与信号。所以**对时序没有逻辑影响**,只是补全了电源/密度。

**Q2:为什么要提供多种宽度的 filler?只给一个 X1 不行吗?**
只给 X1 在功能上也能填满任意 gap,但会生成**海量的小实例**(一个 100-site 的空隙要 100 个 X1),数据库膨胀、工具变慢。提供多种宽度让算法用最少实例填满,又快又省内存。

**Q3:什么时候会看到 `DPL 2 could not fill gap` 报错?怎么解决?**
当你提供的 filler 宽度凑不出某个 gap 时(通常是**缺少 1-site 的 X1 filler**,遇到奇数残余填不上)。解决办法:**在 `filler_placement` 的列表里加入 1-site filler**。

**Q4:`filler_placement` 和 `remove_fillers` 的关系?**
`filler_placement` 插入,`remove_fillers` 全部清除。改动设计后通常是:`remove_fillers` → 改逻辑/重布局 → 再 `filler_placement`。

**Q5:这段代码用的是什么算法?**
逐行**线性扫描**找连续空隙 + 对每段空隙做**贪心(从宽到窄)**装箱,并用 `gap_fillers_` **缓存**相同 gap 的结果。整体时间复杂度大致和 (行数 × 每行 site 数) 成正比,非常高效。

---

## 涉及的源码文件一览

| 文件 | 作用 |
|------|------|
| `src/dpl/src/FillerPlacement.cpp` | **核心**:插入/移除 filler 的全部 C++ 逻辑 |
| `src/dpl/src/Opendp.tcl` | Tcl 命令 `filler_placement` / `remove_fillers` 的定义与参数解析 |
| `src/dpl/include/dpl/Opendp.h` | `Pixel`、`Cell`、网格成员变量(`row_count_`、`site_width_`、`gap_fillers_` 等)的声明 |
| `src/dpl/src/Opendp.i` | SWIG 接口,把 C++ 函数暴露给 Tcl(`filler_placement_cmd` 等) |
| `src/dpl/README.md` | 命令的官方简要文档 |

---

*本文档基于分支 `claude/drt-check-drc-pg-option-ij3eH`(几年前版本)的代码。新版本 OpenROAD 的 dpl 模块已重构(例如网格抽象成独立的 `Grid` 类),整体思路一致但函数组织可能不同。*
