#include "drc/ImplantLayerChecker.h"
#include <dpl2/network.h>
// [FRPORT] Required only for the checker-to-engine dispatch below.
#include <fillerRepair/FillerRepairEngine.h>
#include <tbb/enumerable_thread_specific.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <physlib/techRuleCheck.hh>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>

#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"
#include "util.h"
#include "util/performance.hh"

namespace dpl2 {
namespace ipl {

constexpr Dbu ADJACENT_ROW_VERTICAL_SPACING = 1;

const LeafCellID* cellChangeLeafCellId(const CellChangeRecord& change)
{
    return std::get_if<LeafCellID>(&change.cell_data_);
}

// [fillerRepair-layout] Add records have no database instance yet. Their
// request-local name is the stable identity used while evaluating one batch.
const std::string* cellChangeName(const CellChangeRecord& change)
{
    return std::get_if<std::string>(&change.cell_data_);
}

inline Layer::Polar opposite(Layer::Polar p)
{
    return (p == Layer::Polar::N) ? Layer::Polar::P : Layer::Polar::N;
}

bool Rule::isWidth() const
{
    return source_ == RuleSource::Width || source_ == RuleSource::Lef58Width;
}

bool Rule::isSpacing() const
{
    return source_ == RuleSource::Spacing
           || source_ == RuleSource::Lef58Spacing;
}

bool Rule::contains(Relationship rlt) const
{
    // LEF58 spacing directions describe the spacing direction: horizontal is
    // same-row x spacing, and vertical is adjacent-row spacing gated by x PRL.
    if (source_ == RuleSource::Lef58Spacing) {
        if (direction_ == RuleDirection::Vertical) {
            return rlt == Relationship::InterRow;
        }
        return rlt == Relationship::IntraRow;
    } else {
        if (source_ == RuleSource::Lef58Width && zeroPrl_) {
            return rlt == Relationship::InterRow;
        } else {
            return true;
        }
    }
}

bool lessXInterval(const XInterval& left, const XInterval& right)
{
    if (left.xl != right.xl) {
        return left.xl < right.xl;
    }
    return left.xh < right.xh;
}

bool touchesOrOverlaps(const XInterval& left, const XInterval& right)
{
    return std::max(left.xl, right.xl) <= std::min(left.xh, right.xh);
}

bool touchesOrOverlaps(const XInterval& left,
                       const std::vector<XInterval>& rights)
{
    for (const XInterval& right : rights) {
        if (touchesOrOverlaps(left, right)) {
            return true;
        }
    }
    return false;
}

bool overlaps(const XInterval& left, const XInterval& right)
{
    return std::max(left.xl, right.xl) < std::min(left.xh, right.xh);
}

bool contains(const XInterval& outer, const XInterval& inner)
{
    return outer.xl <= inner.xl && outer.xh >= inner.xh;
}

template <typename Value>
void sortUnique(std::vector<Value>& values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void hashAppend(uint64_t& hash, uint64_t value)
{
    constexpr uint64_t PRIME = 1099511628211ull;
    hash ^= value;
    hash *= PRIME;
}

uint64_t signedHashValue(int64_t value)
{
    return static_cast<uint64_t>(value) ^ (static_cast<uint64_t>(value) >> 32);
}

Dbu spacing(const XInterval& left, const XInterval& right)
{
    return std::max<Dbu>(
        (Dbu) 0, std::max(left.xl, right.xl) - std::min(left.xh, right.xh));
}

Dbu prl(const XInterval& left, const XInterval& right)
{
    return std::min(left.xh, right.xh) - std::max(left.xl, right.xl);
}

XInterval unite(const XInterval& left, const XInterval& right)
{
    return {std::min(left.xl, right.xl), std::max(left.xh, right.xh)};
}

XInterval intersect(const XInterval& left, const XInterval& right)
{
    return {std::max(left.xl, right.xl), std::min(left.xh, right.xh)};
}

XInterval gap(const XInterval& left, const XInterval& right)
{
    return {std::min(left.xh, right.xh), std::max(left.xl, right.xl)};
}

template <typename Value>
void appendUnique(std::vector<Value>& values, Value value)
{
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

std::string makeMessage(const std::string& prefix, int id)
{
    std::ostringstream stream;
    stream << prefix << id;
    return stream.str();
}

template <typename Enum>
int enumInt(Enum value)
{
    return static_cast<int>(value);
}

// --------------------------------------------------------------------------------
// Polarity alternates per row: if row 0 bottom = basePolar,
//    - Even rows: Bottom=basePolar, Top=!basePolar
//    - Odd rows:  Bottom=!basePolar, Top=basePolar
Layer::Polar ImplantLayerChecker::slotPolar(RowId rowId,
                                            BandSlot bandSlot) const
{
    const bool isEven = ((rowId % 2) == 0);
    const Layer::Polar bottomPolar = isEven ? basePolar_ : opposite(basePolar_);
    return (bandSlot == BandSlot::Bottom) ? bottomPolar : opposite(bottomPolar);
}

SlotRef ImplantLayerChecker::getAdjSlot(RowId rowId, BandSlot bandSlot) const
{
    SlotRef ret{-1, BandSlot::Bottom};
    if (bandSlot == BandSlot::Top) {
        if (rowId < grid_->getRowCount().v - 1) {
            ret = {rowId + 1, BandSlot::Bottom};
        }
    } else {
        if (rowId > 0) {
            ret = {rowId - 1, BandSlot::Top};
        }
    }
    return ret;
}

Layer::Polar ImplantLayerChecker::getPolar(RowId rowA, RowId rowB) const
{
    const RowId low = std::min(rowA, rowB);
    return (low % 2 == 0) ? opposite(basePolar_) : basePolar_;
}

ImplantLayerChecker::ImplantLayerChecker(Grid* grid,
                                         eUNL::Design* design,
                                         Network* network)
    : DRCChecker(grid), network_(network), design_(design)
{
    if (grid_ == nullptr) {
        diagnostics_.push_back(
            {"missing_grid",
             "fatal: ImplantLayerChecker requires an initialized Grid"});
        return;
    }
    if (network_ == nullptr) {
        diagnostics_.push_back(
            {"missing_network",
             "fatal: ImplantLayerChecker requires an initialized Network"});
        return;
    }
    if (design_ == nullptr) {
        diagnostics_.push_back(
            {"missing_design",
             "fatal: ImplantLayerChecker requires an explicit Design"});
        return;
    }
    PhysDesMgr* const desMgr = design_->getPhysDesMgr();
    if (desMgr == nullptr) {
        diagnostics_.push_back(
            {"missing_design_phys_des_mgr",
             "fatal: ImplantLayerChecker requires the explicit Design to own"
             " a PhysDesMgr"});
        return;
    }
    if (grid_->getDesMgr() == nullptr) {
        diagnostics_.push_back(
            {"missing_grid_phys_des_mgr",
             "fatal: ImplantLayerChecker requires Grid to retain its"
             " initialization PhysDesMgr"});
        return;
    }
    init(desMgr);
}

ImplantLayerChecker::~ImplantLayerChecker() = default;

bool ImplantLayerChecker::hasUsableInfrastructure() const
{
    if (grid_ == nullptr || network_ == nullptr) {
        return false;
    }
    return std::none_of(
        diagnostics_.begin(),
        diagnostics_.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.status == "missing_grid"
                   || diagnostic.status == "missing_network"
                   || diagnostic.status == "missing_design"
                   || diagnostic.status == "missing_design_phys_des_mgr"
                   || diagnostic.status == "missing_grid_phys_des_mgr";
        });
}

// --------------------------------------------------------------------------------
// initialize
// --------------------------------------------------------------------------------
bool ImplantLayerChecker::init(PhysDesMgr* desMgr)
{
    eUTL::PerfLogger perfLogger("dpl2.init");
    bool isFullUtil = grid_->isFullUtil();
    std::cout << "Fully utilized grid: " << isFullUtil << std::endl;

    bool ok = true;

    // Extract Width and Height (todo: refine)
    siteWidth_ = grid_->getSiteWidth().v;
    for (const PhysRow& row : desMgr->getPhysRowIter()) {
        if (!row.getSite().getIsPad()) {
            Dbu h = row.getSite().getHeight().getStorage();
            if (rowHeight_ == 0 || h < rowHeight_) {
                rowHeight_ = h;
            }
        }
    }
    buildLayers(desMgr);
    buildMasters();
    ok = buildRules() && ok;
    ok = buildMstIntervals() && ok;

    if (!ok) {
        for (Diagnostic& diag : diagnostics_) {
            std::cout << diag.status << " " << diag.message << std::endl;
        }
    }
    printStats(std::cout, true /*isShort*/);
    return ok;
}

// Extract implant layers from the tech library and
// create width/spacing rules for each.
void ImplantLayerChecker::buildLayers(PhysDesMgr* desMgr)
{
    const eLIB::TechLib& tech = desMgr->getTopTech();
    for (const eLIB::TechLayer& layer : tech.getLayerIter()) {
        if (layer.isImplant()) {
            LayerId id = layers_.size();
            Layer il;
            il.setId(id);
            il.setName(layer.getName());
            il.setTechLayerId(layer.getId().getLocalId());
            Layer::Vt fam = Layer::Vt::Unknown;
            Layer::Polar pol = Layer::Polar::N;
            parseLayerName(il.getName(), fam, pol);
            il.setVt(fam);
            il.setPolar(pol);
            techLayerToIdx_[layer.getId().getLocalId()] = id;
            layerNameToIdx_[layer.getName()] = id;
            layers_.push_back(il);
        }
    }

    // todo: check the LEF58_IMPLANTGROUP from the library

    for (const Layer& il : layers_) {
        eLIB::TechLayerRelativeID relId = il.getTechLayerId();
        const eLIB::TechLayer& techLayer = tech.getTechLayer(relId);
        // Width
        rules_.emplace_back(Rule(rules_.size(),
                                 RuleSource::Width,
                                 il.getId(),
                                 techLayer.getWidth().getStorage()));
        // Spacing
        rules_.emplace_back(Rule(rules_.size(),
                                 RuleSource::Spacing,
                                 il.getId(),
                                 techLayer.getMinSpacing().getStorage()));

        for (const eLIB::TechRule& techRule : techLayer.getRuleIter()) {
            if (techRule.getCheck().getType()
                == eLIB::RuleCheckType::WIDTH_RULE) {
                // "WIDTH minWidth [LAYER layerName2] [ZEROPRL]
                // [EXCEPTCORNERTOUCH]
                //      [LENGTH length] [CHECKIMPLANTGROUP groupName]
                //      ; "
                const eLIB::WidthRule* widthRule
                    = dynamic_cast<const eLIB::WidthRule*>(
                        &techRule.getCheck());
                if (widthRule) {
                    rules_.push_back(Rule(rules_.size(),
                                          RuleSource::Lef58Width,
                                          il.getId(),
                                          widthRule->getWidth().getStorage()));
                    Rule& rule = rules_.back();

                    // Set optional clauses
                    const std::string& layerName
                        = widthRule->getOtherImplLayerName();
                    if (!layerName.empty()) {
                        LayerId id = getLayerId(layerName);
                        uvAssert(id != -1);
                        rule.setSecondaryLayer(id);
                    }
                    if (widthRule->getZeroPRL()) {
                        rule.setZeroPrl(true);
                    }
                    if (widthRule->getExceptCornerTouch()) {
                        rule.setExceptCornerTouch(true);
                    }
                    if (widthRule->getLength() > eUTL::UvDist(0)) {
                        rule.setLength(widthRule->getLength().getStorage());
                    }

                    // CHECKIMPLANTGROUP (todo: test the layer groups)
                    const std::string& checkGroup
                        = widthRule->getCheckImplantGroup();
                    if (!checkGroup.empty()) {
                        rule.setCheckGroup(checkGroup);
                        uvAssert(layerGroups_.contains(checkGroup));
                    }
                }
            } else if (techRule.getCheck().getType()
                       == eLIB::RuleCheckType::SPACING_IMPLANT_RULE) {
                // "SPACING minSpacing [LAYER layerName2]
                //      [HORIZONTAL|VERTICAL PRL prl ] [EXCEPTABUTTED]
                //      [EXCEPTCORNERTOUCH] [LENGTH length]
                //      [INTERSECTLAYERS layerNameList...]
                //      ; "
                const eLIB::SpacingImplantRule* spacingRule
                    = dynamic_cast<const eLIB::SpacingImplantRule*>(
                        &techRule.getCheck());
                if (spacingRule) {
                    const std::vector<eLIB::SpacingImplantRule::SIItem>&
                        spacingTable
                        = spacingRule->getSpacingImplantTable();
                    for (const eLIB::SpacingImplantRule::SIItem& item :
                         spacingTable) {
                        rules_.push_back(Rule(rules_.size(),
                                              RuleSource::Lef58Spacing,
                                              il.getId(),
                                              item.minSpacing.getStorage()));
                        Rule& rule = rules_.back();

                        if (!item.layerName2.empty()) {
                            LayerId id = getLayerId(item.layerName2);
                            uvAssert(id != -1);
                            rule.setSecondaryLayer(id);
                        }

                        if (item.prlOrient
                            == eLIB::RuleCheckOrthoTypeE::HORIZONTAL) {
                            rule.setDirection(RuleDirection::Horizontal);
                        } else if (item.prlOrient
                                   == eLIB::RuleCheckOrthoTypeE::VERTICAL) {
                            rule.setDirection(RuleDirection::Vertical);
                        }
                        if (item.prl != eUTL::UvDist(0)) {
                            rule.setPrl(item.prl.getStorage());
                        }

                        if (item.exceptAbutted) {
                            rule.setExceptAbutted(true);
                        }
                        if (item.exceptCornerTouch) {
                            rule.setExceptCornerTouch(true);
                        }
                        if (item.length > eUTL::UvDist(0)) {
                            rule.setLength(item.length.getStorage());
                        }

                        // INTERSECTLAYERS
                        std::vector<LayerId> intersectLayers;
                        for (const std::string& layerName :
                             item.layerNameList) {
                            LayerId id = getLayerId(layerName);
                            uvAssert(id != -1);
                            intersectLayers.push_back(id);
                        }
                        if (!intersectLayers.empty()) {
                            rule.setIntersectLayers(intersectLayers);
                        }
                    }
                }
            }
        }
    }
}

// Validate and index implant layers, groups, and normalized rules;
// sort by specificity.
bool ImplantLayerChecker::buildRules()
{
    bool ok = true;
    for (Rule rule : rules_) {
        // Keep unsupported rules visible to callers, but do not let them
        // participate in violation generation.
        if (!rule.getUnsupportedClauses().empty()) {
            diagnostics_.push_back(
                {"skipped_unsupported_rule_clause",
                 makeMessage("unsupported LEF58 clause in rule ",
                             rule.getRuleId())});
        }
        if (rule.getCheckGroup()) {
            if (layerGroups_.find(*rule.getCheckGroup())
                == layerGroups_.end()) {
                diagnostics_.push_back(
                    {"skipped_missing_rule_parameter",
                     "unknown implant group " + *rule.getCheckGroup()});
            }
        }
    }
    for (auto& rule : rules_) {
        sortedRules_.push_back(&rule);
    }
    // Specific rules are evaluated first so containment suppression can later
    // recognize "specific satisfied, broad violated" as legal.
    std::sort(
        sortedRules_.begin(),
        sortedRules_.end(),
        [](const Rule* left, const Rule* right) {
            if (left->getSpecificityRank() != right->getSpecificityRank()) {
                return left->getSpecificityRank() < right->getSpecificityRank();
            }
            return left->getRuleId() < right->getRuleId();
        });
    setMaxRuleValue();
    return ok;
}

// Set the max search radius (in sites) from the largest rule value.
void ImplantLayerChecker::setMaxRuleValue()
{
    int maxValue = 0;
    for (const Rule& rule : rules_) {
        if (rule.getMinValue() > maxValue) {
            maxValue = rule.getMinValue();
        }
    }
    if (siteWidth_ > 0) {
        maxRuleValue_ = (maxValue + siteWidth_ - 1) / siteWidth_;
    }
}

// Convert master shapes into half-row-band x-intervals for the checker.
bool ImplantLayerChecker::buildMstIntervals()
{
    bool ok = true;
    const Dbu halfRow = rowHeight_ / 2;
    // Macro-internal pieces can be skipped for plain width/spacing after
    // row-band restructuring, but LEF58 group/intersect clauses may still need
    // them as context.
    const bool keepInternal = std::any_of(
        sortedRules_.begin(), sortedRules_.end(), [](const Rule* rule) {
            return rule->getCheckGroup().has_value()
                   || !rule->getIntersectLayers().empty();
        });

    for (MasterItem& item : masterItems_) {
        const Dbu width = item.width;
        // Build intervals for each shape
        std::vector<MasterInterval> intervals;
        for (size_t i = 0; i < item.shapes.size(); ++i) {
            const MasterShape& shape = item.shapes[i];
            // Extract rect coordinates as Dbu upfront to avoid UvDist
            // incompatibilities with Dbu arithmetic and comparisons.
            const Dbu shapeXl = shape.rect._xl.getStorage();
            const Dbu shapeXh = shape.rect._xh.getStorage();
            const Dbu shapeYl = shape.rect._yl.getStorage();
            const Dbu shapeYh = shape.rect._yh.getStorage();

            if (shapeXl >= shapeXh || shapeYl >= shapeYh) {
                diagnostics_.push_back(
                    {"skipped_unsupported_geometry",
                     makeMessage("invalid rectangle shape ", shape.shapeId)});
                ok = false;
                continue;
            }
            if (shapeXl != 0 || shapeXh != width) {
                diagnostics_.push_back(
                    {"implant_shape_width_mismatch",
                     makeMessage("implant shape does not span master width ",
                                 shape.shapeId)});
                ok = false;
            }
            if (shapeXl < 0 || shapeXh > width || shapeYl < 0
                || shapeYh > item.height) {
                diagnostics_.push_back(
                    {"shape_outside_macro_boundary",
                     makeMessage("shape outside macro ", shape.shapeId)});
                ok = false;
                continue;
            }
            for (size_t j = i + 1; j < item.shapes.size(); ++j) {
                const MasterShape& other = item.shapes[j];
                if (shape.layer != other.layer) {
                    continue;
                }
                const bool xOverlap
                    = std::max(shape.rect._xl, other.rect._xl)
                      < std::min(shape.rect._xh, other.rect._xh);
                const bool yOverlap
                    = std::max(shape.rect._yl, other.rect._yl)
                      < std::min(shape.rect._yh, other.rect._yh);
                if (xOverlap && yOverlap) {
                    diagnostics_.push_back(
                        {"shape_overlap_in_input",
                         makeMessage("overlap at shape ", shape.shapeId)});
                    ok = false;
                }
            }

            const bool macroInternal = shapeXl > 0 && shapeXh < width
                                       && shapeYl > 0 && shapeYh < item.height;

            // Restructure rectangles along the orthogonal axis into canonical
            // half-row bands. After this step the checker only stores x
            // intervals; y/height are represented by (rowOffset, bandSlot).
            Dbu y = shapeYl;
            while (y < shapeYh) {
                const int rowOffset = static_cast<int>(y / rowHeight_);
                const Dbu rowBase = static_cast<Dbu>(rowOffset) * rowHeight_;
                const Dbu bottomEnd = rowBase + halfRow;
                const BandSlot slot
                    = y < bottomEnd ? BandSlot::Bottom : BandSlot::Top;
                const Dbu slotEnd = slot == BandSlot::Bottom
                                        ? bottomEnd
                                        : rowBase + rowHeight_;
                const Dbu pieceEnd = std::min(shapeYh, slotEnd);
                if (pieceEnd <= y) {
                    break;
                }
                const bool fullSlot
                    = y == (slot == BandSlot::Bottom ? rowBase : bottomEnd)
                      && pieceEnd == slotEnd;
                if (!fullSlot) {
                    // The current stage assumes regular implant tracks. Partial
                    // bands are still normalized, but are reported because they
                    // violate that modeling assumption.
                    diagnostics_.push_back(
                        {"unsupported_row_band_slot",
                         makeMessage("partial band slot shape ",
                                     shape.shapeId)});
                }
                MasterInterval interval;
                interval.masterId = item.masterId;
                interval.shapeId = shape.shapeId;
                interval.layer = shape.layer;
                interval.rowOffset = rowOffset;
                interval.bandSlot = slot;
                interval.x = {shapeXl, shapeXh};
                interval.runtimeCheckable = !macroInternal || keepInternal;
                interval.skipReason = interval.runtimeCheckable
                                          ? ""
                                          : "macro_internal_after_restructure";
                if (interval.runtimeCheckable) {
                    intervals.push_back(interval);
                }
                y = pieceEnd;
            }
        }
        item.intervals = std::move(intervals);
    }
    return ok;
}

// Build masterItems_ (implant shapes) from the Network masters and tech
// library.
void ImplantLayerChecker::buildMasters()
{
    int masterCount = network_->getMasters().size();
    // [fillerRepair-fix] A late Network registration rebuilds this table.
    // Replace old entries instead of appending every raw shape a second time.
    masterItems_.assign(masterCount, MasterItem{});

    for (const std::unique_ptr<Master>& masterPtr : network_->getMasters()) {
        const Master* nm = masterPtr.get();
        if (!nm) {
            diagnostics_.push_back(
                {"null_network_master", "Network contains a null master slot"});
            continue;
        }
        MasterId mid = nm->getId();
        if (mid < 0 || mid >= masterCount) {
            diagnostics_.push_back(
                {"invalid_network_master_id", makeMessage("master ", mid)});
            continue;
        }
        const eLIB::PhysLibCell* physCell = nm->getPhysLibCell();
        if (physCell == nullptr) {
            diagnostics_.push_back(
                {"missing_physical_master", makeMessage("master ", mid)});
            continue;
        }
        const eLIB::TechSite* techSite = physCell->getTechSite();
        if (techSite == nullptr) {
            diagnostics_.push_back(
                {"missing_master_site", makeMessage("master ", mid)});
            continue;
        }

        MasterItem& item = masterItems_[mid];
        item.width = physCell->getWidth().getStorage();
        item.height = physCell->getHeight().getStorage();
        item.siteHeight = techSite->getHeight().getStorage();
        // [fillerRepair-layout] Overlay Add validation asks Grid for the
        // orientation of this exact site family at the proposed position.
        item.siteName = techSite->getName();
        item.masterId = mid;
        item.isFiller = nm->isFiller();

        // Check if this master has implant shapes
        bool hasImplant = false;
        const std::vector<eLIB::PhysLibObs>& obsVec
            = physCell->getObstruction();
        for (const eLIB::PhysLibObs& obs : obsVec) {
            const eLIB::LayerShapeMapT& shapes
                = obs.getShapes(eUTL::PhysOrientationE::R0);
            for (const auto& [layerRelId, shapeVec] : shapes) {
                if (getLayerId(layerRelId) >= 0) {
                    hasImplant = true;
                    break;
                }
            }
            if (hasImplant)
                break;
        }

        if (!hasImplant) {
            continue;
        }

        ShapeId shapeId = 0;
        for (const eLIB::PhysLibObs& obs : obsVec) {
            const eLIB::LayerShapeMapT& shapes
                = obs.getShapes(eUTL::PhysOrientationE::R0);
            for (const auto& [layerRelId, shapeVec] : shapes) {
                LayerId lid = getLayerId(layerRelId);
                if (lid < 0) {
                    continue;  // non-implant
                }
                for (const eLIB::TechShape& techShape : shapeVec) {
                    if (techShape.getType() != eLIB::TechShape::RECT) {
                        diagnostics_.push_back({"skipped_unsupported_geometry",
                                                "master non-RECT shape"});
                        continue;
                    }
                    const eUTL::Rect& mRect = techShape.getRect();
                    MasterShape mis;
                    mis.masterId = mid;
                    mis.shapeId = shapeId++;
                    mis.layer = lid;
                    mis.rect = mRect;
                    item.shapes.push_back(mis);
                }
            }
        }
        item.rawShapes = item.shapes;
    }

    rebuildMasterShapes();
}

// Rebuild master shapes into canonical bottom/top half-row bands
// spanning full width.
void ImplantLayerChecker::rebuildMasterShapes()
{
    for (MasterItem& item : masterItems_) {
        if (item.rawShapes.empty()) {
            item.shapes.clear();
            continue;
        }

        if (item.siteHeight <= 0) {
            diagnostics_.push_back({"skipped_rebuild_no_site_height",
                                    "skipped_rebuild_no_site_height: master "
                                        + std::to_string(item.masterId)});
            item.shapes = item.rawShapes;
            continue;
        }

        const Dbu fullRow = rowHeight_;
        const Dbu halfRow = fullRow / 2;

        // Determine the number of rows this master spans
        int numRows = static_cast<int>((item.height + fullRow - 1) / fullRow);
        if (numRows < 1)
            numRows = 1;

        // Determine the family from raw shapes (all should be the same family)
        Layer::Vt family = Layer::Vt::Unknown;
        for (const MasterShape& rs : item.rawShapes) {
            auto layerIt = std::find_if(
                layers_.begin(), layers_.end(), [&](const Layer& l) {
                    return l.getId() == rs.layer;
                });
            if (layerIt != layers_.end()) {
                family = layerIt->getVt();
                break;
            }
        }
        if (family == Layer::Vt::Unknown) {
            diagnostics_.push_back({"skipped_rebuild_unknown_family",
                                    "skipped_rebuild_unknown_family: master "
                                        + std::to_string(item.masterId)});
            item.shapes = item.rawShapes;
            continue;
        }

        // Determine the base band polarity from the bottommost raw shape
        Dbu minY = std::numeric_limits<Dbu>::max();
        Layer::Polar bottomPolarity = Layer::Polar::N;
        for (const MasterShape& rs : item.rawShapes) {
            Dbu yl = rs.rect._yl.getStorage();
            if (yl < minY) {
                minY = yl;
                auto layerIt = std::find_if(
                    layers_.begin(), layers_.end(), [&](const Layer& l) {
                        return l.getId() == rs.layer;
                    });
                if (layerIt != layers_.end()) {
                    bottomPolarity = layerIt->getPolar();
                }
            }
        }

        // Find N and P layer IDs for this family
        LayerId familyNLayer = -1;
        LayerId familyPLayer = -1;
        for (const Layer& l : layers_) {
            if (l.getVt() == family) {
                if (l.getPolar() == Layer::Polar::N)
                    familyNLayer = l.getId();
                else
                    familyPLayer = l.getId();
            }
        }
        if (familyNLayer < 0 || familyPLayer < 0) {
            diagnostics_.push_back({"skipped_rebuild_missing_layer",
                                    "skipped_rebuild_missing_layer: master "
                                        + std::to_string(item.masterId)
                                        + " family missing N or P layer"});
            item.shapes = item.rawShapes;
            continue;
        }

        // Build 2*numRows shapes
        item.shapes.clear();
        ShapeId shapeId = 0;
        for (int row = 0; row < numRows; ++row) {
            Layer::Polar bottomBandPol, topBandPol;
            if (row % 2 == 0) {
                bottomBandPol = bottomPolarity;
                topBandPol = (bottomPolarity == Layer::Polar::N)
                                 ? Layer::Polar::P
                                 : Layer::Polar::N;
            } else {
                bottomBandPol = (bottomPolarity == Layer::Polar::N)
                                    ? Layer::Polar::P
                                    : Layer::Polar::N;
                topBandPol = bottomPolarity;
            }

            // Bottom band shape
            {
                MasterShape ms;
                ms.shapeId = shapeId++;
                ms.layer = (bottomBandPol == Layer::Polar::N) ? familyNLayer
                                                              : familyPLayer;
                Dbu yBase = static_cast<Dbu>(row) * fullRow;
                ms.rect = eUTL::Rect(eUTL::UvDist(static_cast<int64_t>(0)),
                                     eUTL::UvDist(yBase),
                                     eUTL::UvDist(item.width),
                                     eUTL::UvDist(yBase + halfRow));
                item.shapes.push_back(ms);
            }

            // Top band shape
            {
                MasterShape ms;
                ms.shapeId = shapeId++;
                ms.layer = (topBandPol == Layer::Polar::N) ? familyNLayer
                                                           : familyPLayer;
                Dbu yBase = static_cast<Dbu>(row) * fullRow + halfRow;
                ms.rect = eUTL::Rect(eUTL::UvDist(static_cast<int64_t>(0)),
                                     eUTL::UvDist(yBase),
                                     eUTL::UvDist(item.width),
                                     eUTL::UvDist(yBase + halfRow));
                item.shapes.push_back(ms);
            }
        }
    }
}

// --------------------------------------------------------------------------------
// Checker Entry Interface
// --------------------------------------------------------------------------------
// [FRPORT] The base checker entry delegates to the repair-aware overload.
bool ImplantLayerChecker::check(const Node* node,
                                GridX x,
                                GridY y,
                                const eUTL::PhysOrientation& orient) const
{
    std::vector<CellChangeRecord> fcRecord;
    return check(node, x, y, orient, fcRecord);
}

bool ImplantLayerChecker::check(const Node* node,
                                GridX x,
                                GridY y,
                                const eUTL::PhysOrientation& orient,
                                std::vector<CellChangeRecord>& fcRecord) const
{
    if (!node || node->getMaster() == nullptr || !hasUsableInfrastructure()) {
        return false;
    }
    CheckRequest request;
    request.instanceId = node->getId();
    request.masterId = node->getMaster()->getId();
    // Preserve the caller's proposed row so unsupported movement is rejected
    // against the engine snapshot instead of being silently normalized.
    request.rowId = y.v;
    request.colId = x.v;
    request.orientation = orient;
    request.targetOp = OpType::Replace;

    // Node-facing checks are same-footprint Replace/rotation only. Std-cell
    // Add/Delete transactions use repair(CellChangeRecord) so the operation
    // is explicit and the caller retains the target record.
    if (enableFillerRepair_ && targetFootprintChanged(request)) {
        return false;
    }
    bool isLegal = checkDirect(request).isLegal;
    // [fillerRepair-fix] Helper-built checkers disable repair so their checks
    // retain DRC-only semantics; ordinary checker instances default to enabled.
    // [FRPORT] Dispatch failed checks to the initialized caller-owned engine.
    if (!isLegal && enableFillerRepair_ && fillerRepairEngine_ != nullptr) {
        isLegal = repairFillers(request, fcRecord);
    }
    return isLegal;
}

bool ImplantLayerChecker::repair(const CellChangeRecord& targetChange,
                                 std::vector<CellChangeRecord>& fcRecord) const
{
    if (!enableFillerRepair_ || fillerRepairEngine_ == nullptr) {
        return false;
    }
    fillerRepair::RepairOutcome outcome
        = fillerRepairEngine_->repair(targetChange);
    if (!outcome.hasSolution) {
        return false;
    }
    fcRecord.insert(fcRecord.end(),
                    std::make_move_iterator(outcome.changes.begin()),
                    std::make_move_iterator(outcome.changes.end()));
    return true;
}

// [FRPORT] Translate the checker request into engine output appended for opto.
bool ImplantLayerChecker::repairFillers(
    const CheckRequest& request,
    std::vector<CellChangeRecord>& fcRecord) const
{
    if (fillerRepairEngine_ == nullptr) {
        return false;
    }
    fillerRepair::RepairOutcome outcome = fillerRepairEngine_->repair(request);
    if (!outcome.hasSolution) {
        return false;
    }
    // Append-only into the caller's record; the checker stores nothing.
    fcRecord.insert(fcRecord.end(),
                    std::make_move_iterator(outcome.changes.begin()),
                    std::make_move_iterator(outcome.changes.end()));
    return true;
}

void ImplantLayerChecker::ensureMasterData(MasterId masterId) const
{
    if (masterId < 0) {
        return;
    }
    {
        std::shared_lock<std::shared_mutex> lock(masterItemsMutex_);
        if (static_cast<size_t>(masterId) < masterItems_.size()) {
            return;
        }
    }
    std::unique_lock<std::shared_mutex> lock(masterItemsMutex_);
    if (static_cast<size_t>(masterId) >= masterItems_.size()) {
        ImplantLayerChecker* self = const_cast<ImplantLayerChecker*>(this);
        self->buildMasters();
        self->buildMstIntervals();
    }
}

// Run all implant rules against the snapshot around a single placement request.
CheckResult ImplantLayerChecker::checkDirect(const CheckRequest& request) const
{
    CheckResult result;
    if (!hasUsableInfrastructure()) {
        result.isLegal = false;
        result.diagnostics = diagnostics_;
        return result;
    }
    // A candidate master may be registered after checker construction. Extend
    // the table once under an exclusive lock, then keep this check on a shared
    // lock so concurrent checker calls cannot observe a partial rebuild.
    ensureMasterData(request.masterId);
    std::shared_lock<std::shared_mutex> masterLock(masterItemsMutex_);
    if (siteWidth_ <= 0 || request.colId < 0) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(
            {"placement_not_site_aligned",
             makeMessage("instance ", request.instanceId)});
        return result;
    }
    Node* node = network_->getNode(request.instanceId);
    if (node == nullptr) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(
            {"unknown_target_instance",
             makeMessage("instance ", request.instanceId)});
        result.isLegal = false;
        return result;
    }
    if (node->getMaster() == nullptr) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(
            {"target_instance_missing_master",
             makeMessage("instance ", request.instanceId)});
        result.isLegal = false;
        return result;
    }
    const bool targetHasData
        = request.masterId >= 0
          && request.masterId < static_cast<MasterId>(masterItems_.size())
          && masterItems_[request.masterId].width > 0;
    if (!targetHasData) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(
            {"unknown_target_master",
             makeMessage("master ", request.masterId)});
        result.isLegal = false;
        return result;
    }
    if (request.rowId < 0 || request.rowId >= grid_->getRowCount().v
        || request.colId >= grid_->getRowSiteCount().v) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(
            {"placement_out_of_grid",
             makeMessage("instance ", request.instanceId)});
        result.isLegal = false;
        return result;
    }
    const OverlapInfo& overlap = checkOverlap(request);
    if (overlap.diags) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(*overlap.diags);
        result.isLegal = false;
        return result;
    }
    std::set<InstanceId> excludedNodes = overlap.fillers;
    excludedNodes.insert(request.instanceId);

    const CheckShapes& snapshot = getSnapshot(request, excludedNodes);
    for (const CheckShape& shape : snapshot) {
        if (shape.isCandidate && !slotPolarityOk(shape)) {
            result.diagnostics = diagnostics_;
            result.diagnostics.push_back(
                {"row_slot_polarity_mismatch",
                 makeMessage("instance ", shape.ownerInstanceIds[0])});
            result.isLegal = false;
            return result;
        }
    }

    // prepare the target intervals
    XInterval tgtItv;
    tgtItv.xl = request.colId * siteWidth_;
    tgtItv.xh = tgtItv.xl + masterItems_[request.masterId].width;
    std::vector<XInterval> itvs{tgtItv};

    const std::vector<CheckShape> shapes = mergeShapes(snapshot);
    auto run_job = [&](size_t i, std::vector<CheckOutcome>& local) {
        const Rule& rule = *sortedRules_[i];
        std::vector<CheckOutcome> partial = evalRule(rule, shapes, itvs);
        local.insert(local.end(), partial.begin(), partial.end());
    };
    std::vector<CheckOutcome> outcomes;
    if (DrcUtil::getArena()) {
        tbb::enumerable_thread_specific<std::vector<CheckOutcome>> tlsOutcomes;
        DrcUtil::parallelFor(sortedRules_.size(), run_job, tlsOutcomes);
        for (std::vector<CheckOutcome>& v : tlsOutcomes) {
            outcomes.insert(outcomes.end(), v.begin(), v.end());
        }
    } else {
        for (size_t i = 0; i < sortedRules_.size(); ++i) {
            run_job(i, outcomes);
        }
    }

    result.violations = makeViolations(outcomes, shapes);
    result.isLegal = result.violations.empty();
    result.diagnostics = diagnostics_;
    return result;
}

// Run checkDirect on every placed instance in parallel.
std::vector<CheckResult> ImplantLayerChecker::checkAllNodesDirect() const
{
    if (!hasUsableInfrastructure()) {
        return {};
    }
    const std::vector<std::unique_ptr<Node>>& nodes = getNodes();
    std::vector<CheckResult> results(nodes.size());
    auto run_job = [&](size_t i) {
        const Node* node = nodes[i].get();
        if (node == nullptr || node->getMaster() == nullptr) {
            results[i].isLegal = false;
            results[i].diagnostics = diagnostics_;
            results[i].diagnostics.push_back(
                {node == nullptr ? "null_network_node"
                                 : "target_instance_missing_master",
                 makeMessage("network node ", static_cast<int>(i))});
            return;
        }
        CheckRequest req;
        req.instanceId = node->getId();
        req.masterId = node->getMaster()->getId();
        req.rowId = grid_->gridSnapDownY(node).v;
        req.colId = grid_->gridX(node).v;
        req.orientation = node->getOrient();
        results[i] = checkDirect(req);
    };
    if (DrcUtil::getArena()) {
        DrcUtil::parallelFor(nodes.size(), run_job);
    } else {
        for (size_t i = 0; i < nodes.size(); ++i) {
            run_job(i);
        }
    }
    return results;
}

// Collect all shape intervals in the neighborhood of a placement request.
CheckShapes ImplantLayerChecker::getSnapshot(
    const CheckRequest& request,
    const std::set<InstanceId>& excludedNodes) const
{
    CheckShapes snapshot;

    const Node* tgtNode = network_->getNode(request.instanceId);
    if (tgtNode == nullptr || tgtNode->getMaster() == nullptr || siteWidth_ <= 0
        || rowHeight_ <= 0) {
        return snapshot;
    }
    // [fillerRepair-layout] The Network Node can still describe the old
    // footprint. Snapshot reach must follow the requested overlay master.
    const bool targetHasData
        = request.masterId >= 0
          && request.masterId < static_cast<MasterId>(masterItems_.size());
    const Dbu targetWidth
        = targetHasData ? masterItems_[request.masterId].width
                        : tgtNode->getWidth().v;
    const Dbu targetHeight
        = targetHasData ? masterItems_[request.masterId].height
                        : tgtNode->getHeight().v;
    int width = std::max(1, (targetWidth + siteWidth_ - 1) / siteWidth_);
    int height = std::max(1, (targetHeight + rowHeight_ - 1) / rowHeight_);
    RowId row0 = std::max(request.rowId - 1, 0);
    RowId row1 = std::min(request.rowId + height, grid_->getRowCount().v - 1);
    ColId col0 = std::max(request.colId - maxRuleValue_, 0);
    ColId col1 = std::min(request.colId + width + maxRuleValue_ - 1,
                          grid_->getRowSiteCount().v - 1);
    std::set<InstanceId> collectedNodes;
    auto shapesForNode = [&](const Node* node) {
        const std::pair<GridX, GridY> coord = grid_->gridXY(node);
        return getNodeShape(node->getId(),
                            node->getMaster()->getId(),
                            coord.second.v,
                            coord.first.v,
                            node->getOrient(),
                            false);
    };
    auto appendNode = [&](const Node* node) {
        if (node == nullptr || node->getMaster() == nullptr
            || excludedNodes.find(node->getId()) != excludedNodes.end()
            || !collectedNodes.insert(node->getId()).second) {
            return CheckShapes{};
        }
        CheckShapes shapes = shapesForNode(node);
        snapshot.insert(snapshot.end(), shapes.begin(), shapes.end());
        return shapes;
    };
    CheckShapes frontier;
    for (RowId r = row0; r <= row1; r++) {
        for (ColId c = col0; c <= col1; c++) {
            Pixel* p = grid_->gridPixel(GridX{c}, GridY{r});
            CheckShapes shapes = appendNode(p == nullptr ? nullptr : p->cell);
            frontier.insert(frontier.end(), shapes.begin(), shapes.end());
        }
    }

    // The rule-radius crop can cut through a longer abutting implant run.
    // Follow same-slot/layer contacts past that crop so the local merge keeps
    // the same owners as the committed full-row run.
    for (size_t i = 0; i < frontier.size(); ++i) {
        const CheckShape shape = frontier[i];
        const ColId left
            = shape.x.xl > 0 ? (shape.x.xl - 1) / siteWidth_ : -1;
        const ColId right = static_cast<ColId>(
            (static_cast<int64_t>(shape.x.xh) + siteWidth_ - 1)
            / siteWidth_);
        for (ColId col : {left, right}) {
            if (col < 0 || col >= grid_->getRowSiteCount().v) {
                continue;
            }
            Pixel* pixel = grid_->gridPixel(GridX{col}, GridY{shape.rowId});
            const Node* node = pixel == nullptr ? nullptr : pixel->cell;
            if (node == nullptr || node->getMaster() == nullptr
                || excludedNodes.find(node->getId()) != excludedNodes.end()
                || collectedNodes.find(node->getId()) != collectedNodes.end()) {
                continue;
            }
            const CheckShapes candidateShapes = shapesForNode(node);
            const bool connects = std::any_of(
                candidateShapes.begin(),
                candidateShapes.end(),
                [&](const CheckShape& candidate) {
                    return candidate.rowId == shape.rowId
                           && candidate.bandSlot == shape.bandSlot
                           && candidate.layer == shape.layer
                           && touchesOrOverlaps(candidate.x, shape.x);
                });
            if (!connects) {
                continue;
            }
            collectedNodes.insert(node->getId());
            snapshot.insert(snapshot.end(),
                            candidateShapes.begin(),
                            candidateShapes.end());
            frontier.insert(frontier.end(),
                            candidateShapes.begin(),
                            candidateShapes.end());
        }
    }

    const CheckShapes& shapes = getNodeShape(request.instanceId,
                                             request.masterId,
                                             request.rowId,
                                             request.colId,
                                             request.orientation,
                                             true);
    snapshot.insert(snapshot.end(), shapes.begin(), shapes.end());
    return snapshot;
}

// Collect shape intervals within a guard region including filler overlay
// changes.
CheckShapes ImplantLayerChecker::getOverlaySnapshot(
    const CheckRequest& request,
    const Rect& guardRegion,
    const FillerChanges& fillerChanges,
    bool useNewFillers,
    const std::set<InstanceId>& excludedNodes) const
{
    CheckShapes snapshot;

    RowId row0 = guardRegion._yl.getStorage() / rowHeight_;
    RowId row1 = guardRegion._yh.getStorage() / rowHeight_;
    ColId col0 = guardRegion._xl.getStorage() / siteWidth_;
    ColId col1 = guardRegion._xh.getStorage() / siteWidth_;
    for (RowId r = row0; r <= row1; r++) {
        for (ColId c = col0; c <= col1; c++) {
            Pixel* p = grid_->gridPixel(GridX{c}, GridY{r});
            if (p && p->cell) {
                const Node* node = p->cell;
                const InstanceId nodeId = node->getId();
                if (excludedNodes.find(nodeId) != excludedNodes.end()) {
                    continue;
                }
                if (node->getMaster() == nullptr) {
                    continue;
                }
                const std::pair<GridX, GridY> coord = grid_->gridXY(node);
                const CheckShapes& shapes
                    = getNodeShape(nodeId,
                                   node->getMaster()->getId(),
                                   coord.second.v,
                                   coord.first.v,
                                   node->getOrient(),
                                   false);
                snapshot.insert(snapshot.end(), shapes.begin(), shapes.end());
            }
        }
    }

    if (request.targetOp != OpType::Delete) {
        const CheckShapes& targetShapes = getNodeShape(request.instanceId,
                                                       request.masterId,
                                                       request.rowId,
                                                       request.colId,
                                                       request.orientation,
                                                       true);
        snapshot.insert(
            snapshot.end(), targetShapes.begin(), targetShapes.end());
    }

    int addIndex = 0;
    for (const CellChangeRecord& change : fillerChanges) {
        if (change.op_ == OpType::Add) {
            // [fillerRepair-layout] Added fillers use negative, request-local
            // participant IDs. They never enter Network or shared checker
            // state, so parallel candidates cannot collide.
            if (!useNewFillers) {
                ++addIndex;
                continue;
            }
            const MasterId masterId
                = network_->getMasterId(change.new_lib_cell_);
            const int64_t xRelative
                = change.x_.getStorage() - grid_->getCore().getXL().getStorage();
            const int64_t yRelative
                = change.y_.getStorage() - grid_->getCore().getYL().getStorage();
            const ColId colId = siteWidth_ > 0
                                    ? static_cast<ColId>(xRelative / siteWidth_)
                                    : 0;
            const RowId rowId
                = grid_->gridSnapDownY(
                    DbuY{static_cast<int>(yRelative)}).v;
            const InstanceId syntheticId = -1 - addIndex++;
            const CheckShapes& fillerShapes = getNodeShape(syntheticId,
                                                           masterId,
                                                           rowId,
                                                           colId,
                                                           change.orientation_,
                                                           true);
            snapshot.insert(
                snapshot.end(), fillerShapes.begin(), fillerShapes.end());
            continue;
        }
        if (change.op_ == OpType::Delete) {
            continue;
        }
        const LeafCellID* cellId = cellChangeLeafCellId(change);
        if (cellId == nullptr) {
            continue;
        }
        const InstanceId fillerInstId = network_->getNodeId(*cellId);
        const Node* fillerNode = network_->getNode(fillerInstId);
        if (!fillerNode || fillerNode->getMaster() == nullptr) {
            continue;
        }
        const RowId fillerRowId
            = grid_ ? grid_->gridSnapDownY(fillerNode).v : 0;
        const ColId fillerColId = grid_ ? grid_->gridX(fillerNode).v : 0;
        MasterId fillerMasterId = fillerNode->getMaster()->getId();
        if (useNewFillers) {
            fillerMasterId = network_->getMasterId(change.new_lib_cell_);
        }
        const PhysOrientation fillerOrientation
            = useNewFillers ? change.orientation_ : fillerNode->getOrient();
        const CheckShapes& fillerShapes = getNodeShape(fillerInstId,
                                                       fillerMasterId,
                                                       fillerRowId,
                                                       fillerColId,
                                                       fillerOrientation,
                                                       true);
        snapshot.insert(
            snapshot.end(), fillerShapes.begin(), fillerShapes.end());
    }
    return snapshot;
}

// Transform a master's canonical shapes to placed coordinates with orientation.
CheckShapes ImplantLayerChecker::getNodeShape(InstanceId instanceId,
                                              MasterId masterId,
                                              RowId rowId,
                                              ColId colId,
                                              PhysOrientation orientation,
                                              bool isCandidate) const
{
    CheckShapes shapes;
    if (masterId < 0 || static_cast<size_t>(masterId) >= masterItems_.size()
        || siteWidth_ <= 0 || rowHeight_ <= 0) {
        return shapes;
    }
    Dbu originX = colId * siteWidth_;
    Dbu originY = rowId * rowHeight_;
    const MasterItem& master = masterItems_[masterId];
    for (const MasterShape& shape : master.shapes) {
        Dbu xl = shape.rect._xl.getStorage();
        Dbu yl = shape.rect._yl.getStorage();
        Dbu xh = shape.rect._xh.getStorage();
        Dbu yh = shape.rect._yh.getStorage();
        if (orientation == PhysOrientationE::MY
            || orientation == PhysOrientationE::R180) {
            xl = master.width - xh;
            xh = master.width - xl;
        }
        if (orientation == PhysOrientationE::MX
            || orientation == PhysOrientationE::R180) {
            yl = master.height - yh;
            yh = master.height - yl;
        }
        CheckShape placed;
        placed.ownerInstanceIds.emplace_back(instanceId);
        placed.ownerShapeIds.emplace_back(shape.shapeId);
        placed.layer = shape.layer;
        placed.x = {originX + xl, originX + xh};
        placed.rowId = (originY + yl) / rowHeight_;
        placed.bandSlot = (originY + yl) % rowHeight_ > 0 ? BandSlot::Top
                                                          : BandSlot::Bottom;
        placed.isCandidate = isCandidate;
        shapes.push_back(placed);
    }
    return shapes;
}

// Merge adjacent/overlapping CheckShapes into unified CheckShapes per
// slot/layer.
CheckShapes ImplantLayerChecker::mergeShapes(const CheckShapes& rects) const
{
    CheckShapes sorted = rects;
    std::sort(sorted.begin(),
              sorted.end(),
              [](const CheckShape& left, const CheckShape& right) {
                  if (left.rowId != right.rowId) {
                      return left.rowId < right.rowId;
                  }
                  if (left.bandSlot != right.bandSlot) {
                      return left.bandSlot < right.bandSlot;
                  }
                  if (left.layer != right.layer) {
                      return left.layer < right.layer;
                  }
                  if (left.x.xl != right.x.xl) {
                      return left.x.xl < right.x.xl;
                  }
                  return left.x.xh < right.x.xh;
              });

    std::vector<CheckShape> shapes;
    int nextId = 1;
    for (const CheckShape& rect : sorted) {
        if (shapes.empty() || shapes.back().rowId != rect.rowId
            || shapes.back().bandSlot != rect.bandSlot
            || shapes.back().layer != rect.layer
            || shapes.back().isCandidate != rect.isCandidate
            || rect.x.xl > shapes.back().x.xh) {
            CheckShape shape = rect;
            shape.id = nextId++;
            shapes.push_back(shape);
            continue;
        }

        CheckShape& active = shapes.back();
        active.x.xh = std::max(active.x.xh, rect.x.xh);
        appendUnique(active.ownerInstanceIds, rect.ownerInstanceIds[0]);
        appendUnique(active.ownerShapeIds, rect.ownerShapeIds[0]);
        active.isCandidate = active.isCandidate || rect.isCandidate;
    }
    return shapes;
}

// Evaluate a single implant rule against all shapes,
// producing outcomes per target.
std::vector<CheckOutcome> ImplantLayerChecker::evalRule(
    const Rule& rule,
    const CheckShapes& shapes,
    const std::vector<XInterval>& tgtItvs) const
{
    std::vector<CheckOutcome> outcomes;
    if (!rule.getUnsupportedClauses().empty()) {
        CheckOutcome outcome;
        outcome.ruleId = rule.getRuleId();
        outcome.ruleSource = rule.getSource();
        outcome.status = OutcomeStatus::Skipped;
        outcomes.push_back(outcome);
        return outcomes;
    }

    for (const CheckShape& target : shapes) {
        if (!target.isCandidate || target.layer != rule.getPrimaryLayer()) {
            continue;
        }
        for (Relationship rlt : relations_) {
            if (!rule.contains(rlt)) {
                continue;
            }

            CheckShape checkTarget = target;
            if (rule.isWidth() && rlt == Relationship::InterRow) {
                for (const CheckShape& sameRowNeighbor : findNeighbors(
                         target, rule, Relationship::IntraRow, shapes)) {
                    if (target.layer == sameRowNeighbor.layer
                        && touchesOrOverlaps(checkTarget.x,
                                             sameRowNeighbor.x)) {
                        checkTarget.x = unite(checkTarget.x, sameRowNeighbor.x);
                        for (InstanceId id : sameRowNeighbor.ownerInstanceIds) {
                            appendUnique(checkTarget.ownerInstanceIds, id);
                        }
                        for (ShapeId id : sameRowNeighbor.ownerShapeIds) {
                            appendUnique(checkTarget.ownerShapeIds, id);
                        }
                    }
                }
            }

            const std::vector<CheckShape> neighbors
                = findNeighbors(checkTarget, rule, rlt, shapes);
            if (rule.isWidth() && neighbors.empty()
                && rlt == Relationship::IntraRow) {
                CheckOutcome outcome;
                outcome.ruleId = rule.getRuleId();
                outcome.ruleSource = rule.getSource();
                outcome.primaryLayer = rule.getPrimaryLayer();
                outcome.secondaryLayer = rule.getSecondaryLayer();
                outcome.relationship = rlt;
                outcome.targetId = checkTarget.id;
                outcome.xWindow = checkTarget.x;
                outcome.targetX = checkTarget.x;
                outcome.measuredValue = checkTarget.x.xh - checkTarget.x.xl;
                outcome.requiredValue = rule.getMinValue();
                if (touchesOrOverlaps(outcome.xWindow, tgtItvs)) {
                    outcome.status = outcome.measuredValue < rule.getMinValue()
                                         ? OutcomeStatus::Violated
                                         : OutcomeStatus::Satisfied;
                } else {
                    outcome.status = OutcomeStatus::Skipped;
                }
                outcome.instanceIds = checkTarget.ownerInstanceIds;
                outcome.rowIds = {checkTarget.rowId};
                outcomes.push_back(outcome);
                continue;
            }

            for (const CheckShape& neighbor : neighbors) {
                CheckOutcome outcome;
                outcome.ruleId = rule.getRuleId();
                outcome.ruleSource = rule.getSource();
                outcome.primaryLayer = rule.getPrimaryLayer();
                outcome.secondaryLayer = rule.getSecondaryLayer();
                outcome.relationship = rlt;
                outcome.targetId = checkTarget.id;
                outcome.neighborId = neighbor.id;
                outcome.xWindow = unite(checkTarget.x, neighbor.x);
                outcome.requiredValue = rule.getMinValue();
                outcome.instanceIds = checkTarget.ownerInstanceIds;
                outcome.rowIds = {checkTarget.rowId};
                for (InstanceId id : neighbor.ownerInstanceIds) {
                    appendUnique(outcome.instanceIds, id);
                }
                appendUnique(outcome.rowIds, neighbor.rowId);

                const Dbu projected = prl(checkTarget.x, neighbor.x);
                if (rule.getExceptAbutted() && projected == 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.getExceptCornerTouch() && rlt == Relationship::InterRow
                    && projected == 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.getLength() && rowHeight_ / 2 >= *rule.getLength()) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }

                if (!rule.getIntersectLayers().empty()
                    && !isIntersectCoverage(
                        rule, checkTarget, neighbor, shapes)) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }

                if (rule.getPrl()) {
                    const bool lef58VerticalSpacing
                        = rule.getSource() == RuleSource::Lef58Spacing
                          && rlt == Relationship::InterRow;
                    const bool lef58HorizontalSpacing
                        = rule.getSource() == RuleSource::Lef58Spacing
                          && rlt == Relationship::IntraRow;
                    if (!lef58HorizontalSpacing && *rule.getPrl() >= 0
                        && projected <= *rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (lef58VerticalSpacing && *rule.getPrl() < 0
                        && spacing(checkTarget.x, neighbor.x)
                               >= -*rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (!lef58HorizontalSpacing && !lef58VerticalSpacing
                        && *rule.getPrl() < 0
                        && spacing(target.x, neighbor.x) > -*rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                }

                if (rule.isWidth()) {
                    XInterval effective = checkTarget.x;
                    if (rlt == Relationship::InterRow) {
                        if (!overlaps(checkTarget.x, neighbor.x)) {
                            outcome.status = OutcomeStatus::NotApplicable;
                            outcomes.push_back(outcome);
                            continue;
                        }
                        effective = intersect(checkTarget.x, neighbor.x);
                    } else if (touchesOrOverlaps(checkTarget.x, neighbor.x)) {
                        effective = unite(checkTarget.x, neighbor.x);
                    }
                    outcome.xWindow = effective;
                    if (rule.getCheckGroup()
                        && !groupFails(rule,
                                       checkTarget,
                                       neighbor,
                                       rlt,
                                       effective,
                                       shapes)) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    outcome.measuredValue = effective.xh - effective.xl;
                    if (touchesOrOverlaps(outcome.xWindow, tgtItvs)) {
                        outcome.status
                            = outcome.measuredValue < rule.getMinValue()
                                  ? OutcomeStatus::Violated
                                  : OutcomeStatus::Satisfied;
                    } else {
                        outcome.status = OutcomeStatus::Skipped;
                    }
                } else {
                    if (checkTarget.layer == neighbor.layer
                        && touchesOrOverlaps(checkTarget.x, neighbor.x)) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    // Unlike same-layer spacing, distinct implant layers can
                    // legally be queried while their x intervals overlap.
                    // Keep a normalized window so the target-local outcome is
                    // evaluated instead of being skipped as a reversed gap.
                    outcome.xWindow
                        = touchesOrOverlaps(checkTarget.x, neighbor.x)
                              ? intersect(checkTarget.x, neighbor.x)
                              : gap(checkTarget.x, neighbor.x);
                    outcome.measuredValue
                        = rule.getSource() == RuleSource::Lef58Spacing
                                  && rlt == Relationship::InterRow
                              ? ADJACENT_ROW_VERTICAL_SPACING
                              : spacing(checkTarget.x, neighbor.x);
                    if (touchesOrOverlaps(outcome.xWindow, tgtItvs)) {
                        outcome.status
                            = outcome.measuredValue < rule.getMinValue()
                                  ? OutcomeStatus::Violated
                                  : OutcomeStatus::Satisfied;
                    } else {
                        outcome.status = OutcomeStatus::Skipped;
                    }
                }
                outcomes.push_back(outcome);
            }
        }
    }
    return outcomes;
}

// Find neighboring shapes within query radius for a given target
// and relationship.
CheckShapes ImplantLayerChecker::findNeighbors(const CheckShape& target,
                                               const Rule& rule,
                                               Relationship rlt,
                                               const CheckShapes& shapes) const
{
    std::vector<CheckShape> neighbors;
    const Dbu radius = queryRadius(rule);
    const XInterval queryWindow{target.x.xl - radius, target.x.xh + radius};
    const LayerId queryLayer
        = rule.getSecondaryLayer().value_or(rule.getPrimaryLayer());

    std::vector<SlotRef> slots;
    if (rlt == Relationship::IntraRow) {
        slots.push_back({target.rowId, target.bandSlot});
    } else {
        const SlotRef& nbr = getAdjSlot(target.rowId, target.bandSlot);
        if (nbr.rowId != -1) {
            slots.push_back(nbr);
        }
    }

    const bool returnSameRowRuns
        = rule.isWidth() && rlt == Relationship::InterRow;

    for (const CheckShape& shape : shapes) {
        if (shape.id == target.id) {
            continue;
        }
        if (shape.layer != queryLayer) {
            continue;
        }
        if (shape.x.xh < queryWindow.xl || shape.x.xl > queryWindow.xh) {
            continue;
        }
        bool slotMatches = false;
        for (SlotRef slot : slots) {
            if (shape.rowId == slot.rowId && shape.bandSlot == slot.bandSlot) {
                slotMatches = true;
                break;
            }
        }
        if (!slotMatches) {
            continue;
        }
        if (rlt == Relationship::IntraRow && rule.isWidth()
            && !touchesOrOverlaps(target.x, shape.x)) {
            continue;
        }
        neighbors.push_back(shape);
    }

    if (!returnSameRowRuns) {
        return neighbors;
    }

    std::sort(neighbors.begin(),
              neighbors.end(),
              [](const CheckShape& left, const CheckShape& right) {
                  if (left.rowId != right.rowId) {
                      return left.rowId < right.rowId;
                  }
                  if (left.bandSlot != right.bandSlot) {
                      return left.bandSlot < right.bandSlot;
                  }
                  if (left.layer != right.layer) {
                      return left.layer < right.layer;
                  }
                  if (left.x.xl != right.x.xl) {
                      return left.x.xl < right.x.xl;
                  }
                  return left.x.xh < right.x.xh;
              });

    std::vector<CheckShape> runs;
    for (const CheckShape& shape : neighbors) {
        if (runs.empty() || runs.back().rowId != shape.rowId
            || runs.back().bandSlot != shape.bandSlot
            || runs.back().layer != shape.layer
            || !touchesOrOverlaps(runs.back().x, shape.x)) {
            runs.push_back(shape);
            continue;
        }

        CheckShape& run = runs.back();
        run.x = unite(run.x, shape.x);
        run.isCandidate = run.isCandidate || shape.isCandidate;
        for (InstanceId id : shape.ownerInstanceIds) {
            appendUnique(run.ownerInstanceIds, id);
        }
        for (ShapeId id : shape.ownerShapeIds) {
            appendUnique(run.ownerShapeIds, id);
        }
    }

    runs.erase(std::remove_if(runs.begin(),
                              runs.end(),
                              [&target](const CheckShape& shape) {
                                  return !overlaps(target.x, shape.x);
                              }),
               runs.end());
    return runs;
}

// Check if one outcome context is contained within another (for suppression).
bool ImplantLayerChecker::contained(const CheckOutcome& specific,
                                    const CheckOutcome& broad) const
{
    if (specific.ruleSource != broad.ruleSource
        || specific.primaryLayer != broad.primaryLayer
        || specific.secondaryLayer != broad.secondaryLayer
        || specific.relationship != broad.relationship
        || specific.targetId != broad.targetId) {
        return false;
    }
    if (broad.neighborId && specific.neighborId != broad.neighborId) {
        return false;
    }
    return specific.xWindow.xl >= broad.xWindow.xl
           && specific.xWindow.xh <= broad.xWindow.xh;
}

// Convert violated outcomes into Violation objects, suppressing contained ones.
std::vector<Violation> ImplantLayerChecker::makeViolations(
    const std::vector<CheckOutcome>& outcomes,
    const CheckShapes& shapes) const
{
    std::set<size_t> suppressed;
    for (size_t broadIndex = 0; broadIndex < outcomes.size(); ++broadIndex) {
        const CheckOutcome& broad = outcomes[broadIndex];
        if (broad.status != OutcomeStatus::Violated) {
            continue;
        }
        const Rule& broadRule = rules_[broad.ruleId];
        if (!broadRule.getContainmentGroup()) {
            continue;
        }
        for (const CheckOutcome& specific : outcomes) {
            if (specific.status != OutcomeStatus::Satisfied) {
                continue;
            }
            const Rule& specificRule = rules_[specific.ruleId];
            if (specificRule.getContainmentGroup()
                != broadRule.getContainmentGroup()) {
                continue;
            }
            if (std::find(specificRule.getContainedByRuleIds().begin(),
                          specificRule.getContainedByRuleIds().end(),
                          broad.ruleId)
                == specificRule.getContainedByRuleIds().end()) {
                continue;
            }
            if (contained(specific, broad)) {
                suppressed.insert(broadIndex);
                break;
            }
        }
    }

    std::vector<Violation> violations;
    for (size_t i = 0; i < outcomes.size(); ++i) {
        const CheckOutcome& outcome = outcomes[i];
        if (outcome.status != OutcomeStatus::Violated
            || suppressed.find(i) != suppressed.end()) {
            continue;
        }
        Violation violation;
        violation.ruleId = outcome.ruleId;
        violation.ruleSource = outcome.ruleSource;
        violation.primaryLayer = outcome.primaryLayer;
        violation.secondaryLayer = outcome.secondaryLayer;
        violation.instances = outcome.instanceIds;
        violation.measuredValue = outcome.measuredValue;
        violation.requiredValue = outcome.requiredValue;
        violation.xWindow = outcome.xWindow;
        violation.relationship = outcome.relationship;
        // Find the target shape.
        auto targetIt = std::find_if(
            shapes.begin(), shapes.end(), [&](const CheckShape& s) {
                return s.id == outcome.targetId;
            });
        if (targetIt != shapes.end()) {
            XInterval mergedX = targetIt->x;
            const bool isWidthInterRow
                = outcome.ruleSource == RuleSource::Width
                  && outcome.relationship == Relationship::InterRow;
            if (isWidthInterRow) {
                for (const CheckShape& sameRow : shapes) {
                    if (sameRow.id == outcome.targetId) {
                        continue;
                    }
                    if (sameRow.layer != targetIt->layer) {
                        continue;
                    }
                    if (sameRow.rowId != targetIt->rowId
                        || sameRow.bandSlot != targetIt->bandSlot) {
                        continue;
                    }
                    if (touchesOrOverlaps(mergedX, sameRow.x)) {
                        mergedX = unite(mergedX, sameRow.x);
                    }
                }
            }
            violation.targetInterval = mergedX;
        } else {
            violation.targetInterval = outcome.targetX;
        }

        auto shapeIt = std::find_if(
            shapes.begin(), shapes.end(), [&](const CheckShape& s) {
                return outcome.neighborId.has_value()
                       && s.id == *outcome.neighborId;
            });
        violation.neighborInterval
            = shapeIt != shapes.end() ? shapeIt->x : violation.targetInterval;

        // Look up layer name
        violation.layerName = layers_[outcome.primaryLayer].getName();
        violation.rowIds = outcome.rowIds;
        finishViolation(violation);
        violations.push_back(violation);
    }
    std::sort(violations.begin(),
              violations.end(),
              [](const Violation& left, const Violation& right) {
                  if (left.ruleId != right.ruleId) {
                      return left.ruleId < right.ruleId;
                  }
                  if (left.primaryLayer != right.primaryLayer) {
                      return left.primaryLayer < right.primaryLayer;
                  }
                  if (left.relationship != right.relationship) {
                      return left.relationship < right.relationship;
                  }
                  return lessXInterval(left.xWindow, right.xWindow);
              });
    return violations;
}

// [fillerRepair-layout] Check the pixel grid against the requested target
// footprint. The Network Node may already carry the candidate master while
// Grid/UDM still describe the pre-commit placement.
OverlapInfo ImplantLayerChecker::checkOverlap(
    const CheckRequest& request) const
{
    OverlapInfo info;
    const Node* node
        = network_ != nullptr ? network_->getNode(request.instanceId) : nullptr;
    if (node == nullptr && request.targetOp != OpType::Add) {
        info.diags = Diagnostic{"unknown_target_instance",
                                "cannot check overlap for a null node"};
        return info;
    }
    if (grid_ == nullptr || siteWidth_ <= 0 || rowHeight_ <= 0
        || request.masterId < 0
        || request.masterId >= static_cast<MasterId>(masterItems_.size())) {
        info.diags = Diagnostic{"unknown_target_master",
                                makeMessage("master ", request.masterId)};
        return info;
    }
    const MasterItem& master = masterItems_[request.masterId];
    const int widthSites
        = std::max(1, (master.width + siteWidth_ - 1) / siteWidth_);
    const int heightRows
        = std::max(1, (master.height + rowHeight_ - 1) / rowHeight_);
    const GridX xEnd{request.colId + widthSites};
    const GridY yEnd{request.rowId + heightRows};
    for (GridX x{request.colId}; x < xEnd; x++) {
        for (GridY y{request.rowId}; y < yEnd; y++) {
            Pixel* pixel = grid_->gridPixel(x, y);
            Node* node2 = pixel ? pixel->cell : nullptr;
            if (node2 != nullptr && node2 != node) {
                if (node2->isFiller()) {
                    info.fillers.insert(node2->getId());
                } else {
                    info.diags = Diagnostic{
                        "placement_overlap_in_input",
                        "inst " + std::to_string(request.instanceId) + ", "
                            + std::to_string(node2->getId())};
                    return info;
                }
            }
        }
    }
    return info;
}

bool ImplantLayerChecker::targetFootprintChanged(
    const CheckRequest& request) const
{
    // [fillerRepair-layout] This query is read-only and O(1). It is used only
    // to decide whether a direct-legal target still needs filler Delete/Add.
    if (grid_ == nullptr || grid_->getDesMgr() == nullptr || network_ == nullptr
        || siteWidth_ <= 0 || rowHeight_ <= 0 || request.masterId < 0
        || request.masterId >= static_cast<MasterId>(masterItems_.size())) {
        return false;
    }
    const Node* node = network_->getNode(request.instanceId);
    if (node == nullptr) {
        return false;
    }
    const eUNL::PhysCell physical
        = grid_->getDesMgr()->getPhysCell(node->getDbInst());
    if (!physical.isValid()) {
        return false;
    }
    const MasterItem& requested = masterItems_[request.masterId];
    const int requestedWidth
        = std::max(1, (requested.width + siteWidth_ - 1) / siteWidth_);
    const int requestedHeight
        = std::max(1, (requested.height + rowHeight_ - 1) / rowHeight_);
    const int oldWidth = std::max(
        1,
        (physical.getPhysMaster().getWidth().getStorage() + siteWidth_ - 1)
            / siteWidth_);
    const int oldHeight = std::max(
        1,
        (physical.getPhysMaster().getHeight().getStorage() + rowHeight_ - 1)
            / rowHeight_);
    const Rect core = grid_->getCore();
    const int oldCol
        = (physical.getOrigin().getX().getStorage()
           - core.getXL().getStorage())
          / siteWidth_;
    const int oldY
        = physical.getOrigin().getY().getStorage()
          - core.getYL().getStorage();
    const int oldRow = grid_->gridSnapDownY(DbuY{oldY}).v;
    return oldCol != request.colId || oldRow != request.rowId
           || oldWidth != requestedWidth || oldHeight != requestedHeight;
}

// Validate an overlay placement request and its filler changes for consistency.
DiagVec ImplantLayerChecker::validateOverlayRequest(
    const CheckRequest& request,
    const FillerChanges& fillerChanges,
    bool enforceLayoutTransaction) const
{
    std::vector<Diagnostic> diagnostics;
    if (network_ == nullptr) {
        diagnostics.push_back(
            {"missing_network",
             "overlay validation requires an initialized Network"});
        return diagnostics;
    }
    if (siteWidth_ <= 0) {
        diagnostics.push_back({"placement_not_site_aligned",
                               makeMessage("instance ", request.instanceId)});
    }
    if (grid_ == nullptr || request.rowId < 0 || request.colId < 0
        || request.rowId >= grid_->getRowCount().v
        || request.colId >= grid_->getRowSiteCount().v) {
        diagnostics.push_back({"placement_out_of_grid",
                               makeMessage("instance ", request.instanceId)});
    }
    const Node* targetNode = network_->getNode(request.instanceId);
    if (request.targetOp == OpType::Add && targetNode != nullptr) {
        diagnostics.push_back({"added_target_already_exists",
                               makeMessage("instance ", request.instanceId)});
    } else if (request.targetOp != OpType::Add && targetNode == nullptr) {
        diagnostics.push_back({"unknown_target_instance",
                               makeMessage("instance ", request.instanceId)});
    } else if (targetNode != nullptr && targetNode->getMaster() == nullptr) {
        diagnostics.push_back({"target_instance_missing_master",
                               makeMessage("instance ", request.instanceId)});
    }
    const bool targetHasData
        = request.masterId >= 0
          && request.masterId < static_cast<MasterId>(masterItems_.size())
          && masterItems_[request.masterId].width > 0;
    if (!targetHasData) {
        diagnostics.push_back({"unknown_target_master",
                               makeMessage("master ", request.masterId)});
    } else {
        // [fillerRepair-layout] Validate the complete target rectangle, not
        // only its origin. This is required for 2-row and growing masters.
        const MasterItem& targetMaster = masterItems_[request.masterId];
        const int widthSites
            = std::max(1,
                       (targetMaster.width + siteWidth_ - 1) / siteWidth_);
        const int heightRows
            = rowHeight_ > 0
                  ? std::max(1,
                             (targetMaster.height + rowHeight_ - 1)
                                 / rowHeight_)
                  : 1;
        if (request.colId + widthSites > grid_->getRowSiteCount().v
            || request.rowId + heightRows > grid_->getRowCount().v) {
            diagnostics.push_back(
                {"placement_out_of_grid",
                 makeMessage("target footprint outside grid ",
                             request.instanceId)});
        }
    }

    struct AddedPlacement
    {
        std::string name;
        MasterId masterId = -1;
        RowId rowId = -1;
        ColId colId = -1;
        int widthSites = 0;
        int heightRows = 0;
    };
    std::vector<AddedPlacement> additions;
    std::set<InstanceId> seenExisting;
    std::set<InstanceId> deleted;
    std::set<std::string> seenNames;
    for (const CellChangeRecord& change : fillerChanges) {
        if (change.op_ == OpType::Add) {
            // [fillerRepair-layout] An Add is request-local and therefore
            // names a cell that does not exist in Network yet.
            const std::string* name = cellChangeName(change);
            if (name == nullptr || name->empty()) {
                diagnostics.push_back(
                    {"added_cell_data_not_name",
                     "Add record must carry a non-empty request-local name"});
                continue;
            }
            if (!seenNames.insert(*name).second) {
                diagnostics.push_back(
                    {"duplicate_added_filler_name",
                     "duplicate added filler name " + *name});
                continue;
            }
            const MasterId newMasterId
                = network_->getMasterId(change.new_lib_cell_);
            const bool newMasterHasData
                = newMasterId >= 0
                  && newMasterId < static_cast<MasterId>(masterItems_.size())
                  && masterItems_[newMasterId].width > 0;
            const Master* newMaster = newMasterHasData
                                          ? network_->getMaster(newMasterId)
                                          : nullptr;
            if (!newMasterHasData || newMaster == nullptr
                || !newMaster->isFiller()) {
                diagnostics.push_back(
                    {"added_master_not_filler",
                     makeMessage("added master is not a filler ",
                                 newMasterId)});
                continue;
            }
            const int64_t xRelative
                = change.x_.getStorage() - grid_->getCore().getXL().getStorage();
            const int64_t yRelative
                = change.y_.getStorage() - grid_->getCore().getYL().getStorage();
            if (xRelative < 0 || yRelative < 0
                || xRelative % siteWidth_ != 0) {
                diagnostics.push_back(
                    {"added_filler_not_site_aligned",
                     "added filler " + *name + " is not site aligned"});
                continue;
            }
            const ColId colId = static_cast<ColId>(xRelative / siteWidth_);
            const RowId rowId = grid_->gridSnapDownY(
                DbuY{static_cast<int>(yRelative)}).v;
            if (grid_->gridYToDbu(GridY{rowId}).v != yRelative) {
                diagnostics.push_back(
                    {"added_filler_not_row_aligned",
                     "added filler " + *name + " is not row aligned"});
                continue;
            }
            const MasterItem& item = masterItems_[newMasterId];
            const int widthSites
                = std::max(1, (item.width + siteWidth_ - 1) / siteWidth_);
            const int heightRows
                = rowHeight_ > 0
                      ? std::max(1,
                                 (item.height + rowHeight_ - 1) / rowHeight_)
                      : 1;
            if (colId < 0 || rowId < 0
                || colId + widthSites > grid_->getRowSiteCount().v
                || rowId + heightRows > grid_->getRowCount().v) {
                diagnostics.push_back(
                    {"added_filler_out_of_grid",
                     "added filler " + *name + " is outside the grid"});
                continue;
            }
            if (!item.siteName.empty()) {
                const std::optional<PhysOrientation> expected
                    = grid_->getSiteOrientation(
                        GridX{colId}, GridY{rowId}, item.siteName);
                if (!expected.has_value()
                    || expected->getValue() != change.orientation_.getValue()) {
                    diagnostics.push_back(
                        {"added_filler_orientation_mismatch",
                         "added filler " + *name
                             + " does not match row/site orientation"});
                    continue;
                }
            }
            additions.push_back(
                {*name, newMasterId, rowId, colId, widthSites, heightRows});
            continue;
        }

        if (change.op_ != OpType::Replace
            && change.op_ != OpType::Delete) {
            diagnostics.push_back(
                {"unsupported_cell_change_operation",
                 "filler overlay supports Replace, Delete, and Add only"});
            continue;
        }
        const LeafCellID* cellId = cellChangeLeafCellId(change);
        if (cellId == nullptr) {
            diagnostics.push_back(
                {"changed_cell_data_not_leaf_id",
                 "Replace/Delete record must identify an existing LeafCellID"});
            continue;
        }
        const InstanceId fillerInstId = network_->getNodeId(*cellId);
        if (!seenExisting.insert(fillerInstId).second) {
            diagnostics.push_back(
                {"duplicate_filler_change",
                 makeMessage("duplicate filler change ", fillerInstId)});
            continue;
        }
        if (request.targetOp != OpType::Add
            && fillerInstId == request.instanceId) {
            diagnostics.push_back(
                {"target_cannot_be_changed_filler",
                 makeMessage("target cannot be changed filler ",
                             fillerInstId)});
        }
        const Node* fillerNode = network_->getNode(fillerInstId);
        if (fillerNode == nullptr) {
            diagnostics.push_back(
                {"unknown_filler_instance",
                 makeMessage("unknown filler instance ", fillerInstId)});
            continue;
        }
        if (fillerNode->getMaster() == nullptr) {
            diagnostics.push_back(
                {"filler_instance_missing_master",
                 makeMessage("filler instance ", fillerInstId)});
            continue;
        }
        if (!fillerNode->isFiller()) {
            diagnostics.push_back(
                {"changed_instance_not_filler",
                 makeMessage("changed instance is not filler ", fillerInstId)});
        }
        if (change.op_ == OpType::Delete) {
            deleted.insert(fillerInstId);
            continue;
        }

        const MasterId newMasterId
            = network_->getMasterId(change.new_lib_cell_);
        const bool newMasterHasData
            = newMasterId >= 0
              && newMasterId < static_cast<MasterId>(masterItems_.size())
              && masterItems_[newMasterId].width > 0;
        if (!newMasterHasData) {
            diagnostics.push_back(
                {"unknown_filler_master",
                 makeMessage("unknown filler master ", newMasterId)});
            continue;
        }
        const Master* newMaster = network_->getMaster(newMasterId);
        if (newMaster == nullptr || !newMaster->isFiller()) {
            diagnostics.push_back(
                {"replacement_master_not_filler",
                 makeMessage("replacement master is not filler ",
                             newMasterId)});
        }
        const MasterId oldMasterId = fillerNode->getMaster()->getId();
        const bool oldMasterHasData
            = oldMasterId >= 0
              && oldMasterId < static_cast<MasterId>(masterItems_.size())
              && masterItems_[oldMasterId].width > 0;
        if (oldMasterHasData && newMasterHasData
            && (masterItems_[oldMasterId].width
                    != masterItems_[newMasterId].width
                || masterItems_[oldMasterId].height
                       != masterItems_[newMasterId].height)) {
            diagnostics.push_back(
                {"replacement_footprint_mismatch",
                 makeMessage("replacement footprint mismatch ", fillerInstId)});
        }
    }

    // A new std-cell Add must explicitly delete every filler under its
    // footprint. Replace is same-footprint-only and Delete has no new target.
    if (enforceLayoutTransaction && targetHasData
        && request.targetOp == OpType::Add) {
        const OverlapInfo overlap = checkOverlap(request);
        if (overlap.diags.has_value()) {
            diagnostics.push_back(*overlap.diags);
        }
        for (const InstanceId filler : overlap.fillers) {
            if (deleted.count(filler) == 0) {
                diagnostics.push_back(
                    {"overlapping_filler_not_deleted",
                     makeMessage("target-overlapping filler not deleted ",
                                 filler)});
            }
        }
    }

    if (!targetHasData || grid_ == nullptr) {
        return diagnostics;
    }
    const MasterItem& targetMaster = masterItems_[request.masterId];
    const int targetWidth
        = std::max(1, (targetMaster.width + siteWidth_ - 1) / siteWidth_);
    const int targetHeight
        = rowHeight_ > 0
              ? std::max(
                    1, (targetMaster.height + rowHeight_ - 1) / rowHeight_)
              : 1;
    std::set<std::pair<RowId, ColId>> addedSites;
    for (const AddedPlacement& add : additions) {
        for (int rowOffset = 0; rowOffset < add.heightRows; ++rowOffset) {
            for (int colOffset = 0; colOffset < add.widthSites; ++colOffset) {
                const RowId row = add.rowId + rowOffset;
                const ColId col = add.colId + colOffset;
                if (request.targetOp != OpType::Delete
                    && row >= request.rowId
                    && row < request.rowId + targetHeight
                    && col >= request.colId
                    && col < request.colId + targetWidth) {
                    diagnostics.push_back(
                        {"added_filler_overlaps_target",
                         "added filler " + add.name + " overlaps target"});
                    continue;
                }
                if (!addedSites.emplace(row, col).second) {
                    diagnostics.push_back(
                        {"added_fillers_overlap",
                         "added filler " + add.name
                             + " overlaps another added filler"});
                    continue;
                }
                const Pixel* pixel = grid_->gridPixel(GridX{col}, GridY{row});
                if (pixel == nullptr || !pixel->is_valid
                    || pixel->padding_reserved_by != nullptr) {
                    diagnostics.push_back(
                        {"added_filler_on_illegal_site",
                         "added filler " + add.name
                             + " covers an illegal or reserved site"});
                    continue;
                }
                const Node* occupant = pixel->cell;
                if (occupant == nullptr || occupant->getId() == request.instanceId
                    || deleted.count(occupant->getId()) != 0) {
                    continue;
                }
                diagnostics.push_back(
                    {"added_filler_overlaps_input",
                     makeMessage("added filler overlaps instance ",
                                 occupant->getId())});
            }
        }
    }
    return diagnostics;
}

bool ImplantLayerChecker::touchesInstance(const Violation& violation,
                                          InstanceId instanceId) const
{
    return std::find(violation.instances.begin(),
                     violation.instances.end(),
                     instanceId)
           != violation.instances.end();
}

bool ImplantLayerChecker::containsViolation(const Violation& oldViolation,
                                            const Violation& newViolation) const
{
    if (oldViolation.hash != newViolation.hash
        || !contains(oldViolation.xWindow, newViolation.xWindow)) {
        return false;
    }
    for (InstanceId instanceId : newViolation.instances) {
        if (std::find(oldViolation.instances.begin(),
                      oldViolation.instances.end(),
                      instanceId)
            == oldViolation.instances.end()) {
            return false;
        }
    }
    return true;
}

bool ImplantLayerChecker::isInGuard(const XInterval& xWindow,
                                    const RowIdVec& rowIds,
                                    const Rect& guard) const
{
    if (!overlaps(
            xWindow,
            XInterval{guard.getXL().getStorage(), guard.getXH().getStorage()})
        && !touchesOrOverlaps(xWindow,
                              XInterval{guard.getXL().getStorage(),
                                        guard.getXH().getStorage()})) {
        return false;
    }
    if (rowHeight_ <= 0 || rowIds.empty()) {
        return true;
    }
    for (RowId rowId : rowIds) {
        const Dbu rowYl = static_cast<Dbu>(rowId) * rowHeight_;
        const Dbu rowYh = rowYl + rowHeight_;
        if (overlaps(XInterval{rowYl, rowYh},
                     XInterval{guard.getYL().getStorage(),
                               guard.getYH().getStorage()})
            || touchesOrOverlaps(XInterval{rowYl, rowYh},
                                 XInterval{guard.getYL().getStorage(),
                                           guard.getYH().getStorage()})) {
            return true;
        }
    }
    return false;
}

void ImplantLayerChecker::finishViolation(Violation& violation) const
{
    // sortUnique(violation.instances);
    sortUnique(violation.rowIds);

    uint64_t hash = 14695981039346656037ull;
    hashAppend(hash, signedHashValue(violation.ruleId));
    hashAppend(hash, signedHashValue(enumInt(violation.ruleSource)));
    hashAppend(hash, signedHashValue(enumInt(violation.relationship)));
    hashAppend(hash, signedHashValue(violation.primaryLayer));
    hashAppend(hash, violation.secondaryLayer ? 1ull : 0ull);
    if (violation.secondaryLayer) {
        hashAppend(hash, signedHashValue(*violation.secondaryLayer));
    }
    for (RowId rowId : violation.rowIds) {
        hashAppend(hash, signedHashValue(rowId));
    }
    violation.hash = hash;
}

bool ImplantLayerChecker::slotPolarityOk(const CheckShape& shape) const
{
    const Layer::Polar expectedPolar = slotPolar(shape.rowId, shape.bandSlot);
    uvAssert((unsigned) shape.layer < layers_.size());
    return layers_[shape.layer].getPolar() == expectedPolar;
}

// Check that all intersect layers in a rule cover the gap between
// target and neighbor.
bool ImplantLayerChecker::isIntersectCoverage(const Rule& rule,
                                              const CheckShape& target,
                                              const CheckShape& neighbor,
                                              const CheckShapes& shapes) const
{
    const XInterval gap{std::min(target.x.xh, neighbor.x.xh),
                        std::max(target.x.xl, neighbor.x.xl)};
    for (LayerId layer : rule.getIntersectLayers()) {
        bool covered = false;
        for (const CheckShape& shape : shapes) {
            if (shape.rowId == target.rowId && shape.bandSlot == target.bandSlot
                && shape.layer == layer && shape.x.xl <= gap.xl
                && shape.x.xh >= gap.xh) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            return false;
        }
    }
    return true;
}

// Check if a group implant width rule fails across target and neighbor shapes.
bool ImplantLayerChecker::groupFails(const Rule& rule,
                                     const CheckShape& target,
                                     const CheckShape& neighbor,
                                     Relationship rlt,
                                     const XInterval& xWindow,
                                     const CheckShapes& shapes) const
{
    if (!rule.getCheckGroup()) {
        return true;
    }
    const auto layersIt = layerGroups_.find(*rule.getCheckGroup());
    if (layersIt == layerGroups_.end()) {
        return false;
    }

    auto inGroup = [&](LayerId layer) {
        return std::find(
                   layersIt->second.begin(), layersIt->second.end(), layer)
               != layersIt->second.end();
    };
    auto groupShapesFor = [&](RowId rowId, BandSlot bandSlot) {
        CheckShapes groupShapes;
        for (const CheckShape& shape : shapes) {
            if (shape.rowId != rowId || shape.bandSlot != bandSlot
                || !inGroup(shape.layer)) {
                continue;
            }
            groupShapes.push_back(shape);
        }
        return mergeGroupShapes(groupShapes, false);
    };

    Dbu maxWidth = 0;
    const std::vector<CheckShape> targetShapes
        = groupShapesFor(target.rowId, target.bandSlot);
    if (rlt == Relationship::InterRow) {
        const std::vector<CheckShape> neighborShapes
            = groupShapesFor(neighbor.rowId, neighbor.bandSlot);
        for (const CheckShape& left : targetShapes) {
            for (const CheckShape& right : neighborShapes) {
                if (!touchesOrOverlaps(left.x, right.x)) {
                    continue;
                }
                const XInterval common = intersect(left.x, right.x);
                maxWidth = std::max(maxWidth, common.xh - common.xl);
            }
        }
    } else {
        for (const CheckShape& shape : targetShapes) {
            if (!touchesOrOverlaps(shape.x, xWindow)) {
                continue;
            }
            maxWidth = std::max(maxWidth, shape.x.xh - shape.x.xl);
        }
    }
    return maxWidth < rule.getMinValue();
}

// Merge overlapping group shapes into contiguous CheckShapes per slot.
CheckShapes ImplantLayerChecker::mergeGroupShapes(const CheckShapes& rawShapes,
                                                  bool isCandidate) const
{
    CheckShapes sorted = rawShapes;
    std::sort(sorted.begin(),
              sorted.end(),
              [](const CheckShape& left, const CheckShape& right) {
                  if (left.rowId != right.rowId) {
                      return left.rowId < right.rowId;
                  }
                  if (left.bandSlot != right.bandSlot) {
                      return left.bandSlot < right.bandSlot;
                  }
                  if (left.x.xl != right.x.xl) {
                      return left.x.xl < right.x.xl;
                  }
                  if (left.x.xh != right.x.xh) {
                      return left.x.xh < right.x.xh;
                  }
                  return left.layer < right.layer;
              });

    std::vector<CheckShape> merged;
    merged.reserve(sorted.size());
    int nextCandidateId = -1;
    for (const CheckShape& raw : sorted) {
        if (merged.empty() || merged.back().rowId != raw.rowId
            || merged.back().bandSlot != raw.bandSlot
            || raw.x.xl > merged.back().x.xh) {
            CheckShape shape = raw;
            shape.id = isCandidate ? nextCandidateId-- : 0;
            shape.isCandidate = isCandidate;
            merged.push_back(shape);
            continue;
        }
        CheckShape& active = merged.back();
        active.x.xh = std::max(active.x.xh, raw.x.xh);
        appendUnique(active.ownerInstanceIds, raw.ownerInstanceIds[0]);
        appendUnique(active.ownerShapeIds, raw.ownerShapeIds[0]);
    }
    return merged;
}
// Compute the x search radius needed to find all possible neighbors for a rule.
Dbu ImplantLayerChecker::queryRadius(const Rule& rule) const
{
    Dbu radius = rule.getMinValue();
    if (rule.getPrl()) {
        radius = std::max(radius, static_cast<Dbu>(std::abs(*rule.getPrl())));
    }
    if (rule.getLength()) {
        radius = std::max(radius, *rule.getLength());
    }
    return radius;
}

// --------------------------------------------------------------------------------
// Parse layer name to extract family and polarity
// Expected format: "<FAMILY>_<POLARITY>" e.g. "VTUL_N", "VTL_P", "VTH_N"
// --------------------------------------------------------------------------------
void ImplantLayerChecker::parseLayerName(const std::string& name,
                                         Layer::Vt& family,
                                         Layer::Polar& polarity)
{
    family = Layer::Vt::Unknown;
    polarity = Layer::Polar::N;
    auto pos = name.rfind('_');
    if (pos == std::string::npos) {
        return;
    }
    std::string famStr = name.substr(0, pos);
    std::string polStr = name.substr(pos + 1);
    polarity = ((polStr == "P" || polStr == "p") ? Layer::Polar::P
                                                 : Layer::Polar::N);

    if (famStr == "VTS" || famStr == "vts") {
        family = Layer::Vt::S;
    } else if (famStr == "VTL" || famStr == "vtl") {
        family = Layer::Vt::L;
    } else if (famStr == "VTH" || famStr == "vth") {
        family = Layer::Vt::H;
    } else if (famStr == "VTUL" || famStr == "vtul") {
        family = Layer::Vt::UL;
    } else {
        family = Layer::Vt::Unknown;
    }
}

LayerId ImplantLayerChecker::getLayerId(eLIB::TechLayerRelativeID relId) const
{
    auto it = techLayerToIdx_.find(relId);
    return (it != techLayerToIdx_.end()) ? it->second : -1;
}

LayerId ImplantLayerChecker::getLayerId(const std::string& name) const
{
    auto it = layerNameToIdx_.find(name);
    return (it != layerNameToIdx_.end()) ? it->second : -1;
}

// --------------------------------------------------------------------------------
// filler support: allow filler change
// --------------------------------------------------------------------------------
// Evaluate multiple filler overlay variants against a placement
// request in parallel.
std::vector<CheckResult> ImplantLayerChecker::checkPlaceWithOverlays(
    const CheckRequest& request,
    const Rect& guardRegion,
    const std::vector<FillerChanges>& fillerChanges) const
{
    unsigned size = fillerChanges.size();
    std::vector<CheckResult> results(size);
    if (!hasUsableInfrastructure()) {
        for (CheckResult& result : results) {
            result.isLegal = false;
            result.diagnostics = diagnostics_;
        }
        return results;
    }
    std::shared_lock<std::shared_mutex> masterLock(masterItemsMutex_);
    // [fillerRepair-layout] This is target-only batch setup. Each concrete
    // candidate below still receives full transaction validation.
    const DiagVec targetDiagnostics
        = validateOverlayRequest(request, {}, false);
    if (!targetDiagnostics.empty()) {
        for (CheckResult& result : results) {
            result.isLegal = false;
            result.diagnostics = diagnostics_;
            result.diagnostics.insert(result.diagnostics.end(),
                                      targetDiagnostics.begin(),
                                      targetDiagnostics.end());
        }
        return results;
    }
    const std::vector<Violation> oldViolations
        = checkOverlayRegion(request, guardRegion, {}, false).violations;
    auto run_job = [&](int i) {
        results[i] = checkPlaceWithOverlay(
            request, guardRegion, fillerChanges[i], oldViolations);
    };
    if (DrcUtil::getArena()) {
        DrcUtil::parallelFor(size, run_job);
    } else {
        for (unsigned i = 0; i < size; i++) {
            run_job(i);
        }
    }
    return results;
}

// Evaluate one filler overlay: filter new violations to only those touching
// the target.
CheckResult ImplantLayerChecker::checkPlaceWithOverlay(
    const CheckRequest& request,
    const Rect& guardRegion,
    const FillerChanges& fillerChanges,
    const std::vector<Violation>& oldViolations) const
{
    CheckResult result;
    result.diagnostics = diagnostics_;
    const std::vector<Diagnostic> requestDiagnostics
        = validateOverlayRequest(request, fillerChanges);
    result.diagnostics.insert(result.diagnostics.end(),
                              requestDiagnostics.begin(),
                              requestDiagnostics.end());
    if (!requestDiagnostics.empty()) {
        result.isLegal = false;
        return result;
    }

    const CheckResult overlay
        = checkOverlayRegion(request, guardRegion, fillerChanges, true);
    result.diagnostics.insert(result.diagnostics.end(),
                              overlay.diagnostics.begin(),
                              overlay.diagnostics.end());

    std::vector<Violation> blocking;
    for (const Violation& violation : overlay.violations) {
        if (touchesInstance(violation, request.instanceId)) {
            blocking.push_back(violation);
            continue;
        }
        const bool isOld
            = std::any_of(oldViolations.begin(),
                          oldViolations.end(),
                          [&violation, this](const Violation& oldViolation) {
                              return containsViolation(oldViolation, violation);
                          });
        if (!isOld) {
            blocking.push_back(violation);
        }
    }

    result.violations = std::move(blocking);
    result.isLegal = result.violations.empty() && requestDiagnostics.empty()
                     && overlay.diagnostics.empty();
    return result;
}

// Run all rules within a guard region, accounting for old vs new filler
// masters.
CheckResult ImplantLayerChecker::checkOverlayRegion(
    const CheckRequest& request,
    const Rect& guardRegion,
    const FillerChanges& fillerChanges,
    bool useNewFillers) const
{
    CheckResult result;
    result.diagnostics = diagnostics_;
    if (siteWidth_ <= 0) {
        result.diagnostics.push_back(
            {"placement_not_site_aligned",
             makeMessage("instance ", request.instanceId)});
        result.isLegal = false;
        return result;
    }

    std::set<InstanceId> excludedNodes;
    Node* node = network_->getNode(request.instanceId);
    if (request.targetOp != OpType::Add
        && (node == nullptr || node->getMaster() == nullptr)) {
        result.diagnostics.push_back(
            {node == nullptr ? "unknown_target_instance"
                             : "target_instance_missing_master",
             makeMessage("instance ", request.instanceId)});
        result.isLegal = false;
        return result;
    }
    if (node != nullptr) {
        excludedNodes.insert(request.instanceId);
    }
    if (request.targetOp == OpType::Add) {
        // The baseline intentionally hides fillers under the proposed buffer.
        // A concrete candidate must name those deletions explicitly.
        const OverlapInfo& overlap = checkOverlap(request);
        if (overlap.diags) {
            result.diagnostics.push_back(*overlap.diags);
            result.isLegal = false;
            return result;
        }
        if (!useNewFillers) {
            excludedNodes.insert(overlap.fillers.begin(),
                                 overlap.fillers.end());
        }
    }
    for (const CellChangeRecord& change : fillerChanges) {
        const LeafCellID* cellId = cellChangeLeafCellId(change);
        if (cellId != nullptr) {
            excludedNodes.insert(network_->getNodeId(*cellId));
        }
    }

    const CheckShapes& snapshot = getOverlaySnapshot(
        request, guardRegion, fillerChanges, useNewFillers, excludedNodes);
    for (const CheckShape& shape : snapshot) {
        InstanceId id = shape.ownerInstanceIds[0];
        // [fillerRepair-layout] Added/replaced fillers are candidate shapes as
        // well. Validate their row-slot polarity before evaluating rules.
        if (shape.isCandidate && !slotPolarityOk(shape)) {
            result.diagnostics.push_back(
                {"row_slot_polarity_mismatch", makeMessage("instance ", id)});
            result.isLegal = false;
            return result;
        }
    }

    std::vector<XInterval> itvs;
    if (request.targetOp != OpType::Delete) {
        XInterval tgtItv;
        tgtItv.xl = request.colId * siteWidth_;
        tgtItv.xh = tgtItv.xl + masterItems_[request.masterId].width;
        itvs.push_back(tgtItv);
    }
    for (const CellChangeRecord& change : fillerChanges) {
        if (change.op_ == OpType::Add) {
            const MasterId masterId
                = network_->getMasterId(change.new_lib_cell_);
            if (masterId >= 0
                && masterId < static_cast<MasterId>(masterItems_.size())) {
                XInterval itv;
                itv.xl = change.x_.getStorage()
                         - grid_->getCore().getXL().getStorage();
                itv.xh = itv.xl + masterItems_[masterId].width;
                itvs.emplace_back(itv);
            }
            continue;
        }
        const LeafCellID* cellId = cellChangeLeafCellId(change);
        Node* filler = cellId != nullptr ? network_->getNode(*cellId) : nullptr;
        if (filler == nullptr) {
            continue;
        }
        XInterval itv;
        itv.xl = grid_->gridX(filler).v * siteWidth_;
        itv.xh = itv.xl + filler->getWidth().v;
        itvs.emplace_back(itv);
    }

    const std::vector<CheckShape> shapes = mergeShapes(snapshot);
    auto run_job = [&](size_t i, std::vector<CheckOutcome>& local) {
        const Rule& rule = *sortedRules_[i];
        std::vector<CheckOutcome> partial = evalRule(rule, shapes, itvs);
        local.insert(local.end(), partial.begin(), partial.end());
    };
    std::vector<CheckOutcome> outcomes;
    if (DrcUtil::getArena()) {
        tbb::enumerable_thread_specific<std::vector<CheckOutcome>> tlsOutcomes;
        DrcUtil::parallelFor(sortedRules_.size(), run_job, tlsOutcomes);
        for (std::vector<CheckOutcome>& v : tlsOutcomes) {
            outcomes.insert(outcomes.end(), v.begin(), v.end());
        }
    } else {
        for (size_t i = 0; i < sortedRules_.size(); ++i) {
            run_job(i, outcomes);
        }
    }

    result.violations = makeViolations(outcomes, shapes);
    result.violations.erase(
        std::remove_if(result.violations.begin(),
                       result.violations.end(),
                       [&guardRegion, this](const Violation& violation) {
                           return !isInGuard(violation.xWindow,
                                             violation.rowIds,
                                             guardRegion);
                       }),
        result.violations.end());
    result.isLegal = result.violations.empty() && result.diagnostics.empty();
    return result;
}
// --------------------------------------------------------------------------------
// print messages
// --------------------------------------------------------------------------------
// print violation data
std::string Violation::toString(Dbu siteWidth) const
{
    auto toSites = [siteWidth](Dbu v) -> Dbu { return v / siteWidth; };

    std::stringstream ss;
    ss << "  " << layerName << " " << dpl2::ipl::toString(ruleSource) << " "
       << dpl2::ipl::toString(relationship);

    if (!instances.empty()) {
        ss << "  merged=[" << toSites(targetInterval.xl) << ','
           << toSites(targetInterval.xh) << ']';
        if (neighborInterval.xl != targetInterval.xl
            || neighborInterval.xh != targetInterval.xh) {
            ss << "  neighbor=[" << toSites(neighborInterval.xl) << ','
               << toSites(neighborInterval.xh) << ']';
        }
    }

    ss << "  measured=" << toSites(measuredValue) << 's'
       << "  required=" << toSites(requiredValue) << 's' << "  x=["
       << toSites(xWindow.xl) << ',' << toSites(xWindow.xh) << ']';

    if (!instances.empty()) {
        ss << "  neighbors=";
        for (size_t i = 0; i < instances.size(); ++i) {
            if (i != 0) {
                ss << ',';
            }
            ss << instances[i];
        }
    }
    return ss.str();
}

// Print a summary of all layers, rules, masters, and placed instances.
void ImplantLayerChecker::printStats(std::ostream& os, bool isShort) const
{
    os << "========================================\n";
    os << " Implant Layer Data Summary\n";
    os << "========================================\n";

    os << "Implant Layers: " << layers_.size() << "\n";
    if (!isShort) {
        for (const Layer& il : layers_) {
            os << "  LayerId=" << il.getId() << " name=\"" << il.getName()
               << "\"" << " family=";
            switch (il.getVt()) {
                case Layer::Vt::S:
                    os << "VTS";
                    break;
                case Layer::Vt::L:
                    os << "VTL";
                    break;
                case Layer::Vt::H:
                    os << "VTH";
                    break;
                case Layer::Vt::UL:
                    os << "VTUL";
                    break;
                default:
                    os << "Unknown";
                    break;
            }
            os << " polarity=" << (il.getPolar() == Layer::Polar::N ? "N" : "P")
               << "\n";
        }
    }

    os << "Rules: " << rules_.size() << " max rule value: " << maxRuleValue_
       << "\n";
    if (!isShort) {
        for (const Rule& rule : rules_) {
            os << "  RuleId=" << rule.getRuleId() << " source=";
            switch (rule.getSource()) {
                case RuleSource::Width:
                    os << "WIDTH";
                    break;
                case RuleSource::Spacing:
                    os << "SPACING";
                    break;
                case RuleSource::Lef58Width:
                    os << "LEF58_WIDTH";
                    break;
                case RuleSource::Lef58Spacing:
                    os << "LEF58_SPACING";
                    break;
                case RuleSource::Count:
                    os << "COUNT";
                    break;
            }
            os << " primaryLayer=" << rule.getPrimaryLayer()
               << " minValue=" << rule.getMinValue();
            if (rule.getSecondaryLayer())
                os << " secondaryLayer=" << *rule.getSecondaryLayer();
            os << "\n";
        }
    }

    os << "Implant Groups: " << layerGroups_.size() << "\n";
    os << "Masters with Implant Shapes: " << masterItems_.size() << "\n";
    if (!isShort) {
        for (const MasterItem& m : masterItems_) {
            if (m.width == 0)
                continue;
            os << "  MasterId=" << m.masterId << " width=" << m.width
               << " height=" << m.height << " siteHeight=" << m.siteHeight
               << " rawShapes=" << m.rawShapes.size()
               << " rebuiltShapes=" << m.shapes.size() << " : <";
            for (const MasterShape& shape : m.shapes) {
                os << "(" << shape.shapeId << ",L" << shape.layer << ")"
                   << shape.rect.toString() << " ";
            }
            os << ">\n";
            if (!m.rawShapes.empty()) {
                os << "    raw: ";
                for (const MasterShape& rs : m.rawShapes) {
                    os << "(L" << rs.layer << ")" << rs.rect.toString() << " ";
                }
                os << "\n";
            }
        }
    }

    int fillerNum = 0;
    size_t placedCount = 0;
    placedCount = network_->getNodes().size();
    for (const std::unique_ptr<Node>& nodePtr : network_->getNodes()) {
        const Node* node = nodePtr.get();
        if (node && node->isFiller())
            fillerNum++;
    }
    os << "Placed Instances: " << placedCount << " filler: " << fillerNum
       << "\n";
    if (!isShort) {
        for (const std::unique_ptr<Node>& nodePtr : network_->getNodes()) {
            const Node* node = nodePtr.get();
            if (!node)
                continue;
            const RowId rowId = grid_ ? grid_->gridSnapDownY(node).v : 0;
            const ColId colId = grid_ ? grid_->gridX(node).v : 0;
            os << "  InstanceId=" << node->getId()
               << " masterId=" << node->getMaster()->getId() << " coord=<"
               << rowId << ", " << colId << ">" << " orient=";
            const PhysOrientation orient = node->getOrient();
            if (orient == PhysOrientationE::R0) {
                os << "R0";
            } else if (orient == PhysOrientationE::MX) {
                os << "MX";
            } else if (orient == PhysOrientationE::MY) {
                os << "MY";
            } else if (orient == PhysOrientationE::R180) {
                os << "R180";
            } else {
                os << "?";
            }
            os << " filler: " << node->isFiller();
            os << "\n";
        }
    }
    os << "Rows: " << grid_->getRowCount() << "\n";
    os << "Cols: " << grid_->getRowSiteCount() << "\n";
    os << "Row Height: " << rowHeight_ << "\n";
    os << "Site Width: " << siteWidth_ << "\n";
    os << "========================================\n";
}

}  // namespace ipl
}  // namespace dpl2
