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
#include <libObjAccessor.hh>
#include <timlib/libCell.hh>
#include <unl/unlObjTypes.hh>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
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
class fillerSetting;
class Network;
class Master;
namespace fillerRepair {
class FillerRepairEngine;
}

// replace holders for filler masters with new master ids
enum class OpType : uint8_t {
  Replace = 0,
  Delete = 1,
  Add = 2,
};

struct FillerCellRecord {
  OpType op_;
  LeafCellID cell_id_;
  UvDist origin_x_;
  UvDist origin_y_;
  LibCellID orig_lib_cell_;
  // [fillerRepair-fix] renamed from new_cell_id_: the cpp consumes
  // change.new_lib_cell_ (validateOverlayRequest / overlay scan).
  LibCellID new_lib_cell_;
};

namespace ipl {
class TestImplantCmd;
class ImplantLayerCheckerHelper;

using Dbu = int32_t;
using LayerId = int32_t;
using MasterId = int32_t;
using InstanceId = int32_t;
using ShapeId = int32_t;
using RowId = int32_t;
using ColId = int32_t;
using GroupId = int32_t;

enum class BandSlot {Bottom, Top};
enum class RuleSource {Width, Spacing, Lef58Width, Lef58Spacing, Count};
enum class RuleDirection {Any, Horizontal, Vertical};
enum class Relationship {IntraRow, InterRow, Count};
enum class OutcomeStatus {Satisfied, Violated, NotApplicable, Skipped};
enum class ViolationType {AllStdCell, AllFiller, Mixed};

constexpr std::array<const char*, (unsigned)Relationship::Count>
    kRelationshipNames{"intra_row", "inter_row"};
inline std::string toString(Relationship rel) {return std::string(kRelationshipNames[(unsigned)rel]);}

constexpr std::array<const char*, (unsigned)RuleSource::Count> kRuleSourceNames{"WIDTH", "SPACING", "LEF58_WIDTH", "LEF58_SPACING"};
inline std::string toString(RuleSource source) {return std::string(kRuleSourceNames[(unsigned)source]);}

struct XInterval
{
  Dbu xl = 0;
  Dbu xh = 0;
};

// Local rect type with Dbu coordinates for checker-internal use.
// Avoids incompatibilities with eUTL::Rect (whose members are UvDist).
struct CheckerRect
{
  Dbu xl = 0;
  Dbu yl = 0;
  Dbu xh = 0;
  Dbu yh = 0;
};

class Layer
{
 public:
  enum class Vt {S, L, H, UL, Unknown};
  enum class Polar {N, P};

  Layer() = default;
  Layer(LayerId id, const std::string& name, Vt vt, Polar polar)
      : id_(id), name_(name), vt_(vt), polar_(polar)
  {
  }

  ADD_SETTER_GETTER_PP(int, Id, id_);
  ADD_SETTER_GETTER_PP(eLIB::TechLayerRelativeID, TechLayerId, tlId_);
  ADD_SETTER_GETTER_PP(std::string, Name, name_);
  ADD_SETTER_GETTER_PP(Vt, Vt, vt_);
  ADD_SETTER_GETTER_PP(Polar, Polar, polar_);

 private:
  LayerId id_ = 0;
  eLIB::TechLayerRelativeID tlId_;
  std::string name_;
  Vt vt_ = Vt::Unknown;
  Polar polar_ = Polar::N;
};

class Rule
{
 public:
  Rule() = default;
  Rule(int id, RuleSource rs, LayerId l1, Dbu v)
      : ruleId_(id), source_(rs), primaryLayer_(l1), minValue_(v)
  {
  }

  ADD_SETTER_GETTER_PP(int, RuleId, ruleId_);
  ADD_SETTER_GETTER_PP(RuleSource, Source, source_);
  ADD_SETTER_GETTER_PP(LayerId, PrimaryLayer, primaryLayer_);
  ADD_SETTER_GETTER_PP(std::optional<LayerId>, SecondaryLayer, secondaryLayer_);
  ADD_SETTER_GETTER_PP(Dbu, MinValue, minValue_);
  ADD_SETTER_GETTER_PP(RuleDirection, Direction, direction_);
  ADD_SETTER_GETTER_PP(std::optional<Dbu>, Prl, prl_);
  ADD_SETTER_GETTER_PP(bool, ZeroPrl, zeroPrl_);
  ADD_SETTER_GETTER_PP(bool, ExceptAbutted, exceptAbutted_);
  ADD_SETTER_GETTER_PP(bool, ExceptCornerTouch, exceptCornerTouch_);
  ADD_SETTER_GETTER_PP(std::optional<Dbu>, Length, length_);
  ADD_SETTER_GETTER_PP(std::optional<std::string>, CheckGroup, checkGroup_);
  ADD_SETTER_GETTER_PP(std::vector<LayerId>, IntersectLayers, intersectLayers_);
  ADD_SETTER_GETTER_PP(std::vector<std::string>,
                       UnsupportedClauses,
                       unsupportedClauses_);
  ADD_SETTER_GETTER_PP(std::optional<int>, ContainmentGroup, containmentGroup_);
  ADD_SETTER_GETTER_PP(std::vector<int>,
                       ContainedByRuleIds,
                       containedByRuleIds_);
  ADD_SETTER_GETTER_PP(int, SpecificityRank, specificityRank_);

 private:
  int ruleId_ = 0;
  RuleSource source_ = RuleSource::Width;
  LayerId primaryLayer_ = 0;
  std::optional<LayerId> secondaryLayer_;
  Dbu minValue_ = 0;
  RuleDirection direction_ = RuleDirection::Any;
  std::optional<Dbu> prl_;
  bool zeroPrl_ = false;
  bool exceptAbutted_ = false;
  bool exceptCornerTouch_ = false;
  std::optional<Dbu> length_;
  std::optional<std::string> checkGroup_;
  std::vector<LayerId> intersectLayers_;
  std::vector<std::string> unsupportedClauses_;
  std::optional<int> containmentGroup_;
  std::vector<int> containedByRuleIds_;
  int specificityRank_ = 0;
};

struct MasterShape
{
  MasterId masterId = 0;
  ShapeId shapeId = 0;
  LayerId layer = 0;
  ::Rect rect;  // eUTL::Rect via using eUTL::Rect
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

struct MasterItem
{
  MasterId masterId = 0; //Legacy field for test purposes, to be removed in future
  Dbu width = 0;
  Dbu height = 0;
  std::vector<MasterShape> shapes;     // rebuilt band shapes (output of rebuildMasterShapes)
  std::vector<MasterShape> rawShapes;  // original raw shapes (preserved input)
  Dbu siteHeight = 0;                  // site height from the master's site type
  bool isFiller = false;
  std::vector<MasterInterval> intervals;
};

struct SlotRef
{
  RowId rowId = 0;
  BandSlot bandSlot = BandSlot::Bottom;
};

struct TrackPattern
{
  std::map<std::pair<RowId, BandSlot>, LayerId> layerBySlot;
  std::map<std::pair<RowId, RowId>, Layer::Polar> activeKindByBoundary;

  std::optional<LayerId> layerForSlot(RowId rowId, BandSlot bandSlot) const;
  std::vector<SlotRef> adjacentSlots(RowId rowId, BandSlot bandSlot) const;
  std::optional<Layer::Polar> activeInterRowKind(RowId rowA,
                                                 RowId rowB) const;
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
  Dbu measuredValue = 0;
  Dbu requiredValue = 0;
  XInterval xWindow;
  Relationship relationship = Relationship::IntraRow;
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

// Tye aliases for namespace level types
using LayerGroupMap = std::unordered_map<std::string, std::vector<LayerId>>;
using RowIdVec = std::vector<RowId>;
using FillerChanges = std::vector<FillerCellRecord>;
using ViolationVec = std::vector<Violation>;
using DiagVec = std::vector<Diagnostic>;

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
  ImplantLayerChecker(Grid* grid, Network* network);
  ~ImplantLayerChecker();

  // Enable the optional filler-repair path owned by this checker. Callers
  // construct only ImplantLayerChecker; its check() method invokes the engine
  // when implant legality needs a filler overlay repair.
  bool initFillerRepair(PhysDesMgr* desMgr,
                        const fillerSetting& fillerSettings);
  // Rebuilds checker/repair snapshots after infrastructure has synchronized
  // Network with UDM. Does not update Network Nodes.
  bool updateFillerRepair(PhysDesMgr* desMgr,
                          const fillerSetting& fillerSettings);
  CheckResult precheckFillerRepair() const;
  void setFillerRepairDebugLogging(bool enabled);

  bool check(const Node* cell, GridX x, GridY y, const PhysOrientation& orient) const override;

  // Cleared at the start of every check(). A successful check with no repair
  // returns an empty list; a successful repaired check returns the exact
  // atomic overlay that opto/infrastructure must commit with its target edit.
  const FillerChanges& getFillerChanges() const { return fillerChanges_; }
  const DiagVec& getFillerRepairDiagnostics() const
  {
    return fillerRepairDiagnostics_;
  }

  CheckResult checkPlace(const CheckRequest& request) const;
  CheckResult checkDirect(const CheckRequest& request) const;
  std::vector<CheckResult> checkPlaceWithOverlays(const CheckRequest& request, const Rect& guardRegion, const std::vector<FillerChanges>& fillerChanges) const;
  
  void printStats(std::ostream& os) const;
  Dbu siteWidth() const {return siteWidth_;}
  const std::vector<std::unique_ptr<Node>>& getNodes() const;
  const std::vector<Layer>& getLayers() const {return layers_;}
  
  UpdateResult commitPlace(const CommitRequest& request);
  const std::vector<Diagnostic>& getDiags() const {return diagnostics_;}
  size_t mergedShapeCount() const;

  friend class TestImplantCmd;
  friend class ImplantLayerCheckerHelper;

 private:
  bool init(PhysDesMgr* desMgr);

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
    std::unordered_map<LayerId, Layer> layers;
    std::vector<Rule> rules;
    std::unordered_map<int, Rule> ruleById;
    std::unordered_map<std::string, std::vector<LayerId>> groups;
  };

  enum class CheckMode {Candidate, Committed};

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
    Relationship relationship = Relationship::IntraRow;
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
    Dbu measuredValue = 0;
    Dbu requiredValue = 0;
    std::vector<InstanceId> instanceIds;
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

  //type aliases for commonly used vectors of the above types
  using MergedShapes = std::vector<MergedShape>;
  using PlacedIntervals = std::vector<PlacedInterval>;
  using ScanRectVec = std::vector<ScanRect>;
  using ScanShapeVec = std::vector<ScanShape>;
  using ScanOutcomeVec = std::vector<ScanOutcome>;
  using RuleOutcomeVec = std::vector<RuleOutcome>;

  //Helper methods for 
  static void parseLayerName(const std::string& name,
                             Layer::Vt& vt,
                             Layer::Polar& polar);
  LayerId findLayerId(eLIB::TechLayerRelativeID relId) const;
  void buildTrackPattern();
  void rebuildMasterShapes();
  
  //build masterItems_ from Network (relies on Master::getPhysLibCell())
  // must be called after layers_ and tracks_ are populated
  void buildMasters();

  // Shared initialization and geometry normalization.
  bool buildRules(const std::vector<Layer>& layers,
                  const LayerGroupMap& groups,
                  const std::vector<Rule>& rules);
  bool buildMstIntervals();
  bool buildPlacedInst(const Node* node, RowId rowId, Dbu x);

  PlacedIntervals instantiate(InstanceId instanceId, MasterId masterId, RowId rowId, Dbu x, PhysOrientation orientation, bool isCandidate) const;

  // Fast indexed check.
  MergedShapes mergeShapes(const PlacedIntervals& intervals, bool isCandidate) const;
  MergedShapes mergeSortedShapes(const PlacedIntervals& intervals, bool isCandidate) const;
  MergedShapes mergeGroupShapes(const PlacedIntervals& intervals, bool isCandidate) const;

  MergedShapes findNeighbors(const MergedShape& target, const Rule& rule, Relationship relationship, CheckMode mode, const MergedShapes& targetShapes, const std::set<InstanceId>& excludedInstances) const;

  RuleOutcomeVec evalRule(const Rule& rule, const MergedShapes& targetShapes, CheckMode mode, const std::set<InstanceId>& excludedInstances) const;

  ViolationVec makeViolations(const RuleOutcomeVec& outcomes) const;

  bool isSameCommittedPose(const CheckRequest& request) const;
  MergedShapes committedTargetShapes(InstanceId instanceId) const;

  // Direct scan check.
  std::vector<ScanRect> scanSnapshot(const CheckRequest& request) const;
  ScanRectVec scanSnapshot(const CheckRequest& request, const std::set<InstanceId>& excludedInstances) const;
  ScanRectVec scanOverlaySnapshot(const CheckRequest& request, const Rect& guardRegion, const FillerChanges& fillerChanges, bool useNewFillers, const std::set<InstanceId>& excludedInstances) const;
  ScanRectVec scanInst(InstanceId instanceId, MasterId masterId, RowId rowId, ColId colId, PhysOrientation orientation, bool isCandidate) const;
  ScanShapeVec scanShapes(const ScanRectVec& rects) const;
  ScanShapeVec scanNeighbors(const ScanShape& target, const Rule& rule, Relationship relationship, const ScanShapeVec& shapes) const;
  ScanOutcomeVec scanRule(const Rule& rule, const ScanShapeVec& shapes) const;
  ViolationVec scanViolations(const ScanOutcomeVec& outcomes) const;
  CheckResult checkPlaceWithOverlay(const CheckRequest& request, const Rect& guardRegion, const FillerChanges& fillerChanges, const ViolationVec& oldViolations) const;
  CheckResult checkOverlayRegion(const CheckRequest& request, const Rect& guardRegion, const FillerChanges& fillerChanges, bool useNewFillers) const;

  bool scanSlotPolarityOk(const ScanRect& rect) const;
  bool scanContained(const ScanOutcome& specific,
                     const ScanOutcome& broad) const;
  bool scanIntersectCoverage(const Rule& rule, const ScanShape& target, const ScanShape& neighbor, const ScanShapeVec& shapes) const;

  // Committed-state maintenance.
  void rebuildShapes();
  void rebuildBuckets(const std::set<BucketKey>& buckets);
  void rebuildGroupBuckets(const std::set<GroupKey>& groups);
  void insertIntervals(const std::vector<PlacedInterval>& intervals);
  void removeInstance(InstanceId instanceId);
  void insertFootprint(const Node* node, RowId rowId, Dbu x, PhysOrientation orientation);
  void removeFootprint(InstanceId instanceId);
  BucketKey bucketFor(const PlacedInterval& interval) const;
  std::set<BucketKey> bucketsForInstance(InstanceId instanceId) const;
  std::set<BucketKey> bucketsForIntervals(const PlacedIntervals& intervals) const;
  std::set<GroupKey> groupKeysForBucket(const BucketKey& key) const;
  void eraseShapeRefs(const MergedShape& shape);

  const MergedShape* findShape(int mergedShapeId) const;
  std::optional<Diagnostic> overlapDiag(const Node* node, RowId rowId, Dbu x, std::optional<InstanceId> excludedInstanceId) const;
  OverlapInfo overlapInfo(
      const CheckRequest& request) const;
  bool slotPolarityOk(const PlacedInterval& interval) const;
  bool isFillerInstance(const Node* node) const;
  DiagVec validateOverlayRequest(const CheckRequest& request, const FillerChanges& fillerChanges) const;
  bool touchesInstance(const Violation& violation,
                       InstanceId instanceId) const;
  bool containsViolation(const Violation& oldViolation,
                         const Violation& newViolation) const;
  bool isInGuard(const XInterval& xWindow, const RowIdVec& rowIds, const Rect& guard) const;
  void finishViolation(Violation& violation) const;
  bool isContainedContext(const RuleContext& specific,
                          const RuleContext& broad) const;
  bool hasIntersectCoverage(const Rule& rule, const MergedShape& target, const MergedShape& neighbor) const;
  bool groupFails(const Rule& rule, const MergedShape& target, const MergedShape& neighbor, Relationship relationship, const XInterval& xWindow, const std::set<InstanceId>& excludedInstances) const;
  bool scanGroupFails(const Rule& rule, const ScanShape& target, const ScanShape& neighbor, Relationship relationship, const XInterval& xWindow, const ScanShapeVec& shapes) const;
  bool ruleAppliesTo(const Rule& rule,
                     Relationship relationship) const;
  Dbu queryRadius(const Rule& rule) const;

  std::vector<Layer> layers_;
  std::vector<Rule> rules_;
  LayerGroupMap groups_;
  std::vector<RowId> rows_;
  TrackPattern tracks_;
  Dbu rowHeight_ = 0;
  Dbu siteWidth_ = 1;

  //infras
  Network* network_ = nullptr;

  //Implant data per Master, indexed by MasterId( aligned with network::masters)
  std::vector<MasterItem> masterItems_;  // Master shapes and intervals.

  // Shared normalized data.
  RuleIndex ruleIndex_;  // Layers, rules, groups, and query radii.
  std::vector<Diagnostic> diagnostics_;  // Initialization diagnostics.
  std::unordered_map<std::string, GroupId> groupIds_;  // Active group ids.
  std::unordered_map<GroupId, std::vector<LayerId>> groupLayers_;  // Group members.
  std::unordered_map<LayerId, std::vector<GroupId>> layerGroups_;  // Layer-to-groups map.

  // Fast-check state.
  std::map<BucketKey, std::vector<PlacedInterval>> rowIndex_;  // Committed raw intervals.
  std::map<BucketKey, std::vector<MergedShape>> shapeIndex_;  // Committed merged shapes.
  std::map<GroupKey, std::vector<MergedShape>> groupIndex_;  // Active group merged shapes.
  std::map<RowId, std::vector<Footprint>> footprintIndex_;  // Row occupancy.
  std::unordered_map<InstanceId, std::vector<RowId>> footRowsByInst_;  // Occupied rows.
  std::unordered_map<InstanceId, std::vector<int>> instIntervals_;  // Instance raw interval ids.
  std::unordered_map<InstanceId, std::vector<int>> instShapes_;  // Instance merged-shape ids.
  std::unordered_map<int, MergedShape> shapeById_;  // Merged-shape id lookup.
  std::unordered_map<int, PlacedInterval> intervalById_;  // Raw interval id lookup.
  mutable int nextIntervalId_ = 1;  // Next committed interval id.
  mutable int nextCandIntervalId_ = -1;  // Temporary candidate interval ids.
  int nextMergedShapeId_ = 1;  // Next committed merged-shape id.
  mutable int nextCandShapeId_ = -1;  // Temporary candidate shape ids.

  std::map<eLIB::TechLayerRelativeID, LayerId> techLayerToCheckerId_;

  std::unique_ptr<fillerRepair::FillerRepairEngine> fillerRepairEngine_;
  bool fillerRepairDebugLogging_ = false;
  mutable FillerChanges fillerChanges_;
  mutable DiagVec fillerRepairDiagnostics_;
};

}  // namespace ipl
}  // namespace dpl2
