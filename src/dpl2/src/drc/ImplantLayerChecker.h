#pragma once
#include <dpl2/DRCChecker.h>
#include <infrastructure/network.h>
#include <techRuleCheck.hh>
#include <techObjTypes.hh>
#include <unl/unlObjTypes.hh>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
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
using FillerChanges = std::vector<CellChangeRecord>;

enum class BandSlot {Bottom, Top};
enum class RuleSource {Width, Spacing, Lef58Width, Lef58Spacing, Count};
enum class RuleDirection {Any, Horizontal, Vertical};
enum class Relationship {IntraRow, InterRow, Count};
enum class OutcomeStatus {Satisfied, Violated, NotApplicable, Skipped};
enum class ViolationType {AllStdCell, AllFiller, Mixed};

constexpr std::array<const char*, (unsigned) Relationship::Count>kRelationshipNames{"intra_row", "inter_row"};
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

class Layer
{
public:
    enum class Vt {S, L, H, UL, Unknown};
    enum class Polar {N, P};

    Layer() = default;
    Layer(LayerId id, const std::string& name, Vt vt, Polar polar) :
      id_(id), name_(name), vt_(vt), polar_(polar) {}

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

    bool isWidth() const;
    bool isSpacing() const;
    bool contains(Relationship rlt) const;

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
    std::vector<MasterShape> shapes;      // rebuilt band shapes
    std::vector<MasterShape> rawShapes;   // original raw shapes (preserved input)
    Dbu siteHeight = 0;                   // site height from the master's site type
    bool isFiller = false;
    std::vector<MasterInterval> intervals;
};

struct SlotRef
{
    RowId rowId = 0;
    BandSlot bandSlot = BandSlot::Bottom;
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

struct OverlapInfo
{
    std::set<InstanceId> fillers;
    std::optional<Diagnostic> diags;
};

// Public pre-commit wire. The temporary Node is the proposed std cell; the
// Delete overlays name either one committed std cell or every filler exactly
// covered by a newly inserted buffer.
struct CheckRequest
{
    const Node* cell = nullptr;
    GridX x{0};
    GridY y{0};
    PhysOrientation orientation = PhysOrientationE::R0;
    std::vector<CellChangeRecord> overlayChanges;
};

struct CheckResult
{
    bool isLegal = false;
    std::vector<Violation> violations;
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

using RowIdVec = std::vector<RowId>;
using DiagVec = std::vector<Diagnostic>;
using CheckShapes = std::vector<CheckShape>;

class ImplantLayerChecker final : public DRCChecker
{
public:
    ImplantLayerChecker(Grid* grid, eUNL::Design* design, Network* network);
    ~ImplantLayerChecker();

    bool check(const Node* cell, GridX x, GridY y,
        const eUTL::PhysOrientation& orient) const override;
    bool check(const Node* cell, GridX x, GridY y,
        const eUTL::PhysOrientation& orient,
        std::vector<CellChangeRecord>& cellChanges,
        std::vector<CellChangeRecord>& overlayChanges) const override;

    // Fixed before publication. Runtime callers select direct or
    // repair-capable behavior by check overload; checker helpers disable
    // repair permanently.
    void setFillerRepairEnabled(bool enabled)
    {
        enableFillerRepair_.store(enabled, std::memory_order_release);
    }
    bool isFillerRepairEnabled() const
    {
        return enableFillerRepair_.load(std::memory_order_acquire);
    }

    CheckResult checkDirect(const CheckRequest& request) const;
    std::vector<CheckResult> checkPlaceWithOverlays(
        const CheckRequest& request, const Rect& guardRegion,
        const std::vector<FillerChanges>& fillerChanges) const;

    std::vector<CheckResult> checkAllNodesDirect() const;
    std::vector<Violation> getUniqueViolations(
        const std::vector<Violation>& violations);

    void printStats(std::ostream& os, bool isShort) const;
    Dbu siteWidth() const {return siteWidth_;}
    int getMaxRuleValue() const {return maxRuleValue_;}
    void setMaxRuleValue();
    const std::map<int, std::unique_ptr<Node>>& getNodes() const
    {
        static const std::map<int, std::unique_ptr<Node>> empty;
        return network_ != nullptr ? network_->getNodes() : empty;
    }
    const std::map<MasterId, MasterItem>& getMasterItems() const
    {return masterItems_;}
    const std::vector<Layer>& getLayers() const {return layers_;}
    Grid* getGrid() const { return grid_; }
    Network* getNetwork() const { return network_; }
    eUNL::Design* getDesign() const { return design_; }

    const std::vector<Diagnostic>& getDiags() const {return diagnostics_;}
    size_t mergedShapeCount() const;

    friend class TestImplantCmd;
    friend class ImplantLayerCheckerHelper;

private:
    // init functions
    bool init(PhysDesMgr* desMgr);

    bool hasUsableInfrastructure() const;
    const MasterItem* masterItem(MasterId masterId) const;
    DiagVec validateCheckRequest(const CheckRequest& request) const;
    std::set<InstanceId> overlayNodeIds(const CheckRequest& request) const;
    const fillerRepair::FillerRepairEngine* fillerRepairEngine() const;
    bool repairOverlay(const CheckRequest& request,
        std::vector<CellChangeRecord>& fillerChanges) const;
    void buildLayers(PhysDesMgr* desMgr);
    static void parseLayerName(const std::string& name,
        Layer::Vt& vt, Layer::Polar& polar);
    bool buildRules();
    void buildMasters();
    void rebuildMasterShapes();
    bool buildMstIntervals();

    LayerId getLayerId(eLIB::TechLayerRelativeID relId) const;
    LayerId getLayerId(const std::string& name) const;
    Layer::Polar slotPolar(RowId rowId, BandSlot bandSlot) const;
    SlotRef getAdjSlot(RowId rowId, BandSlot bandSlot) const;
    Layer::Polar getPolar(RowId rowA, RowId rowB) const;

    bool slotPolarityOk(const CheckShape& shape) const;
    bool contained(const CheckOutcome& specific, const CheckOutcome& broad) const;
    bool isIntersectCoverage(const Rule& rule, const CheckShape& target,
        const CheckShape& neighbor, const CheckShapes& shapes) const;

    CheckShapes getSnapshot(const CheckRequest& request,
        const std::set<InstanceId>& excludedNodes) const;
    CheckShapes getOverlaySnapshot(const CheckRequest& request,
        const Rect& guardRegion, const FillerChanges& fillerChanges,
        bool useNewFillers, const std::set<InstanceId>& excludedNodes) const;
    CheckShapes getNodeShape(InstanceId instanceId, MasterId masterId,
        RowId rowId, ColId colId, PhysOrientation orientation,
        bool isCandidate) const;
    CheckShapes mergeShapes(const CheckShapes& rects) const;
    CheckShapes findNeighbors(const CheckShape& target, const Rule& rule,
        Relationship relationship, const CheckShapes& shapes) const;
    std::vector<CheckOutcome> evalRule(const Rule& rule,
        const CheckShapes& shapes, const std::vector<XInterval>& tgtItvs) const;
    std::vector<Violation> makeViolations(const std::vector<CheckOutcome>& outcomes,
        const CheckShapes& shapes) const;

    CheckResult checkPlaceWithOverlay(const CheckRequest& request,
        const Rect& guardRegion, const FillerChanges& fillerChanges,
        const std::vector<Violation>& oldViolations) const;
    CheckResult checkOverlayRegion(const CheckRequest& request,
        const Rect& guardRegion, const FillerChanges& fillerChanges,
        bool useNewFillers) const;

    CheckShapes mergeGroupShapes(const CheckShapes& rawShapes,
        bool isCandidate) const;

    OverlapInfo checkOverlap(const CheckRequest& request) const;
    DiagVec validateOverlayRequest(const CheckRequest& request,
        const FillerChanges& fillerChanges) const;
    bool touchesInstance(const Violation& violation, InstanceId instanceId) const;
    bool containsViolation(const Violation& oldViolation,
        const Violation& newViolation) const;
    bool isInGuard(const XInterval& xWindow, const RowIdVec& rowIds,
        const Rect& guard) const;
    void finishViolation(Violation& violation) const;
    bool groupFails(const Rule& rule, const CheckShape& target,
        const CheckShape& neighbor, Relationship relationship,
        const XInterval& xWindow, const CheckShapes& shapes) const;
    Dbu queryRadius(const Rule& rule) const;

    Network* network_ = nullptr;
    std::vector<Layer> layers_;
    std::unordered_map<std::string, std::vector<LayerId>> layerGroups_;
    std::vector<Rule> rules_;
    std::vector<Rule*> sortedRules_;
    std::map<MasterId, MasterItem> masterItems_;

    std::vector<Diagnostic> diagnostics_; // Initialization diagnostics.
    Layer::Polar basePolar_ = Layer::Polar::P; // polarity at bottom band of row 0;
    Dbu rowHeight_ = 0;
    Dbu siteWidth_ = 1;
    int maxRuleValue_ = 1; // the maxValue for all rules' minValue
    std::atomic<bool> enableFillerRepair_{true};
    mutable std::once_flag fillerRepairInit_;
    mutable std::unique_ptr<fillerRepair::FillerRepairEngine>
        fillerRepairEngine_;

    std::map<eLIB::TechLayerRelativeID, LayerId> techLayerToIdx_;
    std::map<std::string, LayerId> layerNameToIdx_;
    const std::array<Relationship, 2> relations_ = {Relationship::IntraRow,
      Relationship::InterRow};
};

} // namespace ipl
} // namespace dpl2
