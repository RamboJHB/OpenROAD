# Checker 改动说明 — 供 filler-repair engine 对接(给 checker RD)

对象:`ImplantLayerChecker.{h,cpp}`。背景:filler-repair engine 会消费 overlay
接口的结果来做 VT 修复(把周围 filler 换型消 implant MW/MS 违例)。审查你交付的
overlay 实现后,先发现 **2 条契约问题** 会让 repair 得到错误的"干净"判断,外加
**1 个头/实现不一致的编译问题**;真实 checker harness 又暴露出额外编译漂移与
guard overlay 扫描错误。均已按最小方式修正,**所有 checker 改动都带
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

## 改动 4:对齐其余 header/cpp 编译漂移

standalone harness 首次直接编译生产 checker 后又发现:

- header 声明 `evaluate`,cpp/调用点使用 `evalRule`;
- header 的 `findNeighbors` 多一个 cpp 不接受的 `targetShapes` 参数;
- `ScanOutcome` header 为 `instances`,cpp 使用 `instanceIds`;
- dump/load 把 `vector<RowId>` 当成不存在的 `RowInput`。

已以 cpp 实际实现与 `ImplantInput::rows` 的真实类型为准对齐,均为命名/类型修复,
不改变规则语义。

## 改动 5:guard overlay 的 target/neighbor/merge 语义

真实 dense 测试暴露两个相互关联的 false-clean/false-violation 问题:

1. `scanOverlaySnapshot` 把 guard 内所有 committed rect 标成 candidate,而
   `scanNeighbors` 跳过 candidate → inter-row 与 spacing 找不到任何 committed
   neighbor,产生 false clean。
2. candidate 与 committed 的同层接触 interval 在 `scanShapes` 被强制分成不同
   merged shape → 物理上连续的 implant run 被捏造成窄宽/零间距 violation。

修正后的语义:只把真实 target/changed filler 标为 candidate;committed context
按 guard 裁剪但保持 non-candidate;guard 内所有 shape 都可作为规则 target/neighbor;
同 row/slot/layer 且接触的 interval 无论 provenance 都合并,合并后的
`containsCandidate` 只作为 OR 元数据。blocking API 仍在末端过滤 old violation;
raw API 返回 guard 内全量 finding。

review 后补的两个修正(同属本节语义,均 `[fillerRepair-fix]`):

1. **快照按"guard + 最大规则半径"外扩纳入,结果仍按精确 guard 裁剪**
   (`scanOverlaySnapshot` 的 `paddedGuard`,margin = max over rules of
   `queryRadius` + 纵向一行)。原来的精确-guard 裁快照会把跨 guard 边界的
   implant run **截断**,在断口上捏造 width violation(测试里 guard 两侧各出
   3 条幽灵违例)、并丢失 guard 外一步之遥的 spacing neighbor。外扩后断口
   only 出现在 padded 边缘,离精确 guard 至少一个 radius,结果裁剪必然丢弃。
2. **方向/band 重复折叠移到两条路径共享**:guard-wide 扫描从 pair 两端各
   访问一次、同层双 band 会把一条物理 run 报两次;原折叠代码只放在
   `makeViolations`(fast path,并不产生这种重复),`scanViolations`(真正
   产生重复的 scan path)反而没有。现抽成 `sortAndDedupViolations` 两处共用,
   排序比较器补了 hash/participants tiebreak 保证重复相邻、`std::unique`
   必然可见。契约:raw API 对一条物理违例恰好返回一条 finding
   (`BlockingHidesResidualButRawReports` 用精确计数锁死)。

## 改动 6:fake-UDM 编译边界

新增 test-only `DPL2_FAKE_UDM`:构造器不自动访问 Session/DB,
`initFromUDM` 返回 false;测试随后显式 `initialize(ImplantInput)`。这只替代缺失的
UDM extraction/helper,实际 overlay/index/rule/violation 代码仍直接编译自生产
`ImplantLayerChecker.cpp`。真实 UDM extraction 仍待 checker RD/UDM 环境接管。

---

## 验证

- `src/dpl2/src/drc/test/run_tests.sh`:`-Werror`,4 个 dense overlay、
  raw/baseline/rowIds、planner/checker 类型共存、invalid-batch 隔离、
  row/hash/guard 回归,8/8 全绿。
- `SANITIZE=address ./run_tests.sh`:8/8 全绿。
- fillerRepair planner:`src/fillerRepair/test/run_tests.sh`,79/79 全绿（含
  adaptive-L1 #8 的逐步扩窗、耦合行与 cutoff 回归，以及 planner hardening
  覆盖）。
- 完整 UDM extraction 与全 dpl2 build 仍需真实 UDM/其余 infrastructure 文件;
  当前 harness 明确只 fake DB boundary。
- 后续建议补 bridge-MW false-accept:cell 换色后残留一条不触及 target 的 MW,
  `checkPlaceWithOverlaysRaw` 应报出该违例、`isLegal=false`
  (旧 `checkPlaceWithOverlay` 会 isLegal=true)。同 x 不同行的 rowIds/hash/
  guard 裁剪回归已由 standalone harness 覆盖。
- filler-repair 纯 planner 测试(`src/fillerRepair/test/run_tests.sh`,79 个)不
  依赖 checker,已确认不受影响。

## 后续(engine 侧,非本文)

engine 对接时会写 adapter 做 `ipl::`↔`fillerRepair::` 类型换皮(colId·siteWidth
↔DBU、orientation、guard Rect↔Region),按 index 关联 raw 结果,从
`violation.instances`+`placedInsts` 合成 participants。依赖拓扑(checker 调
engine、engine 只经抽象 oracle 回调 checker,无环)见 spec §3.3。
