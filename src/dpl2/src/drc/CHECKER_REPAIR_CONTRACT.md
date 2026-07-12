# Checker 改动说明 — 供 filler-repair engine 对接(给 checker RD)

对象:`ImplantLayerChecker.{h,cpp}`。背景:filler-repair engine 会消费 overlay
接口的结果来做 VT 修复(把周围 filler 换型消 implant MW/MS 违例)。审查你交付的
overlay 实现后,发现 **2 条契约问题** 会让 repair 得到错误的"干净"判断,外加
**1 个头/实现不一致的编译问题**。我已按最小、可加性方式改好,**所有改动都带
`[fillerRepair-fix]` 注释标记**,`grep -n "fillerRepair-fix"` 即可全部定位。

这些是**你的文件**,改动请你 review 并接管;有异议随时改回或换实现,只要保住
下面两条语义即可。engine 侧在这两条落地前继续接 fake checker,不接真 checker。

---

## 改动 1:新增 `checkPlaceWithOverlaysRaw`(不做 blocking 过滤)

### 问题

`checkPlaceWithOverlay`(cpp 原 3545–3562)对每个候选只返回 **blocking** 违例:

```cpp
if (touchesInstance(violation, request.instanceId))  → 保留
else if (!isOld /* containsViolation */)             → 保留
else                                                  → 丢弃(不上报)
```

即"不触及 target instance **且** 在 baseline 里存在"的违例被静默丢弃。这对
legalizer 的**放置接受**决策是对的(不是我引起的历史违例不关我事),但对
**repair 流程是错的**:

- repair 要修的 original 违例里,有一类 participants **不含 target**——opto 把
  cell 换 VT 后,它离开了原来的同色 run,残下来的 filler run 宽度不足产生 MW,
  这条 MW 的 instances 只有 filler。它在 baseline 里存在。一个**没真正修好它**
  的候选,这条 MW 仍在 → `touchesInstance`=false → 判为 old → **丢弃** →
  `violations` 空 → `isLegal=true` → repair 误以为修好了(**false accept**)。
- `containsViolation` 用 xWindow **包含** 判 old,"缩小但没消除"的违例(非单调
  场景)也会被隐藏。

### 改动

新增公开方法(header ~301,cpp ~3534,`[fillerRepair-fix]`),**不动**原
`checkPlaceWithOverlay/checkPlaceWithOverlays`(legalizer 继续用):

```cpp
std::vector<CheckResult> checkPlaceWithOverlaysRaw(
    const CheckRequest& request,
    const Rect& guardRegion,
    const std::vector<std::vector<FillerChange>>& fillerChanges) const;
```

逐候选:`validateOverlayRequest`(保留逐候选隔离)→ `checkOverlayRegion(...,
useNewFillers=true)` → **返回全部 guard 内违例,不做 blocking 过滤**。
`isLegal = violations 空 && 无诊断`。**不算 per-batch baseline**——engine 自己
发一个空 change-list 拿 baseline,original/new/halo 分类、multiset 一对一 delta
全部由 engine 侧完成(它已经实现好了)。

复用了你现成的 `checkOverlayRegion`,没有新逻辑,成本最低。

### 需要你确认

- 接口命名 / 位置是否 OK;是否愿意把它作为 repair 的正式入口。
- 或者你更想用别的形态(例如给违例打 `status="preexisting"` tag 而非新方法)——
  只要 repair 能拿到 guard 内**全量**违例即可,形态你定。

---

## 改动 2:填充 `Violation.rowIds`(原来恒为空)

### 问题

`Violation.rowIds`(header 240)和 `ScanOutcome.rowIds`(header 440)声明了但
**从未赋值**。后果:

1. `isInGuard`(cpp 3020)里 `if (rowIds.empty()) return true;` → **guard 裁剪
   退化为只按 x**,裁不了行方向。
2. `finishViolation`(cpp 3044)把 rowIds 喂进签名 hash → rowIds 空时,**同一 x
   间隙、不同行的两条违例 hash 相同**,`containsViolation` 的 hash 相等判断会
   过度匹配(与改动 1 的问题叠加)。
3. repair engine 的违例签名(区分 inter-row、避免误并)依赖 rowIds。

### 改动(4 处,均 `[fillerRepair-fix]`)

- **`scanRule`** 两个 `ScanOutcome` 构造点(cpp ~2309 无邻居 width、~2334 邻居
  循环):`outcome.rowIds = {checkTarget.rowId}` / `{checkTarget.rowId,
  neighbor.rowId}`。`ScanShape` 本就带 `rowId`。
- **`scanViolations`**(cpp ~2601):`violation.rowIds = outcome.rowIds;`
  (`finishViolation` 已 sortUnique)。
- **`makeViolations`**(快路径,cpp ~1979/2005):target 行来自
  `outcome.context.rowId`,neighbor 行来自 `neighbor->rowId`。快路径 repair 不
  直接用,一并补是为了 `Violation` 契约一致(两条路径的 hash 都带行)。

### 需要你确认

- 行来源取法是否符合你的模型(尤其 inter-row:我取 target 行 + neighbor 行两行;
  intra-row 取单行)。若某些规则的"涉及行"定义不同,请调整。

---

## 改动 3:修一个头/实现不一致(否则编译不过)

`validateOverlayRequest` 的**头声明是 3 参**(多一个未使用的
`const Rect& guardRegion`),但**cpp 定义和两个调用点都是 2 参**,body 里也没用
guard region。声明与定义不匹配 → 编译报"定义了未声明的成员" / 链接找不到 3 参
版本。已把头声明改成 2 参与定义一致(header ~583,`[fillerRepair-fix]`)。

如果你的本意是让 `validateOverlayRequest` 用 guard region(例如校验 guard-only
filler 不被改),那应改**定义**加参数并用上,而不是只留在声明里——请你定夺。

---

## 验证

- checker 需在你的完整环境(UDM + `infrastructure/Grid.h`)里编译:上述改动
  均为 additive + 类型一致,不涉及新依赖。
- 建议加两个用例:(a) bridge-MW false-accept——cell 换色后残留一条不触及
  target 的 MW,`checkPlaceWithOverlaysRaw` 应报出该违例、`isLegal=false`
  (旧 `checkPlaceWithOverlay` 会 isLegal=true);(b) 同 x 间隙不同行的两条违例,
  填 rowIds 后签名 hash 不同、`isInGuard` 能按行裁剪。
- filler-repair 纯 planner 测试(`src/fillerRepair/test/run_tests.sh`,60 个)不
  依赖 checker,已确认不受影响。

## 后续(engine 侧,非本文)

engine 对接时会写 adapter 做 `ipl::`↔`fillerRepair::` 类型换皮(colId·siteWidth
↔DBU、orientation、guard Rect↔Region),按 index 关联 raw 结果,从
`violation.instances`+`placedInsts` 合成 participants。依赖拓扑(checker 调
engine、engine 只经抽象 oracle 回调 checker,无环)见 spec §3.3。
