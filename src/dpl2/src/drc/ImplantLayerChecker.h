#pragma once

#include "drc/DRCChecker.h"
#include <techRuleCheck.hh>
#include <techObjTypes.hh>
#include <unl/unlObjTypes.hh>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using eUNL::PhysRow;
using eUNL::PhysDesMgr;
using eUTL::Rect;
using eUTL::PhysOrientationE;
using eUTL::PhysOrientation;

namespace dpl2 {
class Network;
class Master;
class fillerSetting;

namespace fillerRepair {
class FillerRepairEngine;
}

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

// [fillerRepair-fix] String literals are const char[N]; binding them to char*
// is ill-formed (-Werror=write-strings). Matches kRuleSourceNames below.
constexpr std::array<const char*, (unsigned) Relationship::Count>
  kRelationshipNames{"intra_row", "inter_row"};
inline std::string toString(Relationship rel)
{return std::string(kRelationshipNames[(unsigned)rel]);}

constexpr std::array<const char*, (unsigned) RuleSource::Count>
kRuleSourceNames{"WIDTH", "SPACING", "LEF58_WIDTH", "LEF58_SPACING"};
inline std::string toString(RuleSource source)
{return std::string(kRuleSourceNames[(unsigned)source]);}

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
      : id_(id), name_(name), vt_(vt), polar_(polar) {}

    ADD_SETTER_GETTER_PP(int, Id, id_);
    ADD_SETTER_GETTER_PP(eLIB::TechLayerRelativeID, TechLayerId, tlId_);
    ADD_SETTER_GETTER_PP(std::string, Name, name_);
    ADD_SETTER_GETTER_PP(Vt, Vt, vt_);
    ADD_SETTER_GETTER_PP(Polar, Polar, polar_);

private:
    LayerId id_ = 0;
    eLIB::TechLayerRelativeID tlId_; // desMgr->getTopTech().getTechLayer()
    std::string name_;
    Vt vt_ = Vt::Unknown;
    Polar polar_ = Polar::N;
};

class Rule
{
public:
    Rule() = default;
    Rule(int id, RuleSource rs, LayerId l1, Dbu v)
        : ruleId_(id), source_(rs), primaryLayer_(l1), minValue_(v) {}

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
        UnsupportedClauses, unsupportedClauses_);
    ADD_SETTER_GETTER_PP(std::optional<int>, ContainmentGroup, containmentGroup_);
    ADD_SETTER_GETTER_PP(std::vector<int>, ContainedByRuleIds, containedByRuleIds_);
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
    Rect rect;
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
    MasterId masterId = 0;  // Legacy field for test compatibility
    Dbu width = 0;
    Dbu height = 0;
    std::vector<MasterShape> shapes;     // rebuilt band shapes
    std::vector<MasterShape> rawShapes;   // original raw shapes (preserved input)
    Dbu siteHeight = 0;                  // site height from the master's site type
    bool isFiller = false;
    std::vector<MasterInterval> intervals;
};

struct SlotRef
{
    RowId rowId = 0;
    BandSlot bandSlot = BandSlot::Bottom;
};

struct Violation
{
    int ruleId = 0;
    RuleSource ruleSource = RuleSource::Width;
    LayerId primaryLayer = 0;
    std::optional<LayerId> secondaryLayer;
    std::vector<InstanceId> instances;
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

using LayerGroupMap = std::unordered_map<std::string, std::vector<LayerId>>;
using RowIdVec = std::vector<RowId>;
using FillerChanges = std::vector<FillerCellRecord>;
using ViolationVec = std::vector<Violation>;
using DiagVec = std::vector<Diagnostic>;

struct CheckResult
{
    bool isLegal = false;
    std::vector<Violation> violations;
    std::vector<Diagnostic> diagnostics;
};

struct UpdateResult
{
    bool success = true;
    std::vector<Diagnostic> diagnostics;
};

struct CheckShape
{
    int id = 0;
    RowId rowId = 0;
    BandSlot bandSlot = BandSlot::Bottom;
    LayerId layer = 0;
    XInterval x;
    std::vector<InstanceId> ownerInstanceIds;
    std::vector<ShapeId> ownerShapeIds;
    bool isCandidate = false;
};

struct CheckOutcome
{
    int ruleId = 0;
    RuleSource ruleSource = RuleSource::Width;
    LayerId primaryLayer = 0;
    std::optional<LayerId> secondaryLayer;
    Relationship relationship = Relationship::IntraRow;
    int targetId = 0;
    std::optional<int> neighborId;
    XInterval xWindow;
    XInterval targetX;  // Effective measured interval for width rules
    OutcomeStatus status = OutcomeStatus::NotApplicable;
    Dbu measuredValue = 0;
    Dbu requiredValue = 0;
    std::vector<InstanceId> instanceIds;
    std::vector<RowId> rowIds;
};

// Type aliases for commonly-used long types
using CheckShapes = std::vector<CheckShape>;
using CheckOutcomeVec = std::vector<CheckOutcome>;

class ImplantLayerChecker final : public DRCChecker
{
public:
    ImplantLayerChecker(Grid* grid, Network* network);
    ~ImplantLayerChecker();

    bool check(const Node* cell, GridX x, GridY y,
        const eUTL::PhysOrientation& orient) const override;

    // [fillerRepair-fix] Not an override: DRCChecker declares only the 4-arg
    // check(). This is the filler-repair-aware extension; the 4-arg override
    // above delegates to it with a local record vector, so callers dispatching
    // through DRCChecker* still reach this logic.
    bool check(const Node* cell,
        GridX x,
        GridY y,
        const eUTL::PhysOrientation& orient,
        std::vector<FillerCellRecord>& fcRecord) const;

    CheckResult checkDirect(const CheckRequest& request) const;
    std::vector<CheckResult> checkPlaceWithOverlays(const CheckRequest& request,
        const Rect& guardRegion,
        const std::vector<FillerChanges>& fillerChanges) const;

    std::vector<CheckResult> checkAllNodesDirect() const;

    void printStats(std::ostream& os, bool isShort) const;
    Dbu siteWidth() const {return siteWidth_;}
    const std::vector<std::unique_ptr<Node>>& getNodes() const;
    const std::vector<Layer>& getLayers() const {return layers_;}

    const std::vector<Diagnostic>& getDiags() const {return diagnostics_;}
    size_t mergedShapeCount() const;

    // Presets what lazy filler-repair initialization would otherwise obtain
    // from the registered provider (desMgr from this checker's init,
    // fillerSetting from set_filler_option). Call before check() in
    // harnesses that do not run under a DePlace owner.
    void setFillerRepairContext(PhysDesMgr* desMgr,
        const fillerSetting* setting);

    // Dependency inversion: the infrastructure owner (DePlace) registers how
    // to reach the active fillerSetting; the checker never names DePlace, so
    // builds without it still link.
    using FillerSettingProvider = const fillerSetting* (*)();
    static void setFillerRepairSettingProvider(FillerSettingProvider provider);

    friend class TestImplantCmd;
    friend class ImplantLayerCheckerHelper;

private:
    bool init(PhysDesMgr* desMgr);

    // Filler repair is lazy (most checks pass and never need it): the engine
    // is created and initialized on the first failing check. On a repairable
    // failure the repair records are APPENDED to the caller's fcRecord; the
    // checker keeps no filler-change member state.
    bool repairFillers(const CheckRequest& request,
        std::vector<FillerCellRecord>& fcRecord) const;

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

    struct OverlapInfo
    {
        std::set<InstanceId> fillers;
        std::optional<Diagnostic> diags;
    };

    struct Footprint
    {
        InstanceId instanceId = 0;
        MasterId masterId = 0;
        XInterval x;
    };

    // Type aliases for commonly-used long types
    using PlacedIntervals = std::vector<PlacedInterval>;
    using ScanRectVec = std::vector<ScanRect>;

    // init functions
    static void parseLayerName(const std::string& name,
        Layer::Vt& vt, Layer::Polar& polar);
    LayerId getLayerId(eLIB::TechLayerRelativeID relId) const;
    LayerId getLayerId(const std::string& name) const;
    void buildLayers(PhysDesMgr* desMgr);
    void buildTrackPattern();
    void rebuildMasterShapes();
    void buildMasters();
    bool buildInsts();


    // Shared initialization and geometry normalization.
    bool buildRules(const std::vector<Layer>& layers,
        const LayerGroupMap& groups, const std::vector<Rule>& rules);
    bool buildMstIntervals();

    PlacedIntervals instantiate(InstanceId instanceId,
        MasterId masterId, RowId rowId, Dbu x,
        PhysOrientation orientation, bool isCandidate) const;

    Layer::Polar slotPolar(RowId rowId, BandSlot bandSlot) const;
    SlotRef getAdjSlot(RowId rowId, BandSlot bandSlot) const;
    Layer::Polar getPolar(RowId rowA, RowId rowB) const;

    // Indexed check helpers (used by committed-state maintenance).
    CheckShapes mergeGroupShapes(const PlacedIntervals& intervals,
        bool isCandidate) const;

    std::vector<ScanRect> scanSnapshot(const CheckRequest& request) const;
    ScanRectVec scanSnapshot(const CheckRequest& request,
        const std::set<InstanceId>& excludedInstances) const;
    ScanRectVec scanOverlaySnapshot(const CheckRequest& request,
        const Rect& guardRegion, const FillerChanges& fillerChanges,
        bool useNewFillers, const std::set<InstanceId>& excludedInstances) const;
    ScanRectVec scanInst(InstanceId instanceId,
        MasterId masterId, RowId rowId, ColId colId,
        PhysOrientation orientation, bool isCandidate) const;
    CheckShapes scanShapes(const ScanRectVec& rects) const;
    CheckShapes scanNeighbors(const CheckShape& target,
        const Rule& rule, Relationship relationship,
        const CheckShapes& shapes) const;
    CheckOutcomeVec scanRule(const Rule& rule, const CheckShapes& shapes) const;
    ViolationVec scanViolations(const CheckOutcomeVec& outcomes,
        const CheckShapes& shapes) const;
    CheckResult checkPlaceWithOverlay(const CheckRequest& request,
        const Rect& guardRegion, const FillerChanges& fillerChanges,
        const ViolationVec& oldViolations) const;
    CheckResult checkOverlayRegion(const CheckRequest& request,
        const Rect& guardRegion, const FillerChanges& fillerChanges,
        bool useNewFillers) const;

    bool scanSlotPolarityOk(const ScanRect& rect) const;
    bool contained(const CheckOutcome& specific, const CheckOutcome& broad) const;
    bool scanIntersectCoverage(const Rule& rule,
        const CheckShape& target, const CheckShape& neighbor,
        const CheckShapes& shapes) const;

    OverlapInfo checkOverlap(const Node* node) const;
    bool slotPolarityOk(const PlacedInterval& interval) const;
    DiagVec validateOverlayRequest(const CheckRequest& request,
        const FillerChanges& fillerChanges) const;
    bool touchesInstance(const Violation& violation,
        InstanceId instanceId) const;
    bool containsViolation(const Violation& oldViolation,
        const Violation& newViolation) const;
    bool isInGuard(const XInterval& xWindow, const RowIdVec& rowIds,
        const Rect& guard) const;
    void finishViolation(Violation& violation) const;
    bool scanGroupFails(const Rule& rule,
        const CheckShape& target, const CheckShape& neighbor,
        Relationship relationship, const XInterval& xWindow,
        const CheckShapes& shapes) const;
    bool ruleAppliesTo(const Rule& rule,
        Relationship relationship) const;
    Dbu queryRadius(const Rule& rule) const;

    std::vector<Layer> layers_;
    std::vector<Rule> rules_;
    LayerGroupMap groups_;
    // polarity at bottom band of row 0; polarity alternates per row.
    Layer::Polar basePolar_ = Layer::Polar::P;
    Dbu rowHeight_ = 0;
    Dbu siteWidth_ = 1;

    Network* network_ = nullptr;
    std::vector<MasterItem> masterItems_;

    // Shared normalized data.
    RuleIndex ruleIndex_; // Layers, rules, groups, and query radii.
    std::vector<Diagnostic> diagnostics_; // Initialization diagnostics.
    std::unordered_map<std::string, GroupId> groupIds_; // Active group ids.
    std::unordered_map<GroupId, std::vector<LayerId>> groupLayers_; // Group members.
    // Layer-to-groups map.
    std::unordered_map<LayerId, std::vector<GroupId>> layerGroups_;

    // Fast-check state.
    mutable int nextIntervalId_ = 1; // Next committed interval id.
    mutable int nextCandIntervalId_ = -1; // Temporary candidate interval ids.
    int nextShapeId_ = 1; // Next committed merged-shape id.
    mutable int nextCandShapeId_ = -1; // Temporary candidate shape ids.


    std::map<eLIB::TechLayerRelativeID, LayerId> techLayerToIdx_;
    std::map<std::string, LayerId> layerNameToIdx_;
    const std::array<Relationship, 2> relations_ =
    {Relationship::IntraRow, Relationship::InterRow};

    // Filler repair (lazy). desMgr_ is remembered by init(); the setting
    // comes from setFillerRepairContext() or DePlace::get() at first use.
    PhysDesMgr* desMgr_ = nullptr;
    const fillerSetting* repairSetting_ = nullptr;
    mutable std::unique_ptr<fillerRepair::FillerRepairEngine> repairEngine_;
    mutable bool repairEngineFailed_ = false;
};

} // namespace ipl
} // namespace dpl2