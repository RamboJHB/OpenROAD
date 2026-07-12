#pragma once

#include "drc/DRCChecker.h"
#include <phys/fpManager.hh>
#include <phys/physDesMgr.hh>
#include <phys/physHier.hh>
#include <physHierImpl.hh>
#include <physLibMgr.hh>
#include <techRuleCheck.hh>
#include <techObjTypes.hh>
#include <util/iter.hh>
#include <fnlObjTypes.hh>
#include <LibObjAccessor.hh>
#include <timlib/libCell.hh>
#include <unl/unlObjTypes.hh>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using eUNL::PhysBlockage;
using eUNL::PhysRow;
using eUNL::PhysCell;
using eUNL::LeafCellID;
using eUNL::LibCellID;
using eUNL::PhysCellImpl;
using eUNL::PhysPin;
using eUNL::PhysSWire;
using eUNL::PhysShape;
using eUNL::PhysObjStatus;
using eUNL::PhysDesMgr;
using eUTL::uvRIter;
using eLIB::ShapeUsageE;
using eLIB::TechLayerID;
using eLIB::TechLayer;
using eLIB::TechShape;
using eLIB::PhysLib;
using eLIB::PhysLibObs;
using eLIB::PhysLibCell;
using eLIB::TechSite;
using eUTL::UvDist;
using eUTL::Rect;
using eUTL::Point2D;
using eUTL::PhysOrientationE;
using eUTL::PhysOrientation;

namespace dpl2 {
namespace ipl {

class TestImplantCmd;

using Dbu = int32_t;
using LayerId = int32_t;
using MasterId = int32_t;
using InstanceId = int32_t;
using ShapeId = int32_t;
using RowId = int32_t;
using ColId = int32_t;
using GroupId = int32_t;

enum class BandSlot { Bottom, Top };
enum class Polarity { N, P };
enum class Family { VTS, VTL, VTH, VTUL, Unknown };
enum class RuleSource { Width, Spacing, Lef58Width, Lef58Spacing, Count };
enum class RuleDirection { Any, Horizontal, Vertical };

constexpr std::array<const char*, static_cast<size_t>(RuleSource::Count)>
    kRuleSourceNames{
        "WIDTH",
        "SPACING",
        "LEF58_WIDTH",
        "LEF58_SPACING",
    };

inline std::string toString(RuleSource source)
{
  const auto index = static_cast<size_t>(source);
  if (index >= kRuleSourceNames.size()) {
    return "UNKNOWN";
  }
  return std::string(kRuleSourceNames[index]);
}

struct XInterval
{
  Dbu xl = 0;
  Dbu xh = 0;
};

struct ImplantLayer
{
  LayerId id = 0;
  std::string name;
  Family family = Family::Unknown;
  Polarity polarity = Polarity::N;
};

struct Rule
{
  int ruleId = 0;
  RuleSource source = RuleSource::Width;
  LayerId primaryLayer = 0;
  std::optional<LayerId> secondaryLayer;
  Dbu minValue = 0;
  RuleDirection direction = RuleDirection::Any;
  std::optional<Dbu> prl;
  bool zeroPrl = false;
  bool exceptAbutted = false;
  bool exceptCornerTouch = false;
  std::optional<Dbu> length;
  std::optional<std::string> checkGroup;
  std::vector<LayerId> intersectLayers;
  std::vector<std::string> unsupportedClauses;
  std::optional<int> containmentGroup;
  std::vector<int> containedByRuleIds;
  int specificityRank = 0;
};

struct MasterShape
{
  MasterId masterId = 0;
  ShapeId shapeId = 0;
  LayerId layer = 0;
  ::Rect rect;
};

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

struct MasterInput
{
  MasterId masterId = 0;
  Dbu width = 0;
  Dbu height = 0;
  std::vector<MasterShape> shapes;
  std::vector<MasterShape> rawShapes;
  Dbu siteHeight = 0;
  bool isFiller = false;
  std::vector<MasterInterval> intervals;
};

struct PlacedInst
{
  InstanceId instanceId = 0;
  MasterId masterId = 0;
  RowId rowId = 0;
  ColId colId = 0;
  PhysOrientation orientation = PhysOrientationE::R0;
  bool isFiller = false;
};

struct SlotRef
{
  RowId rowId = 0;
  BandSlot bandSlot = BandSlot::Bottom;
};

struct TrackPattern
{
  std::map<std::pair<RowId, BandSlot>, LayerId> layerBySlot;
  std::map<std::pair<RowId, RowId>, Polarity> activeKindByBoundary;

  std::optional<LayerId> layerForSlot(RowId rowId, BandSlot bandSlot) const;
  std::vector<SlotRef> adjacentSlots(RowId rowId, BandSlot bandSlot) const;
  std::optional<Polarity> activeInterRowKind(RowId rowA, RowId rowB) const;
};

struct ImplantInput
{
  std::vector<ImplantLayer> layers;
  std::vector<Rule> rules;
  std::unordered_map<std::string, std::vector<LayerId>> groups;
  std::vector<MasterInput> masters;
  std::vector<PlacedInst> placedInsts;
  std::vector<RowId> rows;
  TrackPattern tracks;
  Dbu rowHeight = 0;
  Dbu siteWidth = 0;
};

struct CheckerRect
{
  Dbu xl = 0;
  Dbu yl = 0;
  Dbu xh = 0;
  Dbu yh = 0;
};

enum class Relationship { IntraInstance, IntraRow, InterRow, Count };
enum class OutcomeStatus { Satisfied, Violated, NotApplicable, Skipped };

constexpr std::array<const char*, static_cast<size_t>(Relationship::Count)>
    kRelationshipNames{
        "intra_instance",
        "intra_row",
        "inter_row",
    };

inline std::string toString(Relationship relationship)
{
  const auto index = static_cast<size_t>(relationship);
  if (index >= kRelationshipNames.size()) {
    return "unknown";
  }
  return std::string(kRelationshipNames[index]);
}

struct Violation
{
  int ruleId = 0;
  RuleSource ruleSource = RuleSource::Width;
  LayerId primaryLayer = 0;
  std::optional<LayerId> secondaryLayer;
  std::vector<InstanceId> instances;
  std::vector<ShapeId> shapeIds;
  std::vector<int> mergedShapeIds;
  Dbu measuredValue = 0;
  Dbu requiredValue = 0;
  XInterval xWindow;
  Relationship relationship = Relationship::IntraInstance;
  std::string status = "detected";
  std::string layerName;
  XInterval targetInterval;
  XInterval neighborInterval;
  std::vector<RowId> rowIds;
  uint64_t hash = 0;
  std::string toString(Dbu siteWidth = 1) const;
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
  ColId colId = 0;
  PhysOrientation orientation = PhysOrientationE::R0;
};

struct FillerChange
{
  InstanceId instanceId = 0;
  MasterId newMasterId = 0;
};

struct CommitRequest
{
  CheckRequest place;
};

struct CheckResult
{
  bool isLegal = true;
  std::vector<Violation> violations;
  std::vector<Diagnostic> diagnostics;
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
  bool check(const Node* cell,
             GridX x,
             GridY y,
             const PhysOrientation& orient) const override;
  CheckResult checkPlace(const CheckRequest& request) const;
  CheckResult checkDirect(const CheckRequest& request) const;
  std::vector<CheckResult> checkPlaceWithOverlays(
      const CheckRequest& request,
      const Rect& guardRegion,
      const std::vector<std::vector<FillerChange>>& fillerChanges) const;
  UpdateResult commitPlace(const CommitRequest& request);

  const std::vector<Diagnostic>& initDiagnostics() const;
  const std::map<InstanceId, PlacedInst>& placedInsts() const;
  Dbu siteWidth() const;
  size_t mergedShapeCount() const;

  const std::vector<ImplantLayer>& layers() const { return layers_; }
  const std::vector<Rule>& rules() const { return rules_; }
  const std::vector<MasterInput>& masters() const { return masters_; }
  const std::vector<RowId>& rows() const { return rows_; }
  const TrackPattern& tracks() const { return tracks_; }

  void printInitSummary(std::ostream& os) const;
  static std::string inputToString(const ImplantInput& data);
  static std::string toString(RuleSource source);
  static std::string toString(Relationship relationship);
  bool dump(const std::string& filePath) const;
  bool load(const std::string& filePath);

  friend class TestImplantCmd;

 private:
  bool initFromUDM(const PhysDesMgr& desMgr);

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
    Dbu measuredValue = 0;
    Dbu requiredValue = 0;
  };

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
    Dbu measuredValue = 0;
    Dbu requiredValue = 0;
    std::vector<InstanceId> instances;
    std::vector<ShapeId> shapeIds;
    std::vector<RowId> rowIds;
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

  static void parseLayerName(const std::string& name,
                             Family& family,
                             Polarity& polarity);
  LayerId findLayerId(eLIB::TechLayerRelativeID relId) const;
  void buildTrackPattern();
  void rebuildMasterShapes();

  bool buildRules(const std::vector<ImplantLayer>& layers,
                  const std::unordered_map<std::string, std::vector<LayerId>>& groups,
                  const std::vector<Rule>& rules);
  bool buildMasters(const std::vector<MasterInput>& masters);
  bool buildPlaced(const std::vector<PlacedInst>& placedInsts);
  bool buildPlacedInst(InstanceId instanceId,
                       MasterId masterId,
                       RowId rowId,
                       Dbu x,
                       PhysOrientation orientation,
                       bool isFiller);

  std::vector<PlacedInterval> instantiate(InstanceId instanceId,
                                          MasterId masterId,
                                          RowId rowId,
                                          Dbu x,
                                          PhysOrientation orientation,
                                          bool isCandidate) const;
  std::vector<MergedShape> mergeShapes(
      const std::vector<PlacedInterval>& intervals,
      bool isCandidate) const;
  std::vector<MergedShape> mergeSortedShapes(
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
      const std::vector<MergedShape>& targetShapes,
      const std::set<InstanceId>& excludedInstances) const;
  std::vector<RuleOutcome> evaluate(
      const Rule& rule,
      const std::vector<MergedShape>& targetShapes,
      CheckMode mode,
      const std::set<InstanceId>& excludedInstances) const;
  std::vector<Violation> makeViolations(
      const std::vector<RuleOutcome>& outcomes) const;
  bool isSameCommittedPose(const CheckRequest& request) const;
  std::vector<MergedShape> committedTargetShapes(InstanceId instanceId) const;

  std::vector<ScanRect> scanSnapshot(const CheckRequest& request) const;
  std::vector<ScanRect> scanSnapshot(
      const CheckRequest& request,
      const std::set<InstanceId>& excludedInstances) const;
  std::vector<ScanRect> scanOverlaySnapshot(
      const CheckRequest& request,
      const Rect& guardRegion,
      const std::vector<FillerChange>& fillerChanges,
      bool useNewFillers,
      const std::set<InstanceId>& excludedInstances) const;
  std::vector<ScanRect> scanInst(const PlacedInst& instance,
                                 bool isCandidate) const;
  std::vector<ScanShape> scanShapes(const std::vector<ScanRect>& rects) const;
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
  CheckResult checkPlaceWithOverlay(
      const CheckRequest& request,
      const Rect& guardRegion,
      const std::vector<FillerChange>& fillerChanges,
      const std::vector<Violation>& oldViolations) const;
  CheckResult checkOverlayRegion(
      const CheckRequest& request,
      const Rect& guardRegion,
      const std::vector<FillerChange>& fillerChanges,
      bool useNewFillers) const;

  bool scanSlotPolarityOk(const ScanRect& rect) const;
  bool scanContained(const ScanOutcome& specific,
                     const ScanOutcome& broad) const;
  bool scanIntersectCoverage(const Rule& rule,
                             const ScanShape& target,
                             const ScanShape& neighbor,
                             const std::vector<ScanShape>& shapes) const;

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
      Dbu x,
      std::optional<InstanceId> excludedInstanceId) const;
  OverlapInfo overlapInfo(const CheckRequest& request) const;
  bool slotPolarityOk(const PlacedInterval& interval) const;
  bool isFillerInstance(const PlacedInst& instance) const;
  bool isFillerMaster(MasterId masterId) const;
  std::vector<Diagnostic> validateOverlayRequest(
      const CheckRequest& request,
      const Rect& guardRegion,
      const std::vector<FillerChange>& fillerChanges) const;
  bool touchesInstance(const Violation& violation,
                       InstanceId instanceId) const;
  bool containsViolation(const Violation& oldViolation,
                         const Violation& newViolation) const;
  bool isInGuard(const XInterval& violation,
                 const std::vector<RowId>& rowIds,
                 const Rect& guard) const;
  void finishViolation(Violation& violation) const;
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
  bool scanSameRowTouchingShape(
      const ScanShape& target,
      const Rule& rule,
      const std::vector<ScanShape>& shapes) const;
  bool ruleAppliesTo(const Rule& rule, Relationship relationship) const;
  Dbu queryRadius(const Rule& rule) const;

  std::vector<ImplantLayer> layers_;
  std::vector<Rule> rules_;
  std::unordered_map<std::string, std::vector<LayerId>> groups_;
  std::vector<MasterInput> masters_;
  mutable std::vector<PlacedInst> placedInsts_;
  std::vector<RowId> rows_;
  TrackPattern tracks_;
  Dbu rowHeight_ = 0;
  Dbu siteWidth_ = 1;

  RuleIndex ruleIndex_;
  std::vector<Diagnostic> diagnostics_;
  std::map<InstanceId, PlacedInst> instances_;
  std::unordered_map<std::string, GroupId> groupIds_;
  std::unordered_map<GroupId, std::vector<LayerId>> groupLayers_;
  std::unordered_map<LayerId, std::vector<GroupId>> layerGroups_;

  std::map<BucketKey, std::vector<PlacedInterval>> rowIndex_;
  std::map<BucketKey, std::vector<MergedShape>> shapeIndex_;
  std::map<GroupKey, std::vector<MergedShape>> groupIndex_;
  std::map<RowId, std::vector<Footprint>> footprintIndex_;
  std::unordered_map<InstanceId, std::vector<RowId>> footRowsByInst_;
  std::unordered_map<InstanceId, std::vector<int>> instIntervals_;
  std::unordered_map<InstanceId, std::vector<int>> instShapes_;
  std::unordered_map<int, MergedShape> shapeById_;
  std::unordered_map<int, PlacedInterval> intervalById_;
  mutable int nextIntervalId_ = 1;
  mutable int nextCandIntervalId_ = -1;
  int nextMergedShapeId_ = 1;
  mutable int nextCandShapeId_ = -1;

  std::map<eLIB::TechLayerRelativeID, LayerId> techLayerToCheckerId_;
  std::unordered_map<MasterId, size_t> masterIdToIndex_;
};

}  // namespace ipl
}  // namespace dpl2
