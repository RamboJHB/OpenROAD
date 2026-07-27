#include "drc/ImplantLayerChecker.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <tbb/enumerable_thread_specific.h>
#include "util/performance.hh"

#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"
#include <dpl2/network.h>
#include "util.h"
#include <physlib/techRuleCheck.hh>

namespace dpl2 {
namespace ipl {

constexpr Dbu ZERO = 0;
constexpr Dbu ADJACENT_ROW_VERTICAL_SPACING = 1;

inline Layer::Polar opposite(Layer::Polar p)
{
    return (p == Layer::Polar::N) ? Layer::Polar::P : Layer::Polar::N;
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
    return static_cast<uint64_t>(value) ^
           (static_cast<uint64_t>(value) >> 32);
}

Dbu spacing(const XInterval& left, const XInterval& right)
{
    return std::max<Dbu>(ZERO, std::max(left.xl, right.xl) -
                                    std::min(left.xh, right.xh));
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

XInterval xOf(const CheckerRect& rect)
{
    return {rect.xl, rect.xh};
}

CheckerRect uniteRect(const CheckerRect& left, const CheckerRect& right)
{
    return {std::min(left.xl, right.xl),
            std::min(left.yl, right.yl),
            std::max(left.xh, right.xh),
            std::max(left.yh, right.yh)};
}

template <typename Value>
void appendUnique(std::vector<Value>& values, Value value)
{
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

bool isWidthRule(RuleSource source)
{
    return source == RuleSource::Width || source == RuleSource::Lef58Width;
}

bool isSpacingRule(RuleSource source)
{
    return source == RuleSource::Spacing || source == RuleSource::Lef58Spacing;
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

// -----------------------------------------------------------------------------
// Polarity alternates per row: if row 0 bottom = basePolar,
//   - Even rows: Bottom=basePolar, Top=!basePolar
//   - Odd rows:  Bottom=!basePolar, Top=basePolar
// -----------------------------------------------------------------------------
Layer::Polar ImplantLayerChecker::slotPolar(RowId rowId, BandSlot bandSlot) const
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

bool ImplantLayerChecker::BucketKey::operator<(const BucketKey& other) const
{
    if (rowId != other.rowId) {
        return rowId < other.rowId;
    }
    if (bandSlot != other.bandSlot) {
        return bandSlot < other.bandSlot;
    }
    return layer < other.layer;
}

bool ImplantLayerChecker::GroupKey::operator<(
    const GroupKey& other) const
{
    if (rowId != other.rowId) {
        return rowId < other.rowId;
    }
    if (bandSlot != other.bandSlot) {
        return bandSlot < other.bandSlot;
    }
    return groupId < other.groupId;
}

ImplantLayerChecker::ImplantLayerChecker(Grid* grid, Network* network)
    : DRCChecker(grid), network_(network)
{
    if (grid_ && network_) {
        eUNL::Session& sess = eUNL::Session::getSession();
        eUNL::Design* design = sess.getCurrentDesign();
        if (design) {
            init(design->getPhysDesMgr());
        }
    }
}

ImplantLayerChecker::~ImplantLayerChecker()
{
}

// -----------------------------------------------------------------------------
// initialize
// -----------------------------------------------------------------------------
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
    ok = buildRules(layers_, groups_, rules_) && ok;
    ok = buildMstIntervals() && ok;
    ok = buildInsts() && ok;

    if (!ok) {
        for (Diagnostic& diag : diagnostics_) {
            std::cout << diag.status << " " << diag.message << std::endl;
        }
    }
    printStats(std::cout, true/*isShort*/);
    return ok;
}

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
        rules_.emplace_back(Rule(rules_.size(), RuleSource::Width, il.getId(),
            techLayer.getWidth().getStorage()));
        // Spacing
        rules_.emplace_back(Rule(rules_.size(), RuleSource::Spacing, il.getId(),
            techLayer.getMinSpacing().getStorage()));

        for (const eLIB::TechRule& techRule : techLayer.getRuleIter()) {
            if (techRule.getCheck()._type == eLIB::RuleCheckType::WIDTH_RULE) {
                // "WIDTH minWidth [LAYER layerName2] [ZEROPRL] [EXCEPTCORNERTOUCH]
                //      [LENGTH length] [CHECKIMPLANTGROUP groupName]
                //      ]; "
                const eLIB::WidthRule* widthRule = dynamic_cast<
                    const eLIB::WidthRule*>(&techRule.getCheck());
                if (widthRule) {
                    rules_.push_back(Rule(rules_.size(), RuleSource::Lef58Width,
                        il.getId(), widthRule->getWidth().getStorage()));
                    Rule& rule = rules_.back();

                    // Set optional clauses
                    const std::string& layerName =
                      widthRule->getOtherImplLayerName();
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
                    const std::string& checkGroup =
                      widthRule->getCheckImplantGroup();
                    if (!checkGroup.empty()) {
                        rule.setCheckGroup(checkGroup);
                        uvAssert(groups_.contains(checkGroup));
                    }
                }
            } else if (techRule.getCheck()._type ==
                       eLIB::RuleCheckType::SPACING_IMPLANT_RULE) {
                // "SPACING minSpacing [LAYER layerName2]
                //      [HORIZONTAL|VERTICAL PRL prl ] [EXCEPTABUTTED]
                //      [EXCEPTCORNERTOUCH] [LENGTH length]
                //      [INTERSECTLAYERS layerNameList...]
                //      ; "
                const eLIB::SpacingImplantRule* spacingRule =
                  dynamic_cast<const eLIB::SpacingImplantRule*>
                  (&techRule.getCheck());
                if (spacingRule) {
                    const std::vector<eLIB::SpacingImplantRule::SIItem>&
                      spacingTable = spacingRule->getSpacingImplantTable();
                    for (const eLIB::SpacingImplantRule::SIItem& item
                         : spacingTable) {
                        rules_.push_back(Rule(rules_.size(),
                            RuleSource::Lef58Spacing, il.getId(),
                            item.minSpacing.getStorage()));
                        Rule& rule = rules_.back();

                        if (!item.layerName2.empty()) {
                            LayerId id = getLayerId(item.layerName2);
                            uvAssert(id != -1);
                            rule.setSecondaryLayer(id);
                        }

                        if (item.prlOrient ==
                            eLIB::RuleCheckOrthoTypeE::HORIZONTAL) {
                            rule.setDirection(RuleDirection::Horizontal);
                        } else if (item.prlOrient ==
                                   eLIB::RuleCheckOrthoTypeE::VERTICAL) {
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
                        for (const std::string& layerName : item.layerNameList) {
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

bool ImplantLayerChecker::buildInsts()
{
    bool ok = true;
    for (const std::unique_ptr<Node>& nodePtr : network_->getNodes()) {
        Node* node = nodePtr.get();
        const MasterId mid = node ? node->getMaster()->getId() : -1;
        const std::string nodeName = "instance " + std::to_string(node->getId());
        if (!node || mid < 0 || mid >=
            static_cast<MasterId>(masterItems_.size())
            || masterItems_[mid].width == 0) {
            diagnostics_.push_back({"skipped_invalid_node_master", nodeName});
            continue;
        }

        eUTL::PhysOrientation orient = node->getOrient();
        if (orient != eUTL::PhysOrientationE::R0 && orient !=
            eUTL::PhysOrientationE::MX && orient !=
            eUTL::PhysOrientationE::MY && orient !=
            eUTL::PhysOrientationE::R180) {
            diagnostics_.push_back({"skipped_unsupported_orientation", nodeName});
            continue;
        }
    }
    return ok;
}

// Validate and index implant layers, groups, and normalized rules. Rule ordering
// is prepared here so containment handling can reason about specificity.
bool ImplantLayerChecker::buildRules(
    const std::vector<Layer>& layers,
    const LayerGroupMap& groups,
    const std::vector<Rule>& rules)
{
    bool ok = true;
    for (const Layer& layer : layers) {
        ruleIndex_.layers[layer.getId()] = layer;
    }
    ruleIndex_.groups = groups;

    for (Rule rule : rules) {
        // Keep unsupported rules visible to callers, but do not let them
        // participate in violation generation.
        if (ruleIndex_.layers.find(rule.getPrimaryLayer()) ==
            ruleIndex_.layers.end()) {
            diagnostics_.push_back({"skipped_missing_rule_parameter",
                makeMessage("unknown primary layer for rule ", rule.getRuleId())});
            ok = false;
            continue;
        }
        if (rule.getSecondaryLayer() &&
            ruleIndex_.layers.find(*rule.getSecondaryLayer()) ==
            ruleIndex_.layers.end()) {
            diagnostics_.push_back({"skipped_missing_rule_parameter",
                makeMessage("unknown secondary layer for rule ",
                    rule.getRuleId())});
            ok = false;
            continue;
        }
        if (!rule.getUnsupportedClauses().empty()) {
            diagnostics_.push_back({"skipped_unsupported_rule_clause",
                makeMessage("unsupported LEF58 clause in rule ",
                    rule.getRuleId())});
        }
        if (rule.getCheckGroup()) {
            const auto groupIt = ruleIndex_.groups.find(*rule.getCheckGroup());
            if (groupIt == ruleIndex_.groups.end()) {
                diagnostics_.push_back({"skipped_missing_rule_parameter",
                    "unknown implant group " + *rule.getCheckGroup()});
            } else if (groupIds_.find(*rule.getCheckGroup()) ==
                       groupIds_.end()) {
                const GroupId groupId =
                    static_cast<GroupId>(groupIds_.size() + 1);
                groupIds_[*rule.getCheckGroup()] = groupId;
                groupLayers_[groupId] = groupIt->second;
                for (LayerId layer : groupIt->second) {
                    layerGroups_[layer].push_back(groupId);
                }
            }
        }
        ruleIndex_.ruleById[rule.getRuleId()] = rule;
    }
    ruleIndex_.rules.clear();
    for (const auto& [ruleId, rule] : ruleIndex_.ruleById) {
        ruleIndex_.rules.push_back(rule);
    }
    // Specific rules are evaluated first so containment suppression can later
    // recognize "specific satisfied, broad violated" as legal.
    std::sort(ruleIndex_.rules.begin(),
        ruleIndex_.rules.end(),
        [](const Rule& left, const Rule& right) {
            if (left.getSpecificityRank() != right.getSpecificityRank()) {
                return left.getSpecificityRank() < right.getSpecificityRank();
            }
            return left.getRuleId() < right.getRuleId();
        });
    return ok;
}

// Convert every master rectangle into row-band x intervals.
bool ImplantLayerChecker::buildMstIntervals()
{
    bool ok = true;
    const Dbu halfRow = rowHeight_ / 2;
    // Macro-internal pieces can be skipped for plain width/spacing after
    // row-band restructuring, but LEF58 group/intersect clauses may still need
    // them as context.
    const bool keepInternal =
        std::any_of(ruleIndex_.rules.begin(), ruleIndex_.rules.end(),
            [](const Rule& rule) { return rule.getCheckGroup().has_value()
            || !rule.getIntersectLayers().empty(); });

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

            const auto layerIt = ruleIndex_.layers.find(shape.layer);
            if (layerIt == ruleIndex_.layers.end()) {
                diagnostics_.push_back({"skipped_missing_rule_parameter",
                    makeMessage("unknown implant layer in shape ", shape.shapeId)});
                ok = false;
                continue;
            }
            if (shapeXl >= shapeXh || shapeYl >= shapeYh) {
                diagnostics_.push_back({"skipped_unsupported_geometry",
                    makeMessage("invalid rectangle shape ", shape.shapeId)});
                ok = false;
                continue;
            }
            if (shapeXl != 0 || shapeXh != width) {
                diagnostics_.push_back({"implant_shape_width_mismatch",
                    makeMessage("implant shape does not span master width ",
                        shape.shapeId)});
                ok = false;
            }
            if (shapeXl < 0 || shapeXh > width ||
                shapeYl < 0 || shapeYh > item.height) {
                diagnostics_.push_back({"shape_outside_macro_boundary",
                    makeMessage("shape outside macro ", shape.shapeId)});
                ok = false;
                continue;
            }
            for (size_t j = i + 1; j < item.shapes.size(); ++j) {
                const MasterShape& other = item.shapes[j];
                if (shape.layer != other.layer) {
                    continue;
                }
                const bool xOverlap = std::max(shape.rect._xl, other.rect._xl) <
                                      std::min(shape.rect._xh, other.rect._xh);
                const bool yOverlap = std::max(shape.rect._yl, other.rect._yl) <
                                      std::min(shape.rect._yh, other.rect._yh);
                if (xOverlap && yOverlap) {
                    diagnostics_.push_back({"shape_overlap_in_input",
                        makeMessage("overlap at shape ", shape.shapeId)});
                    ok = false;
                }
            }

            const bool macroInternal = shapeXl > 0 &&
                                       shapeXh < width &&
                                       shapeYl > 0 &&
                                       shapeYh < item.height;

            // Restructure rectangles along the orthogonal axis into canonical
            // half-row bands. After this step the checker only stores x
            // intervals; y/height are represented by (rowOffset, bandSlot).
            Dbu y = shapeYl;
            while (y < shapeYh) {
                const int rowOffset = static_cast<int>(y / rowHeight_);
                const Dbu rowBase = static_cast<Dbu>(rowOffset) * rowHeight_;
                const Dbu bottomEnd = rowBase + halfRow;
                const BandSlot slot = y < bottomEnd ? BandSlot::Bottom
                                                    : BandSlot::Top;
                const Dbu slotEnd = slot == BandSlot::Bottom
                                    ? bottomEnd
                                    : rowBase + rowHeight_;
                const Dbu pieceEnd = std::min(shapeYh, slotEnd);
                if (pieceEnd <= y) {
                    break;
                }
                const bool fullSlot = y == (slot == BandSlot::Bottom ? rowBase
                                        : bottomEnd) && pieceEnd == slotEnd;
                if (!fullSlot) {
                    // The current stage assumes regular implant tracks. Partial
                    // bands are still normalized, but are reported because they
                    // violate that modeling assumption.
                    diagnostics_.push_back({"unsupported_row_band_slot",
                        makeMessage("partial band slot shape ", shape.shapeId)});
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

// Build masterItems_ (implant shapes) from Network
void ImplantLayerChecker::buildMasters()
{
    int masterCount = network_->getMasters().size();
    masterItems_.resize(masterCount);

    for (const std::unique_ptr<Master>& masterPtr : network_->getMasters()) {
        const Master* nm = masterPtr.get();
        if (!nm) {
            continue;
        }
        MasterId mid = nm->getId();
        uvAssert(mid >= 0 && mid < masterCount);
        const eLIB::PhysLibCell* physCell = nm->getPhysLibCell();
        uvAssert(physCell);

        MasterItem& item = masterItems_[mid];
        item.width = physCell->getWidth().getStorage();
        item.height = physCell->getHeight().getStorage();
        item.siteHeight = physCell->getTechSite()->getHeight().getStorage();
        item.masterId = mid;
        item.isFiller = physCell->getType().isCoreFiller();

        // Check if this master has implant shapes
        bool hasImplant = false;
        const std::vector<eLIB::PhysLibObs>& obsVec = physCell->getObstruction();
        for (const eLIB::PhysLibObs& obs : obsVec) {
            const eLIB::LayerShapeMapT& shapes = obs.getShapes(
                eUTL::PhysOrientationE::R0);
            for (const auto& [layerRelId, shapeVec] : shapes) {
                if (getLayerId(layerRelId) >= 0) {
                    hasImplant = true;
                    break;
                }
            }
            if (hasImplant) break;
        }

        if (!hasImplant) {
            continue;
        }

        ShapeId shapeId = 0;
        for (const eLIB::PhysLibObs& obs : obsVec) {
            const eLIB::LayerShapeMapT& shapes = obs.getShapes(
                eUTL::PhysOrientationE::R0);
            for (const auto& [layerRelId, shapeVec] : shapes) {
                LayerId lid = getLayerId(layerRelId);
                if (lid < 0) {
                    continue; // non-implant
                }
                for (const eLIB::TechShape& techShape : shapeVec) {
                    if (techShape.getType() != eLIB::TechShape::RECT) {
                        diagnostics_.push_back({"skipped_unsupported_geometry",
                            "skipped_unsupported_geometry:master non-RECT shape"});
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

// -----------------------------------------------------------------------------
// Rebuild master shapes into canonical band-level shapes.
// For each row the master spans, produces 2 shapes (bottom band, top band),
// each spanning the full master width with height = rowHeight / 2.
// -----------------------------------------------------------------------------
void ImplantLayerChecker::rebuildMasterShapes()
{
    for (MasterItem& item : masterItems_) {
        if (item.rawShapes.empty()) {
            item.shapes.clear();
            continue;
        }

        if (item.siteHeight <= 0) {
            diagnostics_.push_back({"skipped_rebuild_no_site_height",
                "skipped_rebuild_no_site_height: master " +
                std::to_string(item.masterId)});
            item.shapes = item.rawShapes;
            continue;
        }

        const Dbu fullRow = rowHeight_;
        const Dbu halfRow = fullRow / 2;

        // Determine the number of rows this master spans
        int numRows = static_cast<int>((item.height + fullRow - 1) / fullRow);
        if (numRows < 1) numRows = 1;

        // Determine the family from raw shapes (all should be the same family)
        Layer::Vt family = Layer::Vt::Unknown;
        for (const MasterShape& rs : item.rawShapes) {
            auto layerIt = std::find_if(layers_.begin(), layers_.end(),
                [&](const Layer& l) { return l.getId() == rs.layer; });
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
                auto layerIt = std::find_if(layers_.begin(), layers_.end(),
                    [&](const Layer& l) { return l.getId() == rs.layer; });
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
                if (l.getPolar() == Layer::Polar::N) familyNLayer = l.getId();
                else familyPLayer = l.getId();
            }
        }
        if (familyNLayer < 0 || familyPLayer < 0) {
            diagnostics_.push_back({"skipped_rebuild_missing_layer",
                "skipped_rebuild_missing_layer: master " +
                std::to_string(item.masterId) +
                " family missing N or P layer"});
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
                topBandPol = (bottomPolarity == Layer::Polar::N) ? Layer::Polar::P
                    : Layer::Polar::N;
            } else {
                bottomBandPol = (bottomPolarity == Layer::Polar::N) ?
                    Layer::Polar::P : Layer::Polar::N;
                topBandPol = bottomPolarity;
            }

            // Bottom band shape
            {
                MasterShape ms;
                ms.shapeId = shapeId++;
                ms.layer = (bottomBandPol == Layer::Polar::N) ?
                    familyNLayer : familyPLayer;
                Dbu yBase = static_cast<Dbu>(row) * fullRow;
                ms.rect = eUTL::Rect(
                    eUTL::UvDist(static_cast<int64_t>(0)),
                    eUTL::UvDist(yBase),
                    eUTL::UvDist(item.width),
                    eUTL::UvDist(yBase + halfRow));
                item.shapes.push_back(ms);
            }

            // Top band shape
            {
                MasterShape ms;
                ms.shapeId = shapeId++;
                ms.layer = (topBandPol == Layer::Polar::N) ?
                    familyNLayer : familyPLayer;
                Dbu yBase = static_cast<Dbu>(row) * fullRow + halfRow;
                ms.rect = eUTL::Rect(
                    eUTL::UvDist(static_cast<int64_t>(0)),
                    eUTL::UvDist(yBase),
                    eUTL::UvDist(item.width),
                    eUTL::UvDist(yBase + halfRow));
                item.shapes.push_back(ms);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Checker Entry Interface
// -----------------------------------------------------------------------------
bool ImplantLayerChecker::check(const Node* node, GridX x,
    GridY y, const eUTL::PhysOrientation& orient) const
{
    std::vector<FillerCellRecord> fcRecord;
    return check(node, x, y, orient, fcRecord);
}

bool ImplantLayerChecker::check(const Node* node, GridX x,
    GridY y, const eUTL::PhysOrientation& orient,
    std::vector<FillerCellRecord>& fcRecord) const
{
    if (!node || !network_ || !grid_) {
        return false;
    }
    CheckRequest request;
    request.instanceId = node->getId();
    request.masterId = node->getMaster()->getId();
    request.rowId = grid_->gridSnapDownY(node).v;
    request.colId = x.v;
    request.orientation = orient;

    bool isLegal = checkDirect(request).isLegal;
    if (!isLegal) {
        //!!! todo: call filler repairer here
        // isLegal = repair.check(request, fcRecord)
    }
    return isLegal;
}

CheckResult ImplantLayerChecker::checkDirect(const CheckRequest& request) const
{
    CheckResult result;
    if (siteWidth_ <= 0 || request.colId < 0) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back({"placement_not_site_aligned",
            makeMessage("instance ", request.instanceId)});
        return result;
    }
    Node* node = network_->getNode(request.instanceId);
    const OverlapInfo& overlap = checkOverlap(node);
    if (overlap.diags) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(*overlap.diags);
        result.isLegal = false;
        return result;
    }
    std::set<InstanceId> excludedInstances = overlap.fillers;
    excludedInstances.insert(request.instanceId);

    const std::vector<ScanRect> snapshot = scanSnapshot(request, excludedInstances);
    for (const ScanRect& rect : snapshot) {
        if (rect.isCandidate && !scanSlotPolarityOk(rect)) {
            result.diagnostics = diagnostics_;
            result.diagnostics.push_back({"row_slot_polarity_mismatch",
                makeMessage("instance ", rect.instanceId)});
            result.isLegal = false;
            return result;
        }
    }

    const std::vector<CheckShape> shapes = scanShapes(snapshot);
    auto run_job = [&](size_t i, std::vector<CheckOutcome>& local) {
        const Rule& rule = ruleIndex_.rules[i];
        std::vector<CheckOutcome> partial = scanRule(rule, shapes);
        local.insert(local.end(), partial.begin(), partial.end());
    };
    std::vector<CheckOutcome> outcomes;
    if (DrcUtil::getArena()) {
        tbb::enumerable_thread_specific<std::vector<CheckOutcome>> tlsOutcomes;
        DrcUtil::parallelFor(ruleIndex_.rules.size(), run_job, tlsOutcomes);
        for (std::vector<CheckOutcome>& v : tlsOutcomes) {
            outcomes.insert(outcomes.end(), v.begin(), v.end());
        }
    } else {
        for (size_t i = 0; i < ruleIndex_.rules.size(); ++i) {
            run_job(i, outcomes);
        }
    }

    result.violations = scanViolations(outcomes, shapes);
    result.isLegal = result.violations.empty();
    result.diagnostics = diagnostics_;
    return result;
}

// check all nodes's violations
std::vector<CheckResult> ImplantLayerChecker::checkAllNodesDirect() const
{
    const std::vector<std::unique_ptr<Node>>& nodes = getNodes();
    std::vector<CheckResult> results(nodes.size());
    auto run_job = [&](size_t i) {
        const Node* node = nodes[i].get();
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

auto ImplantLayerChecker::scanSnapshot(const CheckRequest& request)
    const -> std::vector<ScanRect>
{
    return scanSnapshot(request, {request.instanceId});
}

auto ImplantLayerChecker::scanSnapshot(const CheckRequest& request,
    const std::set<InstanceId>& excludedInstances) const -> std::vector<ScanRect>
{
    std::vector<ScanRect> snapshot;
    if (network_) {
        for (const std::unique_ptr<Node>& nodePtr : network_->getNodes()) {
            const Node* node = nodePtr.get();
            const InstanceId instanceId = node->getId();
            if (excludedInstances.find(instanceId) != excludedInstances.end()) {
                continue;
            }
            const RowId rowId = grid_ ? grid_->gridSnapDownY(node).v : 0;
            const ColId colId = grid_ ? grid_->gridX(node).v : 0;
            std::vector<ScanRect> rects = scanInst(instanceId,
                node->getMaster()->getId(), rowId, colId,
                node->getOrient(), false);
            snapshot.insert(snapshot.end(), rects.begin(), rects.end());
        }
    }

    std::vector<ScanRect> candidateRects = scanInst(request.instanceId,
        request.masterId, request.rowId, request.colId,
        request.orientation, true);
    snapshot.insert(snapshot.end(),
        candidateRects.begin(),
        candidateRects.end());
    return snapshot;
}

auto ImplantLayerChecker::scanOverlaySnapshot(const CheckRequest& request,
    const Rect& guardRegion, const FillerChanges& fillerChanges,
    bool useNewFillers,
    const std::set<InstanceId>& excludedInstances) const -> std::vector<ScanRect>
{
    std::vector<ScanRect> snapshot;
    if (network_) {
        for (const std::unique_ptr<Node>& nodePtr : network_->getNodes()) {
            const Node* node = nodePtr.get();
            if (!node) {
                continue;
            }
            const InstanceId instanceId = node->getId();
            if (excludedInstances.find(instanceId) != excludedInstances.end()) {
                continue;
            }
            const RowId rowId = grid_ ? grid_->gridSnapDownY(node).v : 0;
            const ColId colId = grid_ ? grid_->gridX(node).v : 0;
            std::vector<ScanRect> rects = scanInst(instanceId,
                node->getMaster()->getId(),
                rowId, colId, node->getOrient(), false);
            for (ScanRect& rect : rects) {
                if (isInGuard(xOf(rect.rect), {rect.rowId}, guardRegion)) {
                    rect.isCandidate = true;
                }
            }
            snapshot.insert(snapshot.end(), rects.begin(), rects.end());
        }
    }

    std::vector<ScanRect> targetRects = scanInst(request.instanceId,
        request.masterId, request.rowId, request.colId, request.orientation, true);
    snapshot.insert(snapshot.end(), targetRects.begin(), targetRects.end());

    for (const FillerCellRecord& change : fillerChanges) {
        const InstanceId fillerInstId = network_->getNodeId(change.cell_id_);
        const Node* fillerNode = network_->getNode(fillerInstId);
        if (!fillerNode) {
            continue;
        }
        const RowId fillerRowId = grid_ ? grid_->gridSnapDownY(fillerNode).v : 0;
        const ColId fillerColId = grid_ ? grid_->gridX(fillerNode).v : 0;
        MasterId fillerMasterId = fillerNode->getMaster()->getId();
        if (useNewFillers) {
            fillerMasterId =  network_->getMasterId(change.new_lib_cell_);
        }
        std::vector<ScanRect> fillerRects = scanInst(fillerInstId,
            fillerMasterId, fillerRowId, fillerColId,
            fillerNode->getOrient(), true);
        snapshot.insert(snapshot.end(),
            fillerRects.begin(),
            fillerRects.end());
    }
    return snapshot;
}
std::vector<ImplantLayerChecker::ScanRect> ImplantLayerChecker::scanInst(
    InstanceId instanceId, MasterId masterId, RowId rowId,
    ColId colId, PhysOrientation orientation, bool isCandidate) const
{
    std::vector<ScanRect> rects;
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
        ScanRect placed;
        placed.instanceId = instanceId;
        placed.masterId = masterId;
        placed.shapeId = shape.shapeId;
        placed.layer = shape.layer;
        placed.rect = {originX + xl, originY + yl, originX + xh, originY + yh};
        placed.rowId = placed.rect.yl / rowHeight_;
        placed.bandSlot = placed.rect.yl % rowHeight_ > 0 ?
            BandSlot::Top : BandSlot::Bottom;
        placed.isCandidate = isCandidate;
        rects.push_back(placed);
    }
    return rects;
}

CheckShapes ImplantLayerChecker::scanShapes(const ScanRectVec& rects) const
{
    std::vector<ScanRect> sorted = rects;
    std::sort(sorted.begin(),
        sorted.end(),
        [](const ScanRect& left, const ScanRect& right) {
            if (left.rowId != right.rowId) {
                return left.rowId < right.rowId;
            }
            if (left.bandSlot != right.bandSlot) {
                return left.bandSlot < right.bandSlot;
            }
            if (left.layer != right.layer) {
                return left.layer < right.layer;
            }
            if (left.rect.xl != right.rect.xl) {
                return left.rect.xl < right.rect.xl;
            }
            return left.rect.xh < right.rect.xh;
        });

    std::vector<CheckShape> shapes;
    int nextId = 1;
    for (const ScanRect& rect : sorted) {
        const XInterval x = xOf(rect.rect);
        if (shapes.empty() ||
            shapes.back().rowId != rect.rowId ||
            shapes.back().bandSlot != rect.bandSlot ||
            shapes.back().layer != rect.layer ||
            shapes.back().isCandidate != rect.isCandidate ||
            x.xl > shapes.back().x.xh) {
            CheckShape shape;
            shape.id = nextId++;
            shape.layer = rect.layer;
            shape.rowId = rect.rowId;
            shape.bandSlot = rect.bandSlot;
            shape.x = x;
            shape.ownerInstanceIds = {rect.instanceId};
            shape.ownerShapeIds = {rect.shapeId};
            shape.isCandidate = rect.isCandidate;
            shapes.push_back(shape);
            continue;
        }

        CheckShape& active = shapes.back();
        active.x.xh = std::max(active.x.xh, x.xh);
        appendUnique(active.ownerInstanceIds, rect.instanceId);
        appendUnique(active.ownerShapeIds, rect.shapeId);
        active.isCandidate = active.isCandidate || rect.isCandidate;
    }
    return shapes;
}

CheckOutcomeVec ImplantLayerChecker::scanRule(const Rule& rule,
    const CheckShapes& shapes) const
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
            if (!ruleAppliesTo(rule, rlt)) {
                continue;
            }

            CheckShape checkTarget = target;
            if (isWidthRule(rule.getSource()) && rlt == Relationship::InterRow) {
                for (const CheckShape& sameRowNeighbor :
                    scanNeighbors(target, rule, Relationship::IntraRow, shapes)) {
                    if (target.layer == sameRowNeighbor.layer &&
                        touchesOrOverlaps(checkTarget.x, sameRowNeighbor.x)) {
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

            const std::vector<CheckShape> neighbors =
                scanNeighbors(checkTarget, rule, rlt, shapes);
            if (isWidthRule(rule.getSource()) && neighbors.empty() &&
                rlt == Relationship::IntraRow) {
                CheckOutcome outcome;
                outcome.ruleId = rule.getRuleId();
                outcome.ruleSource = rule.getSource();
                outcome.primaryLayer = rule.getPrimaryLayer();
                outcome.secondaryLayer = rule.getSecondaryLayer();
                outcome.relationship = rlt;
                outcome.targetId = checkTarget.id;
                outcome.xWindow = checkTarget.x;
                outcome.targetX = checkTarget.x;
                outcome.measuredValue =
                    checkTarget.x.xh - checkTarget.x.xl;
                outcome.requiredValue = rule.getMinValue();
                outcome.status = outcome.measuredValue < rule.getMinValue()
                    ? OutcomeStatus::Violated
                    : OutcomeStatus::Satisfied;
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
                outcome.xWindow =
                    unite(checkTarget.x, neighbor.x);
                outcome.requiredValue = rule.getMinValue();
                outcome.instanceIds = checkTarget.ownerInstanceIds;
                outcome.rowIds = {checkTarget.rowId};
                for (InstanceId id : neighbor.ownerInstanceIds) {
                    appendUnique(outcome.instanceIds, id);
                }
                appendUnique(outcome.rowIds, neighbor.rowId);

                const Dbu projected =
                    prl(checkTarget.x, neighbor.x);
                if (rule.getExceptAbutted() && projected == 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.getExceptCornerTouch() &&
                    rlt == Relationship::InterRow &&
                    projected == 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.getLength() && rowHeight_ / 2 >= *rule.getLength()) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (!rule.getIntersectLayers().empty() &&
                    !scanIntersectCoverage(rule,
                        checkTarget,
                        neighbor,
                        shapes)) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.getPrl()) {
                    const bool lef58VerticalSpacing =
                        rule.getSource() == RuleSource::Lef58Spacing &&
                        rlt == Relationship::InterRow;
                    const bool lef58HorizontalSpacing =
                        rule.getSource() == RuleSource::Lef58Spacing &&
                        rlt == Relationship::IntraRow;
                    if (!lef58HorizontalSpacing && *rule.getPrl() >= 0 &&
                        projected <= *rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (lef58VerticalSpacing && *rule.getPrl() < 0 &&
                        spacing(checkTarget.x, neighbor.x) >=
                            -*rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (!lef58HorizontalSpacing && !lef58VerticalSpacing &&
                        *rule.getPrl() < 0 &&
                        spacing(target.x, neighbor.x) >
                            -*rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                }

                if (isWidthRule(rule.getSource())) {
                    XInterval effective = checkTarget.x;
                    if (rlt == Relationship::InterRow) {
                        if (!overlaps(checkTarget.x,
                                neighbor.x)) {
                            outcome.status = OutcomeStatus::NotApplicable;
                            outcomes.push_back(outcome);
                            continue;
                        }
                        effective = intersect(checkTarget.x,
                            neighbor.x);
                    } else if (touchesOrOverlaps(checkTarget.x,
                            neighbor.x)) {
                        effective = unite(checkTarget.x,
                            neighbor.x);
                    }
                    outcome.xWindow = effective;
                    if (rule.getCheckGroup() &&
                        !scanGroupFails(rule,
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
                    outcome.status = outcome.measuredValue < rule.getMinValue()
                        ? OutcomeStatus::Violated
                        : OutcomeStatus::Satisfied;
                } else {
                    if (checkTarget.layer == neighbor.layer &&
                        touchesOrOverlaps(checkTarget.x,
                            neighbor.x)) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    outcome.measuredValue =
                        rule.getSource() == RuleSource::Lef58Spacing &&
                            rlt == Relationship::InterRow
                        ? ADJACENT_ROW_VERTICAL_SPACING
                        : spacing(checkTarget.x,
                            neighbor.x);
                    outcome.status = outcome.measuredValue < rule.getMinValue()
                        ? OutcomeStatus::Violated
                        : OutcomeStatus::Satisfied;
                }
                outcomes.push_back(outcome);
            }
        }
    }
    return outcomes;
}

CheckShapes ImplantLayerChecker::scanNeighbors(const CheckShape& target,
    const Rule& rule, Relationship rlt, const CheckShapes& shapes) const
{
    std::vector<CheckShape> neighbors;
    const Dbu radius = queryRadius(rule);
    const XInterval queryWindow{target.x.xl - radius,
        target.x.xh + radius};
    const LayerId queryLayer = rule.getSecondaryLayer().value_or(
        rule.getPrimaryLayer());

    std::vector<SlotRef> slots;
    if (rlt == Relationship::IntraRow) {
        slots.push_back({target.rowId, target.bandSlot});
    } else {
        const SlotRef& nbr = getAdjSlot(target.rowId, target.bandSlot);
        if (nbr.rowId != -1) {
            slots.push_back(nbr);
        }
    }

    const bool returnSameRowRuns = isWidthRule(rule.getSource())
        && rlt == Relationship::InterRow;

    for (const CheckShape& shape : shapes) {
        if (shape.id == target.id) {
            continue;
        }
        if (shape.layer != queryLayer) {
            continue;
        }
        if (shape.x.xh < queryWindow.xl ||
            shape.x.xl > queryWindow.xh) {
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
        if (rlt == Relationship::IntraRow &&
            isWidthRule(rule.getSource()) &&
            !touchesOrOverlaps(target.x, shape.x)) {
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
        if (runs.empty() ||
            runs.back().rowId != shape.rowId ||
            runs.back().bandSlot != shape.bandSlot ||
            runs.back().layer != shape.layer ||
            !touchesOrOverlaps(runs.back().x, shape.x)) {
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
                return !overlaps(target.x,
                    shape.x);
            }),
        runs.end());
    return runs;
}

// Check if one outcome context is contained within another (for suppression).
bool ImplantLayerChecker::contained(const CheckOutcome& specific,
    const CheckOutcome& broad) const
{
    if (specific.ruleSource != broad.ruleSource ||
        specific.primaryLayer != broad.primaryLayer ||
        specific.secondaryLayer != broad.secondaryLayer ||
        specific.relationship != broad.relationship ||
        specific.targetId != broad.targetId) {
        return false;
    }
    if (broad.neighborId &&
        specific.neighborId != broad.neighborId) {
        return false;
    }
    return specific.xWindow.xl >= broad.xWindow.xl &&
        specific.xWindow.xh <= broad.xWindow.xh;
}

ViolationVec ImplantLayerChecker::scanViolations(const CheckOutcomeVec& outcomes,
    const CheckShapes& shapes) const
{
    std::set<size_t> suppressed;
    for (size_t broadIndex = 0; broadIndex < outcomes.size(); ++broadIndex) {
        const CheckOutcome& broad = outcomes[broadIndex];
        if (broad.status != OutcomeStatus::Violated) {
            continue;
        }
        const auto broadRuleIt = ruleIndex_.ruleById.find(broad.ruleId);
        if (broadRuleIt == ruleIndex_.ruleById.end() ||
            !broadRuleIt->second.getContainmentGroup()) {
            continue;
        }
        for (const CheckOutcome& specific : outcomes) {
            if (specific.status != OutcomeStatus::Satisfied) {
                continue;
            }
            const auto specificRuleIt =
                ruleIndex_.ruleById.find(specific.ruleId);
            if (specificRuleIt == ruleIndex_.ruleById.end()) {
                continue;
            }
            const Rule& specificRule = specificRuleIt->second;
            if (specificRule.getContainmentGroup() !=
                broadRuleIt->second.getContainmentGroup()) {
                continue;
            }
            if (std::find(specificRule.getContainedByRuleIds().begin(),
                    specificRule.getContainedByRuleIds().end(),
                    broad.ruleId) ==
                specificRule.getContainedByRuleIds().end()) {
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
        if (outcome.status != OutcomeStatus::Violated ||
            suppressed.find(i) != suppressed.end()) {
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
            shapes.begin(), shapes.end(),
            [&](const CheckShape& s) {
                return s.id == outcome.targetId;
            });
        if (targetIt != shapes.end()) {
            XInterval mergedX = targetIt->x;
            const bool isWidthInterRow =
                outcome.ruleSource == RuleSource::Width &&
                outcome.relationship == Relationship::InterRow;
            if (isWidthInterRow) {
                for (const CheckShape& sameRow : shapes) {
                    if (sameRow.id == outcome.targetId) {
                        continue;
                    }
                    if (sameRow.layer != targetIt->layer) {
                        continue;
                    }
                    if (sameRow.rowId != targetIt->rowId ||
                        sameRow.bandSlot != targetIt->bandSlot) {
                        continue;
                    }
                    if (touchesOrOverlaps(mergedX,
                            sameRow.x)) {
                        mergedX =
                            unite(mergedX, sameRow.x);
                    }
                }
            }
            violation.targetInterval = mergedX;
        } else {
            violation.targetInterval = outcome.targetX;
        }

        auto shapeIt = std::find_if(
            shapes.begin(), shapes.end(),
            [&](const CheckShape& s) {
                return outcome.neighborId.has_value() &&
                    s.id == *outcome.neighborId;
            });
        violation.neighborInterval =
            shapeIt != shapes.end()
                ? shapeIt->x
                : violation.targetInterval;

        // Look up layer name
        auto layerIt = ruleIndex_.layers.find(outcome.primaryLayer);
        if (layerIt != ruleIndex_.layers.end()) {
            violation.layerName = layerIt->second.getName();
        }
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

const std::vector<std::unique_ptr<Node>>& ImplantLayerChecker::getNodes() const
{
    return network_->getNodes();
}

ImplantLayerChecker::OverlapInfo ImplantLayerChecker::checkOverlap(
    const Node* node) const
{
    OverlapInfo info;

    for (GridX x = grid_->gridX(node); x < grid_->gridEndX(node); x++) {
        for (GridY y = grid_->gridSnapDownY(node); y < grid_->gridEndY(node); y++) {
            Pixel* pixel = grid_->gridPixel(x, y);
            Node* node2 = pixel ? pixel->cell : nullptr;
            if (node2 != nullptr && node2 != node) {
                if (node2->isFiller()) {
                    info.fillers.insert(node2->getId());
                } else {
                    info.diags = Diagnostic{"placement_overlap_in_input",
                        "inst " + std::to_string(node->getId()) + ", "
                        + std::to_string(node2->getId())};
                    return info;
                }
            }
        }
    }

    return info;
}

// Return whether an instantiated interval fits the row's expected N/P slot.
bool ImplantLayerChecker::slotPolarityOk(const ImplantLayerChecker::PlacedInterval&
    interval) const
{
    const Layer::Polar expectedPolar = slotPolar(interval.rowId,
        interval.bandSlot);
    const auto actualLayerIt = ruleIndex_.layers.find(interval.layer);
    uvAssert(actualLayerIt != ruleIndex_.layers.end());
    return actualLayerIt->second.getPolar() == expectedPolar;
}

DiagVec ImplantLayerChecker::validateOverlayRequest(const CheckRequest& request,
    const FillerChanges& fillerChanges) const
{
    std::vector<Diagnostic> diagnostics;
    if (siteWidth_ <= 0) {
        diagnostics.push_back({"placement_not_site_aligned",
            makeMessage("instance ", request.instanceId)});
    }
    const bool targetHasData =
        request.masterId >= 0 &&
        request.masterId < static_cast<MasterId>(masterItems_.size()) &&
        masterItems_[request.masterId].width > 0;
    if (!targetHasData) {
        diagnostics.push_back({"unknown_target_master",
            makeMessage("master ", request.masterId)});
    }

    std::set<InstanceId> seen;
    for (const FillerCellRecord& change : fillerChanges) {
        const InstanceId fillerInstId = network_->getNodeId(change.cell_id_);

        if (!seen.insert(fillerInstId).second) {
            diagnostics.push_back({"duplicate_filler_change",
                makeMessage("duplicate filler change ", fillerInstId)});
            continue;
        }
        if (fillerInstId == request.instanceId) {
            diagnostics.push_back({"target_cannot_be_changed_filler",
                makeMessage("target cannot be changed filler ", fillerInstId)});
        }

        const Node* fillerNode = network_->getNode(fillerInstId);
        if (!fillerNode) {
            diagnostics.push_back({"unknown_filler_instance",
                makeMessage("unknown filler instance ", fillerInstId)});
            continue;
        }
        if (!fillerNode->isFiller()) {
            diagnostics.push_back({"changed_instance_not_filler",
                makeMessage("changed instance is not filler ", fillerInstId)});
        }

        const MasterId newMasterId = network_->getMasterId(change.new_lib_cell_);
        const bool newMasterHasData =
            newMasterId >= 0 &&
            newMasterId < static_cast<MasterId>(masterItems_.size()) &&
            masterItems_[newMasterId].width > 0;
        if (!newMasterHasData) {
            diagnostics.push_back({"unknown_filler_master",
                makeMessage("unknown filler master ", newMasterId)});
            continue;
        }
        if (!masterItems_[newMasterId].isFiller) {
            diagnostics.push_back({"replacement_master_not_filler",
                makeMessage("replacement master is not filler ", newMasterId)});
        }

        const MasterId oldMasterId = fillerNode->getMaster()->getId();
        const bool oldMasterHasData =
            oldMasterId >= 0 &&
            oldMasterId < static_cast<MasterId>(masterItems_.size()) &&
            masterItems_[oldMasterId].width > 0;
        if (oldMasterHasData && newMasterHasData &&
            (masterItems_[oldMasterId].width != masterItems_[newMasterId].width ||
             masterItems_[oldMasterId].height != masterItems_[newMasterId].height)) {
            diagnostics.push_back({"replacement_footprint_mismatch",
                makeMessage("replacement footprint mismatch ", fillerInstId)});
        }
    }
    return diagnostics;
}

bool ImplantLayerChecker::touchesInstance(const Violation& violation,
    InstanceId instanceId) const
{
    return std::find(violation.instances.begin(),
        violation.instances.end(),
        instanceId) != violation.instances.end();
}

bool ImplantLayerChecker::containsViolation(const Violation& oldViolation,
    const Violation& newViolation) const
{
    if (oldViolation.hash != newViolation.hash ||
        !contains(oldViolation.xWindow, newViolation.xWindow)) {
        return false;
    }
    for (InstanceId instanceId : newViolation.instances) {
        if (std::find(oldViolation.instances.begin(),
                oldViolation.instances.end(),
                instanceId) == oldViolation.instances.end()) {
            return false;
        }
    }
    return true;
}

bool ImplantLayerChecker::isInGuard(const XInterval& xWindow,
    const RowIdVec& rowIds, const Rect& guard) const
{
    if (!overlaps(xWindow, XInterval{guard.getXL().getStorage(),
        guard.getXH().getStorage()}) &&
        !touchesOrOverlaps(xWindow, XInterval{guard.getXL().getStorage(),
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
                guard.getYH().getStorage()}) ||
            touchesOrOverlaps(XInterval{rowYl, rowYh},
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

bool ImplantLayerChecker::scanSlotPolarityOk(const ScanRect& rect) const
{
    const Layer::Polar expectedPolar = slotPolar(rect.rowId, rect.bandSlot);
    const auto actualLayerIt = ruleIndex_.layers.find(rect.layer);
    uvAssert(actualLayerIt != ruleIndex_.layers.end());
    return actualLayerIt->second.getPolar() == expectedPolar;
}

// Scan path version - linear search in shapes vector.
bool ImplantLayerChecker::scanIntersectCoverage(const Rule& rule,
    const CheckShape& target, const CheckShape& neighbor,
    const CheckShapes& shapes) const
{
    const XInterval gap{std::min(target.x.xh, neighbor.x.xh),
        std::max(target.x.xl, neighbor.x.xl)};
    for (LayerId layer : rule.getIntersectLayers()) {
        bool covered = false;
        for (const CheckShape& shape : shapes) {
            if (shape.rowId == target.rowId &&
                shape.bandSlot == target.bandSlot &&
                shape.layer == layer &&
                shape.x.xl <= gap.xl &&
                shape.x.xh >= gap.xh) {
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

bool ImplantLayerChecker::scanGroupFails(const Rule& rule,
    const CheckShape& target, const CheckShape& neighbor,
    Relationship rlt, const XInterval& xWindow, const CheckShapes& shapes) const
{
    if (!rule.getCheckGroup()) {
        return true;
    }
    const auto layersIt = ruleIndex_.groups.find(*rule.getCheckGroup());
    if (layersIt == ruleIndex_.groups.end()) {
        return false;
    }

    auto inGroup = [&](LayerId layer) {
        return std::find(layersIt->second.begin(),
            layersIt->second.end(),
            layer) != layersIt->second.end();
    };
    auto groupShapesFor = [&](RowId rowId, BandSlot bandSlot) {
        std::vector<PlacedInterval> intervals;
        int nextId = 1;
        for (const CheckShape& shape : shapes) {
            if (shape.rowId != rowId ||
                shape.bandSlot != bandSlot ||
                !inGroup(shape.layer)) {
                continue;
            }
            PlacedInterval interval;
            interval.intervalId = nextId++;
            interval.instanceId = shape.ownerInstanceIds.empty()
                ? 0
                : shape.ownerInstanceIds.front();
            interval.shapeId = shape.ownerShapeIds.empty()
                ? 0 : shape.ownerShapeIds.front();
            interval.layer = shape.layer;
            interval.rowId = shape.rowId;
            interval.bandSlot = shape.bandSlot;
            interval.x = shape.x;
            intervals.push_back(interval);
        }
        return mergeGroupShapes(intervals, false);
    };

    Dbu maxWidth = 0;
    const std::vector<CheckShape> targetShapes =
        groupShapesFor(target.rowId, target.bandSlot);
    if (rlt == Relationship::InterRow) {
        const std::vector<CheckShape> neighborShapes =
            groupShapesFor(neighbor.rowId, neighbor.bandSlot);
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

CheckShapes ImplantLayerChecker::mergeGroupShapes(const PlacedIntervals& intervals,
    bool isCandidate) const
{
    std::vector<PlacedInterval> sorted = intervals;
    std::sort(sorted.begin(),
        sorted.end(),
        [](const PlacedInterval& left,
            const PlacedInterval& right) {
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
    for (const PlacedInterval& interval : sorted) {
        if (merged.empty() ||
            merged.back().rowId != interval.rowId ||
            merged.back().bandSlot != interval.bandSlot ||
            interval.x.xl > merged.back().x.xh) {
            CheckShape shape;
            shape.id = isCandidate ? nextCandShapeId_-- : 0;
            shape.rowId = interval.rowId;
            shape.bandSlot = interval.bandSlot;
            shape.layer = interval.layer;
            shape.x = interval.x;
            shape.ownerInstanceIds.push_back(interval.instanceId);
            shape.ownerShapeIds.push_back(interval.shapeId);
            shape.isCandidate = isCandidate;
            merged.push_back(shape);
            continue;
        }
        CheckShape& active = merged.back();
        active.x.xh = std::max(active.x.xh, interval.x.xh);
        appendUnique(active.ownerInstanceIds, interval.instanceId);
        appendUnique(active.ownerShapeIds, interval.shapeId);
    }
    return merged;
}

// Map rule direction and ZEROPRL semantics to intra-row or inter-row checks.
bool ImplantLayerChecker::ruleAppliesTo(const Rule& rule, Relationship rlt) const
{
    // LEF58 spacing directions describe the spacing direction: horizontal is
    // same-row x spacing, and vertical is adjacent-row spacing gated by x PRL.
    if (rule.getSource() == RuleSource::Lef58Spacing) {
        if (rule.getDirection() == RuleDirection::Vertical) {
            return rlt == Relationship::InterRow;
        }
        return rlt == Relationship::IntraRow;
    }
    if (rule.getDirection() == RuleDirection::Horizontal) {
        return rlt == Relationship::IntraRow ||
            (isSpacingRule(rule.getSource()) &&
            rlt == Relationship::InterRow);
    }
    if (rule.getDirection() == RuleDirection::Vertical || rule.getZeroPrl()) {
        return rlt == Relationship::InterRow;
    }
    return rlt == Relationship::IntraRow ||
        rlt == Relationship::InterRow;
}

// Compute the x search radius needed to find all possible neighbors for a rule.
Dbu ImplantLayerChecker::queryRadius(const Rule& rule) const
{
    Dbu radius = rule.getMinValue();
    if (rule.getPrl()) {
        radius = std::max(radius, static_cast<Dbu>(std::llabs(*rule.getPrl())));
    }
    if (rule.getLength()) {
        radius = std::max(radius, *rule.getLength());
    }
    return radius;
}

// -----------------------------------------------------------------------------
// Parse layer name to extract family and polarity
// Expected format: "<FAMILY>_<POLARITY>" e.g. "VTUL_N", "VTL_P", "VTH_N"
// -----------------------------------------------------------------------------
void ImplantLayerChecker::parseLayerName(const std::string& name,
    Layer::Vt& family, Layer::Polar& polarity)
{
    family = Layer::Vt::Unknown;
    polarity = Layer::Polar::N;
    auto pos = name.rfind('_');
    if (pos == std::string::npos) {
        return;
    }
    std::string famStr = name.substr(0, pos);
    std::string polStr = name.substr(pos + 1);
    polarity = ((polStr == "P" || polStr == "p") ?
        Layer::Polar::P : Layer::Polar::N);

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

// -----------------------------------------------------------------------------
// filler support: allow filler change
// -----------------------------------------------------------------------------
std::vector<CheckResult> ImplantLayerChecker::checkPlaceWithOverlays(
    const CheckRequest& request, const Rect& guardRegion,
    const std::vector<FillerChanges>& fillerChanges) const
{
    unsigned size = fillerChanges.size();
    std::vector<CheckResult> results(size);
    const std::vector<Violation> oldViolations =
      checkOverlayRegion(request, guardRegion, {}, false).violations;
    auto run_job = [&](int i) {
        results[i] = checkPlaceWithOverlay(request,
            guardRegion, fillerChanges[i], oldViolations);
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

CheckResult ImplantLayerChecker::checkPlaceWithOverlay(
    const CheckRequest& request, const Rect& guardRegion,
    const FillerChanges& fillerChanges, const ViolationVec& oldViolations) const
{
    CheckResult result;
    result.diagnostics = diagnostics_;
    const std::vector<Diagnostic> requestDiagnostics =
        validateOverlayRequest(request, fillerChanges);
    result.diagnostics.insert(result.diagnostics.end(),
        requestDiagnostics.begin(),
        requestDiagnostics.end());
    if (!requestDiagnostics.empty()) {
        result.isLegal = false;
        return result;
    }

    const CheckResult overlay =
        checkOverlayRegion(request, guardRegion, fillerChanges, true);
    result.diagnostics.insert(result.diagnostics.end(),
        overlay.diagnostics.begin(),
        overlay.diagnostics.end());

    std::vector<Violation> blocking;
    for (const Violation& violation : overlay.violations) {
        if (touchesInstance(violation, request.instanceId)) {
            blocking.push_back(violation);
            continue;
        }
        const bool isOld =
            std::any_of(oldViolations.begin(),
                oldViolations.end(),
                [&violation, this](const Violation& oldViolation) {
                    return containsViolation(oldViolation, violation);
                });
        if (!isOld) {
            blocking.push_back(violation);
        }
    }

    result.violations = std::move(blocking);
    result.isLegal = result.violations.empty() &&
        requestDiagnostics.empty() && overlay.diagnostics.empty();
    return result;
}

CheckResult ImplantLayerChecker::checkOverlayRegion(
    const CheckRequest& request, const Rect& guardRegion,
    const FillerChanges& fillerChanges, bool useNewFillers) const
{
    CheckResult result;
    result.diagnostics = diagnostics_;
    if (siteWidth_ <= 0) {
        result.diagnostics.push_back({"placement_not_site_aligned",
            makeMessage("instance ", request.instanceId)});
        result.isLegal = false;
        return result;
    }

    std::set<InstanceId> excludedInstances;
    excludedInstances.insert(request.instanceId);
    Node* node = network_->getNode(request.instanceId);
    const OverlapInfo& overlap = checkOverlap(node);
    if (overlap.diags) {
        result.diagnostics.push_back(*overlap.diags);
        result.isLegal = false;
        return result;
    }
    excludedInstances.insert(overlap.fillers.begin(),
        overlap.fillers.end());
    for (const FillerCellRecord& change : fillerChanges) {
        excludedInstances.insert(network_->getNodeId(change.cell_id_));
    }

    const std::vector<ScanRect> snapshot =
        scanOverlaySnapshot(request, guardRegion,
            fillerChanges, useNewFillers, excludedInstances);
    for (const ScanRect& rect : snapshot) {
        if (rect.isCandidate && rect.instanceId == request.instanceId &&
            !scanSlotPolarityOk(rect)) {
            result.diagnostics.push_back({"row_slot_polarity_mismatch",
                makeMessage("instance ", rect.instanceId)});
            result.isLegal = false;
            return result;
        }
    }

    const std::vector<CheckShape> shapes = scanShapes(snapshot);
    auto run_job = [&](size_t i, std::vector<CheckOutcome>& local) {
        const Rule& rule = ruleIndex_.rules[i];
        std::vector<CheckOutcome> partial = scanRule(rule, shapes);
        local.insert(local.end(), partial.begin(), partial.end());
    };
    std::vector<CheckOutcome> outcomes;
    if (DrcUtil::getArena()) {
        tbb::enumerable_thread_specific<std::vector<CheckOutcome>> tlsOutcomes;
        DrcUtil::parallelFor(ruleIndex_.rules.size(), run_job, tlsOutcomes);
        for (std::vector<CheckOutcome>& v : tlsOutcomes) {
            outcomes.insert(outcomes.end(), v.begin(), v.end());
        }
    } else {
        for (size_t i = 0; i < ruleIndex_.rules.size(); ++i) {
            run_job(i, outcomes);
        }
    }

    result.violations = scanViolations(outcomes, shapes);
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

// -----------------------------------------------------------------------------
// print messages
// -----------------------------------------------------------------------------

// print violation data
std::string Violation::toString(Dbu siteWidth) const
{
    auto toSites = [siteWidth](Dbu v) -> Dbu {return v / siteWidth;};

    std::stringstream ss;
    ss << "  " << layerName
        << " " << dpl2::ipl::toString(ruleSource)
        << " " << dpl2::ipl::toString(relationship);

    if (!instances.empty()) {
        ss << "  merged=[" << toSites(targetInterval.xl)
            << ',' << toSites(targetInterval.xh) << ']';
        if (neighborInterval.xl != targetInterval.xl ||
            neighborInterval.xh != targetInterval.xh) {
            ss << "  neighbor=[" << toSites(neighborInterval.xl)
                << ',' << toSites(neighborInterval.xh) << ']';
        }
    }
    ss << "  measured=" << toSites(measuredValue) << 's'
        << "  required=" << toSites(requiredValue) << 's'
        << "  x=[" << toSites(xWindow.xl) << ','
        << toSites(xWindow.xh) << ']';

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

// Print checker data summary
void ImplantLayerChecker::printStats(std::ostream& os, bool isShort) const
{
    os << "========================================\n";
    os << " Implant Layer Data Summary\n";
    os << "========================================\n";

    os << "Implant Layers: " << layers_.size() << "\n";
    if (!isShort) {
        for (const Layer& il : layers_) {
            os << "  LayerId=" << il.getId() << " name=\""
                << il.getName() << "\"" << " family=";
            switch (il.getVt()) {
                case Layer::Vt::S:  os << "VTS"; break;
                case Layer::Vt::L:  os << "VTL"; break;
                case Layer::Vt::H:  os << "VTH"; break;
                case Layer::Vt::UL: os << "VTUL"; break;
                default:            os << "Unknown"; break;
            }
            os << " polarity=" << (il.getPolar() == Layer::Polar::N ? "N" : "P")
                << "\n";
        }
    }

    os << "Rules: " << rules_.size() << "\n";
    if (!isShort) {
        for (const Rule& rule : rules_) {
            os << "  RuleId=" << rule.getRuleId() << " source=";
            switch (rule.getSource()) {
                case RuleSource::Width:         os << "WIDTH"; break;
                case RuleSource::Spacing:       os << "SPACING"; break;
                case RuleSource::Lef58Width:    os << "LEF58_WIDTH"; break;
                case RuleSource::Lef58Spacing:  os << "LEF58_SPACING"; break;
            }
            os << " primaryLayer=" << rule.getPrimaryLayer() <<
                " minValue=" << rule.getMinValue();
            if (rule.getSecondaryLayer()) os << " secondaryLayer="
                << *rule.getSecondaryLayer();
            os << "\n";
        }
    }

    os << "Implant Groups: " << groups_.size() << "\n";
    os << "Masters with Implant Shapes: " << masterItems_.size() << "\n";
    if (!isShort) {
        for (const MasterItem& m : masterItems_) {
            if (m.width == 0) continue;
            os << "  MasterId=" << m.masterId << " width=" << m.width
                << " height=" << m.height
                << " siteHeight=" << m.siteHeight << " rawShapes="
                << m.rawShapes.size()
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
    if (network_) {
        placedCount = network_->getNodes().size();
        for (const std::unique_ptr<Node>& nodePtr : network_->getNodes()) {
            const Node* node = nodePtr.get();
            if (node && node->isFiller()) fillerNum++;
        }
    }
    os << "Placed Instances: " << placedCount << " filler: " << fillerNum << "\n";
    if (!isShort) {
        for (const std::unique_ptr<Node>& nodePtr : network_->getNodes()) {
            const Node* node = nodePtr.get();
            if (!node) continue;
            const RowId rowId = grid_ ? grid_->gridSnapDownY(node).v : 0;
            const ColId colId = grid_ ? grid_->gridX(node).v : 0;
            os << "  InstanceId=" << node->getId() << " masterId="
                << node->getMaster()->getId()
                << " coord=<" << rowId << ", " << colId << ">" << " orient=";
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

} // namespace ipl
} // namespace dpl2