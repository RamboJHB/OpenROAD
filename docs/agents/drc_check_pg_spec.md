# Implementation Spec — `check_drc -check_pg` (PG-only DRC)

## 1. Goal
给 `drt::check_drc` 增加一个 `-check_pg` flag，使 `check_drc` 只报告**涉及
power/ground (PG) 几何的 DRC violation**（PG-PG、PG-signal、PG-blockage、PG
自身、PG 越界），并抑制纯非 PG 的 violation（signal-vs-signal 等）。语义对齐
Cadence Innovus `verify_drc -check_only special`。不改变无 flag 时 `check_drc`
的原有行为。

## 2. Command interface
```
check_drc -output_file <file> [-box {x1 y1 x2 y2}] [-check_pg]
```
- `-check_pg`：boolean flag，开启 PG-only 模式。
- 调用链：`check_drc` (TritonRoute.tcl) → `drt::check_drc_cmd(..., bool check_pg)`
  (TritonRoute.i) → `TritonRoute::checkDRC(..., bool check_pg)` (TritonRoute.cpp)。

## 3. Global switch
- `bool DRC_CHECK_PG`（`global.h` 声明 / `global.cpp` 定义，默认 `false`；
  `serialization.h` 的 `serializeGlobals` 序列化，供 distributed worker 一致）。
- `checkDRC` 入口 `DRC_CHECK_PG = check_pg;`，结束处复位为 `false`，防止泄漏到
  后续普通 run。GC worker 通过该 global（而非传参）读取模式。

## 4. Core mechanism — fixed / non-fixed classification
关键事实：GC engine 对 spacing/short 检查在**两个 shape 都 fixed** 时跳过
（`FlexGC_main.cpp`）。利用这一点，PG-only 模式下**不删除任何 object**，只控制
`isFixed`：

- **PG (supply) 几何 → non-fixed**（"under test"）。
- **其余一切（signal/clock pin+routing、blockage/obstruction）→ fixed**
  （"background"）。

结果：含 PG 的 pair 一定有一侧 non-fixed → 被检查；非 PG 之间的 pair 双 fixed →
被跳过。

实现（`FlexGC_init.cpp`）：
- `bool FlexGCWorker::Impl::isPGObj(frBlockObject*)`：判定 object 是否属于
  supply net。按 type 解析 owner net（`frcBTerm` / `frcInstTerm` / `frcPathSeg`
  / `frcVia` / `frcPatchWire` / `drc*`），用 `odb::dbSigType::isSupply()`；
  blockage / 无 net → 非 PG。
- `initDesign`：固定 object 加载处
  `isFixed = DRC_CHECK_PG ? !isPGObj(obj) : true;`（normal 模式仍全 fixed）。
- `initRouteObj(frBlockObject*, gcNet*, bool isFixed = false)`：新增 `isFixed`
  形参并透传到 `addPolygon` / `addRectangle`。
- `initNetsFromDesign`：不再跳过非 supply net；以
  `isFixed = DRC_CHECK_PG ? !net->getType().isSupply() : false` 加载其 routing
  （PG 必须是 special net，DRT-0305 → 此处网络皆非 PG → fixed background）。
- `initDesign_skipObj`：移除 PG 相关跳过，恢复仅受 `targetObjs_` 控制。

## 5. Special case — Min Area reporting
`checkMetalShape_minArea`（`FlexGC_main.cpp`）原本被 `!targetNet_` gate 到
detailed routing，并以 patch 修复（依赖 `drWorker_`）。改动（仅 `DRC_CHECK_PG`
生效）：
- gate 改为 `if (ignoreMinArea_ || (!targetNet_ && !DRC_CHECK_PG)) return;`
- `drWorker_->getDrcBox()` 加空指针保护：
  `if (drWorker_ && !drWorker_->getDrcBox().contains(bbox2)) return;`
- `DRC_CHECK_PG` 时 **emit marker (`AreaConstraint`) 而非 `addPatch`**。
- routing 时 `DRC_CHECK_PG == false`，行为不变。

## 6. PG-to-boundary check (ring / mesh perimeter)
GC engine 无 die/block boundary DRC，故在 `checkDRC` 内、`getDRCMarkers` 之后
内建一个检查（仅 `check_pg` 时运行，**无参数**）：
- 遍历 odb 中 `getSigType().isSupply()` 的 special net 的 `dbSWire` / `dbSBox`。
- 计算 shape 到四条 die edge 的最小有符号距离 `dmin`；`dmin < 0`（即 shape
  **越出** die boundary）→ violation；齐边（`dmin == 0`）不报。
- 生成 `frMarker`（bbox = shape，layer = 对应 drt layer，src = `findNet(name)`
  得到的 `frNet`），constraint 用新增的 `frPGBoundaryConstraint`，追加进 markers，
  再 `reportDRC`。
- 新增 violation type：`frConstraintTypeEnum::frcPGBoundaryConstraint`
  （`frBaseTypes.h`）、`class frPGBoundaryConstraint`（`frConstraint.h`）、
  `getViolName()` 返回 `"PG Boundary Spacing"`。

## 7. Debug logic chain
debug stream tag `checkPG`（`set_debug_level DRT checkPG 1`）输出清晰链条：
```
[check_drc] entry: mode=PG-ONLY (-check_pg), box=...
[check_drc] effective drc box=...
[init] PG(non-fixed)=N, background-fixed(signal/obs)=M
[netinit] load non-PG net <name> as fixed background
[check_drc] PG boundary: K marker(s) (shapes crossing the die edge)
[check_drc] done: T marker(s) reported (PG-only)
```

## 8. Reported rules under `-check_pg`
- Metal **Short** / **Metal Spacing**（含 metal2 width-dependent `SPACINGTABLE`
  / PRL）：PG-PG、PG-signal、PG-blockage。
- **Min Width**、**Off Grid**（single-shape，跳过全 fixed shape）。
- **Cut Spacing** / cut short（PG via）；**Minimum Cut**。
- **Min Area**（见 §5）。
- **PG Boundary Spacing**（见 §6）。
- 引擎 / tech 限制未覆盖：**Min Step**（standalone `check_drc` 不 surface
  marker）、**NDR** / metal multi-patterning mask、LEF58 `METALWIDTHVIATABLE`
  enclosure（需对应 LEF rule）。

## 9. Behavior guarantees
- 无 `-check_pg` 时 `check_drc` 与改动前完全一致（所有条件分支取原 branch）。
- detailed routing 不受影响：`DRC_CHECK_PG` 在 routing 期间恒为 `false`，且 GC
  在 DR 模式下走 `initDRObj`（非 `initNetsFromDesign`），boundary 检查不运行。

## 10. Files changed
| File | Change |
|---|---|
| `src/drt/src/global.h` / `global.cpp` | `DRC_CHECK_PG` global |
| `src/drt/src/serialization.h` | serialize `DRC_CHECK_PG` |
| `src/drt/src/TritonRoute.tcl` / `.i` / `include/triton_route/TritonRoute.h` | `-check_pg` flag → `checkDRC(..., bool check_pg)` |
| `src/drt/src/TritonRoute.cpp` | set/reset `DRC_CHECK_PG`；PG-boundary check；debug chain |
| `src/drt/src/gc/FlexGC_init.cpp` / `FlexGC_impl.h` | `isPGObj`；fixed/non-fixed classification；`initRouteObj` `isFixed` 形参；debug |
| `src/drt/src/gc/FlexGC_main.cpp` | `checkMetalShape_minArea`：PG 模式 emit marker + null-`drWorker_` guard |
| `src/drt/src/frBaseTypes.h` / `db/tech/frConstraint.h` | `frcPGBoundaryConstraint` + `frPGBoundaryConstraint` + `"PG Boundary Spacing"` |
