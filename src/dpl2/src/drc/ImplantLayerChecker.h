#pragma once

#include "drc/DRCChecker.h"
#include "drc/ImplantLayerCheckerHelper.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace dpl2 {
namespace ipl {

// Local rect type with DbCoord coordinates for checker-internal use.
// Avoids incompatibilities with eUTL::Rect (whose members are UvDist).
struct CheckerRect
{
  DbCoord xl = 0;
  DbCoord yl = 0;
  DbCoord xh = 0;
  DbCoord yh = 0;
};

enum class Relationship { IntraInstance, IntraRow, InterRow };
enum class OutcomeStatus { Satisfied, Violated, NotApplicable, Skipped };

// ===========================================================================
// Filler VT overlay repair — spec section 5 interface (added).
// -----------------------------------------------------------------------------
// These types are the wire contract between the checker and the filler repair
// engine (docs/filler_vt_overlay_repair_spec.md, section 5). They are ADDED
// alongside the existing checker types; nothing here is removed. The struct
// Violation and struct CheckResult below are EVOLVED (extra fields added,
// old fields kept) so the legacy checkPlace/checkDirect/commitPlace path and
// the new overlay path can share them.
// ===========================================================================

// spec 5.1: MW/MS as a stable enum so the repair side never depends on
// RuleSource string/kind mapping.
enum class ViolationKind { MinWidth, MinSpacing };

// spec 5.1: intra-instance / intra-row / inter-row as a stable enum. Mirrors
// the legacy Relationship enum but is the repair-facing name.
enum class ViolationRelation { IntraInstance, IntraRow, InterRow };

// spec 5.1: a participant of a violation. rowId distinguishes same-x /
// different-row participants; isFiller / isTarget tag the role.
struct ViolationParticipant
{
  InstanceId instanceId = 0;
  MasterId masterId = 0;
  RowId rowId = 0;  // row containing this participant instance/shape
  XInterval xRange;
  bool isFiller = false;
  bool isTarget = false;
};

// spec 5.2: one filler master swap. Repair engine's V1 wire format.
struct FillerChange
{
  InstanceId instanceId = 0;  // filler instance only
  MasterId newMasterId = 0;   // same w/h/orient-compatible filler master
};

struct Violation
{
  int ruleId = 0;
  RuleSource ruleSource = RuleSource::Width;
  LayerId primaryLayer = 0;
  std::optional<LayerId> secondaryLayer;
  std::vector<InstanceId> instances;
  std::vector<ShapeId> shapeIds;
  std::vector<int> mergedShapeIds;
  DbCoord measuredValue = 0;
  DbCoord requiredValue = 0;
  XInterval xWindow;
  Relationship relationship = Relationship::IntraInstance;
  std::string status = "detected";
  std::string layerName;
  XInterval targetInterval;
  XInterval neighborInterval;

  // --- spec 5.1 fields (added for the repair engine) -----------------------
  // kind/relation mirror ruleSource/relationship in the repair-facing enums.
  // primaryLayer/secondaryLayer above already carry the implant layer(s) that
  // the repair signature uses to tell P-band from N-band at the same x gap.
  ViolationKind kind = ViolationKind::MinWidth;
  ViolationRelation relation = ViolationRelation::IntraRow;
  // Sorted unique. Inter-row reports all touched rows; intra-row reports one.
  std::vector<RowId> rowIds;
  std::vector<ViolationParticipant> participants;

  std::string toString(DbCoord siteWidth = 1) const;
};

struct Diagnostic
{
  std::string status;
  std::string message;
};

struct CheckRequest
{
  InstanceId instanceId = 0;
  MasterId masterId = 0;
  RowId rowId = 0;
  DbCoord x = 0;
  PhysOrientation orientation = PhysOrientationE::R0;
};

// spec 5.1: the changed std-cell anchor. Same fields as CheckRequest, which
// the spec explicitly allows keeping as the implementation name. Aliased so
// the overlay API reads as the spec does.
using TargetPlace = CheckRequest;

struct CommitRequest
{
  CheckRequest place;
};

// spec 5.2: status of one overlay check.
enum class CheckStatus
{
  Checked,        // check completed; isLegal/violations are meaningful
  InvalidOverlay, // request malformed (bad fillerChange, dup instance, ...)
  Unsupported,    // overlay shape not supported by this checker
  CheckerError    // internal failure
};

using OverlayRequestId = int;

// spec 5.2: one atomic overlay candidate. All fillerChanges are applied
// together, then re-checked; violations are collected at least within
// guardRegion (the repair window expanded by a two-cell guard halo).
struct OverlayCheckRequest
{
  OverlayRequestId requestId = -1;  // repair-engine generated, unique per batch
  TargetPlace targetPlace;
  CheckerRect guardRegion;  // spec's `Rect`; DbCoord to avoid UvDist coupling
  std::vector<FillerChange> fillerChanges;
};

struct CheckResult
{
  bool isLegal = true;
  std::vector<Violation> violations;
  std::vector<Diagnostic> diagnostics;

  // --- spec 5.2 fields (added for the overlay API) -------------------------
  // requestId MUST echo the OverlayCheckRequest.requestId; status reports
  // whether the check completed. Legacy checkPlace/checkDirect leave these at
  // their defaults (requestId = -1, status = Checked).
  OverlayRequestId requestId = -1;
  CheckStatus status = CheckStatus::Checked;
};

struct UpdateResult
{
  bool success = true;
  std::vector<Diagnostic> diagnostics;
};

class ImplantLayerChecker final : public DRCChecker
{
 public:
  explicit ImplantLayerChecker(Grid* grid);
  ~ImplantLayerChecker();

  bool initialize(const ImplantInput& input);
  bool dump(const std::string& filePath) const;
  bool load(const std::string& filePath);

  bool check(const Node* cell,
             GridX x,
             GridY y,
             const PhysOrientation& orient) const override;

  CheckResult checkPlace(const CheckRequest& request) const;
  CheckResult checkDirect(const CheckRequest& request) const;
  UpdateResult commitPlace(const CommitRequest& request);

  // spec 5.2: non-mutating overlay DRC verify for the filler repair engine.
  // checkPlaceWithOverlay applies one atomic overlay (all fillerChanges
  // together) and returns one CheckResult echoing requestId. The batch form
  // takes a vector of independent requests; one invalid request only affects
  // its own result, and correctness does not depend on return order.
  //
  // NOTE: these are currently STUB implementations (see .cpp) -- the real DRC
  // is not wired through them yet. They exist so the repair engine can be
  // built against the spec 5 interface.
  CheckResult checkPlaceWithOverlay(const OverlayCheckRequest& request) const;
  std::vector<CheckResult> checkPlaceWithOverlays(
      const std::vector<OverlayCheckRequest>& requests) const;

  const std::vector<Diagnostic>& initDiagnostics() const;
  const std::vector<PlacedInst>& placedInsts() const;
  DbCoord siteWidth() const;
  size_t mergedShapeCount() const;

  ImplantLayerCheckerHelper* getHelper() { return helper_; }

  static std::string toString(RuleSource source);
  static std::string toString(Relationship relationship);

 private:
  // Shared keys and indexes.
  struct BucketKey
  {
    RowId rowId = 0;
    BandSlot bandSlot = BandSlot::Bottom;
    LayerId layer = 0;

    bool operator<(const BucketKey& other) const;
  };

  struct GroupKey
  {
    RowId rowId = 0;
    BandSlot bandSlot = BandSlot::Bottom;
    GroupId groupId = 0;

    bool operator<(const GroupKey& other) const;
  };

  struct RuleIndex
  {
    std::unordered_map<LayerId, ImplantLayer> layers;
    std::vector<Rule> rules;
    std::unordered_map<int, Rule> ruleById;
    std::unordered_map<std::string, std::vector<LayerId>> groups;
  };

  enum class CheckMode { Candidate, Committed };

  struct MasterInterval
  {
    MasterId masterId = 0;
    ShapeId shapeId = 0;
    LayerId layer = 0;
    int rowOffset = 0;
    BandSlot bandSlot = BandSlot::Bottom;
    XInterval x;
    bool runtimeCheckable = true;
    std::string skipReason;
  };

  struct PlacedInterval
  {
    int intervalId = 0;
    InstanceId instanceId = 0;
    MasterId masterId = 0;
    ShapeId shapeId = 0;
    LayerId layer = 0;
    RowId rowId = 0;
    BandSlot bandSlot = BandSlot::Bottom;
    XInterval x;
    bool isCandidate = false;
  };

  struct MergedShape
  {
    int mergedShapeId = 0;
    RowId rowId = 0;
    BandSlot bandSlot = BandSlot::Bottom;
    LayerId layer = 0;
    XInterval x;
    std::vector<int> ownerIntervalIds;
    std::vector<InstanceId> ownerInstanceIds;
    std::vector<ShapeId> ownerShapeIds;
    bool isCandidate = false;
  };

  struct RuleContext
  {
    RuleSource ruleKind = RuleSource::Width;
    LayerId primaryLayer = 0;
    std::optional<LayerId> secondaryLayer;
    Relationship relationship = Relationship::IntraInstance;
    int targetMergedShapeId = 0;
    std::optional<int> neighborMergedShapeId;
    RowId rowId = 0;
    BandSlot bandSlot = BandSlot::Bottom;
    XInterval xWindow;
    XInterval targetX;
  };

  struct RuleOutcome
  {
    int ruleId = 0;
    RuleContext context;
    OutcomeStatus status = OutcomeStatus::NotApplicable;
    DbCoord measuredValue = 0;
    DbCoord requiredValue = 0;
  };

  // Direct-check shapes built by scanning current committed instances.
  struct ScanRect
  {
    InstanceId instanceId = 0;
    MasterId masterId = 0;
    ShapeId shapeId = 0;
    LayerId layer = 0;
    RowId rowId = 0;
    BandSlot bandSlot = BandSlot::Bottom;
    CheckerRect rect;
    bool isCandidate = false;
  };

  struct ScanShape
  {
    int shapeId = 0;
    LayerId layer = 0;
    RowId rowId = 0;
    BandSlot bandSlot = BandSlot::Bottom;
    CheckerRect bbox;
    std::vector<InstanceId> ownerInstanceIds;
    std::vector<ShapeId> ownerShapeIds;
    bool containsCandidate = false;
  };

  struct ScanOutcome
  {
    int ruleId = 0;
    RuleSource ruleSource = RuleSource::Width;
    LayerId primaryLayer = 0;
    std::optional<LayerId> secondaryLayer;
    Relationship relationship = Relationship::IntraRow;
    int targetShapeId = 0;
    std::optional<int> neighborShapeId;
    XInterval xWindow;
    OutcomeStatus status = OutcomeStatus::NotApplicable;
    DbCoord measuredValue = 0;
    DbCoord requiredValue = 0;
    std::vector<InstanceId> instanceIds;
    std::vector<ShapeId> shapeIds;
  };

  struct OverlapInfo
  {
    std::set<InstanceId> removableFillers;
    std::optional<Diagnostic> blockingOverlap;
  };

  struct Footprint
  {
    InstanceId instanceId = 0;
    MasterId masterId = 0;
    XInterval x;
  };

  // Shared initialization and geometry normalization.
  bool buildRules(const ImplantInput& input);
  bool buildMasters(const ImplantInput& input);
  bool buildPlaced(const ImplantInput& input);

  std::vector<PlacedInterval> instantiate(InstanceId instanceId,
                                          MasterId masterId,
                                          RowId rowId,
                                          DbCoord x,
                                          PhysOrientation orientation,
                                          bool isCandidate) const;

  // Fast indexed check.
  std::vector<MergedShape> mergeShapes(
      const std::vector<PlacedInterval>& intervals,
      bool isCandidate) const;
  std::vector<MergedShape> mergeCandidateShapes(
      const std::vector<PlacedInterval>& intervals,
      bool isCandidate) const;
  std::vector<MergedShape> mergeGroupShapes(
      const std::vector<PlacedInterval>& intervals,
      bool isCandidate) const;

  std::vector<MergedShape> findNeighbors(
      const MergedShape& target,
      const Rule& rule,
      Relationship relationship,
      CheckMode mode,
      const std::set<InstanceId>& excludedInstances) const;

  std::vector<RuleOutcome> evalRule(
      const Rule& rule,
      const std::vector<MergedShape>& targetShapes,
      CheckMode mode,
      const std::set<InstanceId>& excludedInstances) const;

  std::vector<Violation> makeViolations(
      const std::vector<RuleOutcome>& outcomes) const;

  bool isSameCommittedPose(const CheckRequest& request) const;
  std::vector<MergedShape> committedTargetShapes(InstanceId instanceId) const;

  // Direct scan check.
  std::vector<ScanRect> scanSnapshot(const CheckRequest& request) const;
  std::vector<ScanRect> scanSnapshot(
      const CheckRequest& request,
      const std::set<InstanceId>& excludedInstances) const;
  std::vector<ScanRect> scanInst(const PlacedInst& instance,
                                 bool isCandidate) const;
  std::vector<ScanShape> scanShapes(
      const std::vector<ScanRect>& rects) const;
  std::vector<ScanShape> scanNeighbors(
      const ScanShape& target,
      const Rule& rule,
      Relationship relationship,
      const std::vector<ScanShape>& shapes) const;
  std::vector<ScanOutcome> scanRule(
      const Rule& rule,
      const std::vector<ScanShape>& shapes) const;
  std::vector<Violation> scanViolations(
      const std::vector<ScanOutcome>& outcomes) const;

  bool scanSlotPolarityOk(const ScanRect& rect) const;
  bool scanContained(const ScanOutcome& specific,
                     const ScanOutcome& broad) const;
  bool scanIntersectCoverage(const Rule& rule,
                             const ScanShape& target,
                             const ScanShape& neighbor,
                             const std::vector<ScanShape>& shapes) const;
  bool scanSameRowTouchingShape(
      const ScanShape& target,
      const Rule& rule,
      const std::vector<ScanShape>& shapes) const;

  // Committed-state maintenance.
  void rebuildShapes();
  void rebuildBuckets(const std::set<BucketKey>& buckets);
  void rebuildGroupBuckets(const std::set<GroupKey>& groups);
  void insertIntervals(const std::vector<PlacedInterval>& intervals);
  void removeInstance(InstanceId instanceId);
  void insertFootprint(const PlacedInst& instance);
  void removeFootprint(InstanceId instanceId);
  BucketKey bucketFor(const PlacedInterval& interval) const;
  std::set<BucketKey> bucketsForInstance(InstanceId instanceId) const;
  std::set<BucketKey> bucketsForIntervals(
      const std::vector<PlacedInterval>& intervals) const;
  std::set<GroupKey> groupKeysForBucket(const BucketKey& key) const;
  void eraseShapeRefs(const MergedShape& shape);

  const MergedShape* findShape(int mergedShapeId) const;
  std::optional<Diagnostic> overlapDiag(
      InstanceId instanceId,
      MasterId masterId,
      RowId rowId,
      DbCoord x,
      std::optional<InstanceId> excludedInstanceId) const;
  OverlapInfo overlapInfo(
      const CheckRequest& request) const;
  bool slotPolarityOk(const PlacedInterval& interval) const;
  bool isFillerInstance(const PlacedInst& instance) const;
  bool layerPolarityMatches(LayerId layer, Polarity polarity) const;
  bool isContainedContext(const RuleContext& specific,
                          const RuleContext& broad) const;
  bool hasIntersectCoverage(const Rule& rule,
                            const MergedShape& target,
                            const MergedShape& neighbor) const;
  bool groupFails(const Rule& rule,
                  const MergedShape& target,
                  const MergedShape& neighbor,
                  Relationship relationship,
                  const XInterval& xWindow,
                  const std::set<InstanceId>& excludedInstances) const;
  bool scanGroupFails(const Rule& rule,
                      const ScanShape& target,
                      const ScanShape& neighbor,
                      Relationship relationship,
                      const XInterval& xWindow,
                      const std::vector<ScanShape>& shapes) const;

  bool hasSameRowTouch(
      const MergedShape& target,
      const Rule& rule,
      const std::set<InstanceId>& excludedInstances) const;
  bool ruleAppliesTo(const Rule& rule,
                     Relationship relationship) const;
  DbCoord queryRadius(const Rule& rule) const;

  // Shared normalized data.
  ImplantLayerCheckerHelper* helper_ = nullptr;
  RuleIndex ruleIndex_;  // Layers, rules, groups, and query radii.
  ImplantInput input_;   // Original input plus committed placement edits.
  TrackPattern tracks_;  // Row-band layer and active-boundary pattern.
  std::vector<Diagnostic> diagnostics_;  // Initialization diagnostics.
  std::unordered_map<InstanceId, PlacedInst> instances_;  // Current placements.
  std::unordered_map<MasterId, DbCoord> masterWidths_;    // Master width lookup.
  std::unordered_map<MasterId, DbCoord> masterHeights_;   // Master height lookup.
  std::unordered_map<MasterId, bool> masterIsFiller_;     // Master filler lookup.
  std::unordered_map<std::string, GroupId> groupIds_;     // Active group ids.
  std::unordered_map<GroupId, std::vector<LayerId>> groupLayers_;  // Group members.
  std::unordered_map<LayerId, std::vector<GroupId>> layerGroups_;  // Layer-to-groups map.
  DbCoord rowHeight_ = 0;  // Placement row height.
  DbCoord siteWidth_ = 1;  // Site pitch for x/grid conversion.

  // Fast-check state.
  std::map<MasterId, std::vector<MasterInterval>> masterCache_;  // Master row-band intervals.
  std::map<BucketKey, std::vector<PlacedInterval>> rowIndex_;    // Committed raw intervals.
  std::map<BucketKey, std::vector<MergedShape>> shapeIndex_;     // Committed merged shapes.
  std::map<GroupKey, std::vector<MergedShape>> groupIndex_;      // Active grouped shapes.
  std::map<RowId, std::vector<Footprint>> footprintIndex_;       // Row occupancy.
  std::unordered_map<InstanceId, std::vector<RowId>> footRowsByInst_;  // Occupied rows.
  std::unordered_map<InstanceId, std::vector<int>> instIntervals_;  // Instance raw interval ids.
  std::unordered_map<InstanceId, std::vector<int>> instShapes_;  // Instance merged-shape ids.
  std::unordered_map<int, MergedShape> shapeById_;  // Merged-shape id lookup.
  std::unordered_map<int, PlacedInterval> intervalById_;  // Raw interval id lookup.
  mutable int nextIntervalId_ = 1;      // Next committed interval id.
  mutable int nextCandIntervalId_ = -1;  // Temporary candidate interval ids.
  int nextMergedShapeId_ = 1;           // Next committed merged-shape id.
  mutable int nextCandShapeId_ = -1;    // Temporary candidate shape ids.
};

}  // namespace ipl
}  // namespace dpl2
