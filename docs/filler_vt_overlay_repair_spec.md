};

struct FillerChange
{
    InstanceId instanceId = 0;
    MasterId newMasterId = 0;
};

struct OverlayCandidate
{
    int candidateId = -1;
    std::vector<FillerChange> fillerChanges;
};

enum class CheckStatus
{
    Checked,
    InvalidOverlay,
    Unsupported,
    CheckerError
};

struct CheckResult
{
    int candidateId = -1;
    CheckStatus status = CheckStatus::CheckerError;
    bool isLegal = false;
    std::vector<Violation> violations;
    std::vector<Diagnostic> diagnostics;
};

struct OverlayCheckRequest
{
    TargetPlace targetPlace;
    std::vector<OverlayCandidate> candidates;
};

std::vector<CheckResult> checkPlaceWithOverlays(
    const OverlayCheckRequest& request) const;
```

`Violation` 推荐结构:

```cpp
enum class ViolationKind
{
    MinWidth,
    MinSpacing
};

enum class ViolationRelation
{
    IntraInstance,
    IntraRow,
    InterRow
};

struct ViolationParticipant
{
    InstanceId instanceId = 0;
    MasterId masterId = 0;
    RowId rowId = 0;
    XInterval xRange;
    bool isFiller = false;
    bool isTarget = false;
};

struct Violation
{
    int ruleId = 0;
    ViolationKind kind = ViolationKind::MinWidth;
    ViolationRelation relation = ViolationRelation::IntraRow;
    LayerId primaryLayer = 0;
    std::optional<LayerId> secondaryLayer;
    std::vector<RowId> rowIds;
    XInterval xWindow;
    DbCoord measuredValue = 0;
    DbCoord requiredValue = 0;
    std::vector<ViolationParticipant> participants;
};
```

checker 侧必须确认的语义:

- 返回结果顺序与 `request.candidates` 输入顺序一一对应。
- `candidateId` 只用于 debug echo,不靠裸指针关联。
- 一个 candidate 内必须支持多个 `FillerChange`。
- 单个 invalid/unsupported candidate 只影响自己的 `CheckResult`。
- `status != Checked` 时必须返回 diagnostics。
- `status == Checked && isLegal == false` 时尽量返回 violations 或 diagnostics。
- `status == Checked && isLegal == true && violations` 非空时,repair 仍会按不 clean 拒绝。
- checker 不需要维护历史 violation,也不需要标注 original/fixed/residual/new/spillover。
- 可选未来扩展:`OverlayCheckRequest` 加 `std::optional<Rect> guardRegion` 用于边界/spillover
  collect,但 first version 可以不做。

### 12.2 给 Infrastructure RD

需要 infrastructure RD 提供 100% utility precheck:

```cpp
struct SiteCoverageRequest
{
    std::vector<RowId> rowIds;  // empty means all legal std-cell rows
};

enum class CoverageIssueKind
{
    Gap,
    Overlap,
    OffGrid,
    IllegalOccupant
};

struct CoverageIssue
{
    CoverageIssueKind kind = CoverageIssueKind::Gap;
    RowId rowId = 0;
    DbCoord xLo = 0;
    DbCoord xHi = 0;
    int siteCount = 0;
    std::vector<InstanceId> instances;
};

struct SiteCoverageResult
{
    bool isFullUtility = false;
    std::vector<CoverageIssue> issues;
    std::vector<Diagnostic> diagnostics;
};

SiteCoverageResult checkFullSiteCoverage(
    const SiteCoverageRequest& request) const;
```

需要 infrastructure RD 提供 filler/master 查询:

```cpp
struct FillerMasterInfo
{
    MasterId masterId = 0;
    DbCoord width = 0;
    DbCoord height = 0;
    DbCoord siteHeight = 0;
    Family family = Family::Unknown;
    bool isFiller = false;
};

struct EditableFiller
{
    InstanceId instanceId = 0;
    MasterId currentMasterId = 0;
    RowId rowId = 0;
    DbCoord x = 0;
    DbCoord width = 0;
    DbCoord height = 0;
    PhysOrientation orientation = PhysOrientationE::R0;
    Family currentFamily = Family::Unknown;
    std::vector<MasterId> legalReplacementMasters;
};

struct FillerQuery
{
    std::vector<RowId> rowIds;
    DbCoord xLo = 0;
    DbCoord xHi = 0;
};

std::vector<EditableFiller> collectEditableFillers(
    const FillerQuery& query) const;

const FillerMasterInfo* getFillerMasterInfo(
    MasterId masterId) const;

std::vector<MasterId> getLegalReplacementMasters(
    InstanceId fillerInstanceId) const;
```

infrastructure 侧必须确认的语义:

- `checkFullSiteCoverage` 检查所有 legal std-cell sites 是否被 std cell 或 filler 精确覆盖一次。
- gap/overlap/off-grid/illegal occupant 都要作为 precondition failure 返回。
- macro/blockage/core cutout 等非 legal std-cell site 由 infrastructure 排除。
- `collectEditableFillers` 只返回 filler instance。
- `legalReplacementMasters` 只返回 filler master,不包含当前 master。
- replacement 必须 same width / same height / same site compatibility / orientation-compatible。
