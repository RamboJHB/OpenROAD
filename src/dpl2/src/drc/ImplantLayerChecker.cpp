#include "drc/ImplantLayerChecker.h"

#include "fillerRepair/FillerRepairEngine.h"
#include "infrastructure/fillerSetting.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>

#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"
#include <dpl2/network.h>

namespace dpl2 {
namespace ipl {

// Type alias for brevity (namespace-level type only)
using LGVec = std::unordered_map<std::string, std::vector<LayerId>>;

constexpr Dbu ZERO = 0;
constexpr Dbu ADJACENT_ROW_VERTICAL_SPACING = 1;

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

bool containsAnyInstance(const std::vector<InstanceId>& ids,
                         const std::set<InstanceId>& targets)
{
    for (InstanceId id : ids) {
        if (targets.find(id) != targets.end()) {
            return true;
        }
    }
    return false;
}
//
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
std::optional<LayerId> TrackPattern::layerForSlot(RowId rowId,
    BandSlot bandSlot) const
{
    const auto found = layerBySlot.find({rowId, bandSlot});
    if (found == layerBySlot.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<SlotRef> TrackPattern::adjacentSlots(RowId rowId,
    BandSlot bandSlot) const
{
    std::vector<SlotRef> slots;
    if (bandSlot == BandSlot::Top) {
        const auto top = layerBySlot.find({rowId + 1, BandSlot::Bottom});
        if (top != layerBySlot.end()) {
            slots.push_back({rowId + 1, BandSlot::Bottom});
        }
    } else {
        const auto bottom = layerBySlot.find({rowId - 1, BandSlot::Top});
        if (bottom != layerBySlot.end()) {
            slots.push_back({rowId - 1, BandSlot::Top});
        }
    }
    return slots;
}
//
std::optional<Layer::Polar> TrackPattern::activeInterRowKind(RowId rowA,
    RowId rowB) const
{
    const RowId low = std::min(rowA, rowB);
    const RowId high = std::max(rowA, rowB);
    const auto found = activeKindByBoundary.find({low, high});
    if (found == activeKindByBoundary.end()) {
        return std::nullopt;
    }
    return found->second;
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

bool ImplantLayerChecker::initFillerRepair(
    PhysDesMgr* desMgr,
    const fillerSetting& fillerSettings)
{
    if (fillerRepairEngine_ != nullptr) {
        return false;
    }
    auto engine = std::make_unique<fillerRepair::FillerRepairEngine>(
        grid_, network_);
    engine->setDebugLogging(fillerRepairDebugLogging_);
    const bool initialized = engine->init(desMgr, fillerSettings);
    fillerRepairEngine_ = std::move(engine);
    return initialized;
}

void ImplantLayerChecker::setFillerRepairDebugLogging(bool enabled)
{
    fillerRepairDebugLogging_ = enabled;
    if (fillerRepairEngine_ != nullptr) {
        fillerRepairEngine_->setDebugLogging(enabled);
    }
}

bool ImplantLayerChecker::updateFillerRepair(
    PhysDesMgr* desMgr,
    const fillerSetting& fillerSettings)
{
    if (fillerRepairEngine_ == nullptr) {
        return false;
    }
    fillerChanges_.clear();
    fillerRepairDiagnostics_.clear();
    if (!fillerRepairEngine_->update(desMgr, fillerSettings)) {
        return false;
    }
    // Network::updateNodes() changed the shared committed snapshot. Refresh
    // this wrapper checker as well as the engine's private oracle so direct
    // checker APIs and check() continue to describe the same revision.
    return init(desMgr);
}

CheckResult ImplantLayerChecker::precheckFillerRepair() const
{
    if (fillerRepairEngine_ == nullptr) {
        CheckResult result;
        result.isLegal = false;
        result.diagnostics.push_back(
            {"precheck_not_initialized",
             "warning: filler repair is not initialized"});
        return result;
    }
    return fillerRepairEngine_->precheck();
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
        const LayerId primaryLayer = rule.getPrimaryLayer();
        const std::optional<LayerId> secondaryLayer = rule.getSecondaryLayer();
        const std::optional<std::string> checkGroup = rule.getCheckGroup();
        if (ruleIndex_.layers.find(primaryLayer) == ruleIndex_.layers.end()) {
            diagnostics_.push_back({"skipped_missing_rule_parameter",
                makeMessage("unknown primary layer for rule ", rule.getRuleId())});
            ok = false;
            continue;
        }
        if (secondaryLayer
            && ruleIndex_.layers.find(*secondaryLayer)
                   == ruleIndex_.layers.end()) {
            diagnostics_.push_back({"skipped_missing_rule_parameter",
                makeMessage("unknown secondary layer for rule ", rule.getRuleId())});
            ok = false;
            continue;
        }
        if (!rule.getUnsupportedClauses().empty()) {
            diagnostics_.push_back({"skipped_unsupported_rule_clause",
                makeMessage("unsupported LEF58 clause in rule ", rule.getRuleId())});
        }
        if (checkGroup) {
            const auto groupIt =
                ruleIndex_.groups.find(*checkGroup);
            if (groupIt == ruleIndex_.groups.end()) {
                diagnostics_.push_back({"skipped_missing_rule_parameter",
                    "unknown implant group " + *checkGroup});
            } else if (groupIds_.find(*checkGroup) == groupIds_.end()) {
                const GroupId groupId =
                    static_cast<GroupId>(groupIds_.size() + 1);
                groupIds_[*checkGroup] = groupId;
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

    for (auto& item : masterItems_) {
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
                const bool fullSlot = y == (slot == BandSlot::Bottom ?
                    rowBase
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

// Per-instance placement logic: overlap-check, interval instantiation,
// interval overlap-check, index population, footprint insertion.
bool ImplantLayerChecker::buildPlacedInst(const Node* node, RowId rowId, Dbu x)
{
    const InstanceId instanceId = node->getId();
    const MasterId masterId = node->getMaster()->getId();
    const PhysOrientation orientation = node->getOrient();

    bool ok = true;
    if (const std::optional<Diagnostic> diagnostic =
            overlapDiag(node, rowId, x, instanceId)) {
        diagnostics_.push_back(*diagnostic);
        ok = false;
    }
    std::vector<PlacedInterval> intervals =
    instantiate(instanceId, masterId, rowId, x, orientation, false);
    // Input placement is expected not to create same-layer overlaps. Check
    // that invariant early so later merge logic can stay simple.
    for (const PlacedInterval& interval : intervals) {
    if (!slotPolarityOk(interval)) {
        diagnostics_.push_back({"row_slot_polarity_mismatch",
            makeMessage("row-slot polarity mismatch instance ",
                interval.instanceId)});
    }
    const BucketKey key{interval.rowId, interval.bandSlot, interval.layer};
    for (const PlacedInterval& existing : rowIndex_[key]) {
        if (overlaps(existing.x, interval.x)) {
            diagnostics_.push_back({"shape_overlap_in_input",
                makeMessage("placed overlap instance ",
                    interval.instanceId)});
            ok = false;
        }
      }
    }
    insertIntervals(intervals);
    insertFootprint(node, rowId, x, orientation);
    return ok;
}

// Project cached master intervals into placement coordinates for either a
// committed instance or a temporary candidate.
std::vector<ImplantLayerChecker::PlacedInterval> ImplantLayerChecker::instantiate(
    InstanceId instanceId, MasterId masterId, RowId rowId, Dbu x,
    PhysOrientation orientation, bool isCandidate) const
{
    std::vector<PlacedInterval> intervals;
    if (masterId < 0 ||
        masterId >= static_cast<MasterId>(masterItems_.size()) ||
        masterItems_[masterId].intervals.empty()) {
        return intervals;
    }
    const MasterItem& m = masterItems_[masterId];
    const int masterRows = static_cast<int>(m.height / rowHeight_);
    for (const MasterInterval& masterInterval : m.intervals) {
        XInterval transformed = masterInterval.x;
        // Master preprocessing encodes y as row offsets and band slots, so
        // vertical mirroring swaps those normalized fields instead of carrying
        // raw y coordinates into runtime checking.
        if (orientation == PhysOrientationE::MY ||
            orientation == PhysOrientationE::R180) {
            transformed = {m.width - masterInterval.x.xh,
                           m.width - masterInterval.x.xl};
        }
        int rowOffset = masterInterval.rowOffset;
        BandSlot bandSlot = masterInterval.bandSlot;
        if (orientation == PhysOrientationE::MX ||
            orientation == PhysOrientationE::R180) {
            rowOffset = masterRows - 1 - masterInterval.rowOffset;
            bandSlot = masterInterval.bandSlot == BandSlot::Top
                           ? BandSlot::Bottom
                           : BandSlot::Top;
        }
        PlacedInterval interval;
        interval.intervalId = isCandidate ? nextCandIntervalId_--
                                          : nextIntervalId_++;
        interval.instanceId = instanceId;
        interval.masterId = masterId;
        interval.shapeId = masterInterval.shapeId;
        interval.layer = masterInterval.layer;
        interval.rowId = rowId + rowOffset;
        interval.bandSlot = bandSlot;
        interval.x = {x + transformed.xl, x + transformed.xh};
        interval.isCandidate = isCandidate;
        intervals.push_back(interval);
    }
    return intervals;
}

// Add already-normalized intervals to the committed row/layer index.
void ImplantLayerChecker::insertIntervals(const PlacedIntervals& intervals)
{
    for (const PlacedInterval& interval : intervals) {
        const BucketKey key = bucketFor(interval);
        rowIndex_[key].push_back(interval);
        intervalById_[interval.intervalId] = interval;
        instIntervals_[interval.instanceId].push_back(interval.intervalId);
    }
}

void ImplantLayerChecker::insertFootprint(const Node* node, RowId rowId, Dbu x,
    PhysOrientation orientation)
{
    (void) orientation;
    const MasterId masterId = node->getMaster()->getId();
    const InstanceId instanceId = node->getId();
    if (masterId < 0 ||
        masterId >= static_cast<MasterId>(masterItems_.size()) ||
        rowHeight_ <= 0) {
        return;
    }
    const MasterItem& m = masterItems_[masterId];
    if (m.width == 0) {
        return;
    }

    const int rowSpan = std::max<Dbu>(
        1, (m.height + rowHeight_ - 1) / rowHeight_);
    const Footprint footprint{
        instanceId, masterId, XInterval{x, x + m.width}};

    std::vector<RowId>& rows = footRowsByInst_[instanceId];
    rows.clear();
    rows.reserve(static_cast<size_t>(rowSpan));
    for (int rowOffset = 0; rowOffset < rowSpan; ++rowOffset) {
        const RowId row = rowId + rowOffset;
        rows.push_back(row);
        std::vector<Footprint>& rowvec = footprintIndex_[row];
        const auto insertIt = std::lower_bound(
            rowvec.begin(),
            rowvec.end(),
            footprint.x.xl,
            [](const Footprint& left, Dbu xLeft) {
                return left.x.xl < xLeft;
            });
        rowvec.insert(insertIt, footprint);
    }
}

void ImplantLayerChecker::removeFootprint(InstanceId instanceId)
{
    const auto rowsIt = footRowsByInst_.find(instanceId);
    if (rowsIt == footRowsByInst_.end()) {
        return;
    }
    for (RowId rowId : rowsIt->second) {
        auto rowIt = footprintIndex_.find(rowId);
        if (rowIt == footprintIndex_.end()) {
            continue;
        }
        std::vector<Footprint>& row = rowIt->second;
        row.erase(std::remove_if(row.begin(),
                                 row.end(),
                                 [instanceId](const Footprint& fp) {
                                     return fp.instanceId == instanceId;
                                 }),
                  row.end());
        if (row.empty()) {
            footprintIndex_.erase(rowIt);
        }
    }
    footRowsByInst_.erase(rowsIt);
}

// Rebuild every committed merged-shape cache from the raw interval index.
void ImplantLayerChecker::rebuildShapes()
{
    // Merged shapes are materialized for every merge interpretation. This keeps
    // check-time rule evaluation fast and avoids repeatedly merging the same
    // committed intervals for each candidate.
    shapeIndex_.clear();
    groupIndex_.clear();
    shapeById_.clear();
    instShapes_.clear();
    nextMergedShapeId_ = 1;
    std::set<GroupKey> groups;
    for (auto& [key, intervals] : rowIndex_) {
        std::sort(intervals.begin(),
                  intervals.end(),
                  [](const PlacedInterval& left,
                     const PlacedInterval& right) {
                      if (left.x.xl != right.x.xl) {
                          return left.x.xl < right.x.xl;
                      }
                      return left.x.xh < right.x.xh;
                  });
        std::vector<MergedShape> merged = mergeSortedShapes(intervals, false);
        for (MergedShape& shape : merged) {
            shape.mergedShapeId = nextMergedShapeId_++;
            shapeById_[shape.mergedShapeId] = shape;
            for (InstanceId instanceId : shape.ownerInstanceIds) {
                instShapes_[instanceId].push_back(shape.mergedShapeId);
            }
        }
        shapeIndex_[key] = merged;
        const std::set<GroupKey> touched = groupKeysForBucket(key);
        groups.insert(touched.begin(), touched.end());
    }
    rebuildGroupBuckets(groups);
}
//
void ImplantLayerChecker::rebuildBuckets(const std::set<BucketKey>& buckets)
{
    for (const BucketKey& key : buckets) {
        auto intervalIt = rowIndex_.find(key);
        if (intervalIt != rowIndex_.end()) {
            std::sort(intervalIt->second.begin(),
                      intervalIt->second.end(),
                      [](const PlacedInterval& left,
                         const PlacedInterval& right) {
                          if (left.x.xl != right.x.xl) {
                              return left.x.xl < right.x.xl;
                          }
                          return left.x.xh < right.x.xh;
                      });
        }

        const auto oldIt = shapeIndex_.find(key);
        if (oldIt != shapeIndex_.end()) {
            for (const MergedShape& shape : oldIt->second) {
                eraseShapeRefs(shape);
            }
            shapeIndex_.erase(oldIt);
        }

        if (intervalIt != rowIndex_.end() && !intervalIt->second.empty()) {
            std::vector<MergedShape> merged =
                mergeSortedShapes(intervalIt->second, false);

            for (MergedShape& shape : merged) {
                shape.mergedShapeId = nextMergedShapeId_++;
                shapeById_[shape.mergedShapeId] = shape;
                for (InstanceId instanceId : shape.ownerInstanceIds) {
                    instShapes_[instanceId].push_back(shape.mergedShapeId);
                }
            }
            shapeIndex_[key] = merged;
        }

        if (intervalIt != rowIndex_.end() && intervalIt->second.empty()) {
            rowIndex_.erase(intervalIt);
        }
    }

    std::set<GroupKey> groups;
    for (const BucketKey& key : buckets) {
        const std::set<GroupKey> touched = groupKeysForBucket(key);
        groups.insert(touched.begin(), touched.end());
    }
    rebuildGroupBuckets(groups);
}

void ImplantLayerChecker::rebuildGroupBuckets(const std::set<GroupKey>& groups)
{
    for (const GroupKey& key : groups) {
        groupIndex_.erase(key);
        const auto layersIt = groupLayers_.find(key.groupId);
        if (layersIt == groupLayers_.end()) {
            continue;
        }
        std::vector<PlacedInterval> intervals;
        for (LayerId layer : layersIt->second) {
            const auto rowIt =
                rowIndex_.find({key.rowId, key.bandSlot, layer});
            if (rowIt == rowIndex_.end()) {
                continue;
            }
            intervals.insert(intervals.end(),
                             rowIt->second.begin(),
                             rowIt->second.end());
        }
        if (intervals.empty()) {
            continue;
        }
        groupIndex_[key] = mergeGroupShapes(intervals, false);
    }
}
//
ImplantLayerChecker::BucketKey
ImplantLayerChecker::bucketFor(const PlacedInterval& interval) const
{
    return {interval.rowId, interval.bandSlot, interval.layer};
}

std::set<ImplantLayerChecker::BucketKey>
ImplantLayerChecker::bucketsForInstance(InstanceId instanceId) const
{
    std::set<BucketKey> buckets;
    const auto found = instIntervals_.find(instanceId);
    if (found == instIntervals_.end()) {
        return buckets;
    }
    for (int intervalId : found->second) {
        const auto intervalIt = intervalById_.find(intervalId);
        if (intervalIt != intervalById_.end()) {
            buckets.insert(bucketFor(intervalIt->second));
        }
    }
    return buckets;
}

std::set<ImplantLayerChecker::BucketKey>
ImplantLayerChecker::bucketsForIntervals(const PlacedIntervals& intervals) const
{
    std::set<BucketKey> buckets;
    for (const PlacedInterval& interval : intervals) {
        buckets.insert(bucketFor(interval));
    }
    return buckets;
}

void ImplantLayerChecker::eraseShapeRefs(const MergedShape& shape)
{
    shapeById_.erase(shape.mergedShapeId);
    for (InstanceId instanceId : shape.ownerInstanceIds) {
        auto ownerIt = instShapes_.find(instanceId);
        if (ownerIt == instShapes_.end()) {
            continue;
        }
        std::vector<int>& ids = ownerIt->second;
        ids.erase(std::remove(ids.begin(), ids.end(), shape.mergedShapeId),
                  ids.end());
        if (ids.empty()) {
            instShapes_.erase(ownerIt);
        }
    }
}

std::set<ImplantLayerChecker::GroupKey>
ImplantLayerChecker::groupKeysForBucket(const BucketKey& key) const
{
    std::set<GroupKey> groups;
    const auto found = layerGroups_.find(key.layer);
    if (found == layerGroups_.end()) {
        return groups;
    }
    for (GroupId groupId : found->second) {
        groups.insert({key.rowId, key.bandSlot, groupId});
    }
    return groups;
}

// Merge sorted intervals into continuous x shapes on one implant layer.
std::vector<ImplantLayerChecker::MergedShape>
ImplantLayerChecker::mergeShapes(const PlacedIntervals& intervals, bool isCandidate) const
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
                  if (left.layer != right.layer) {
                      return left.layer < right.layer;
                  }
                  if (left.x.xl != right.x.xl) {
                      return left.x.xl < right.x.xl;
                  }
                  return left.x.xh < right.x.xh;
            });

    return mergeSortedShapes(sorted, isCandidate);
}

std::vector<ImplantLayerChecker::MergedShape>
ImplantLayerChecker::mergeSortedShapes(const PlacedIntervals& intervals,
    bool isCandidate) const
{
    std::vector<MergedShape> merged;
    merged.reserve(intervals.size());
    for (const PlacedInterval& interval : intervals) {
        // Adjacent or overlapping x intervals form one electrically continuous
        // implant shape within the same row band and layer.
        if (merged.empty() ||
            merged.back().rowId != interval.rowId ||
            merged.back().bandSlot != interval.bandSlot ||
            merged.back().layer != interval.layer ||
            interval.x.xl > merged.back().x.xh) {
            MergedShape shape;
            shape.mergedShapeId = isCandidate ? nextCandShapeId_-- : 0;
            shape.rowId = interval.rowId;
            shape.bandSlot = interval.bandSlot;
            shape.layer = interval.layer;
            shape.x = interval.x;
            shape.ownerIntervalIds.push_back(interval.intervalId);
            shape.ownerInstanceIds.push_back(interval.instanceId);
            shape.ownerShapeIds.push_back(interval.shapeId);
            shape.isCandidate = isCandidate;
            merged.push_back(shape);
            continue;
        }
        MergedShape& active = merged.back();
        active.x.xh = std::max(active.x.xh, interval.x.xh);
        appendUnique(active.ownerIntervalIds, interval.intervalId);
        appendUnique(active.ownerInstanceIds, interval.instanceId);
        appendUnique(active.ownerShapeIds, interval.shapeId);
    }
    return merged;
}
//
std::vector<ImplantLayerChecker::MergedShape>
ImplantLayerChecker::mergeGroupShapes(const PlacedIntervals& intervals,
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

    std::vector<MergedShape> merged;
    merged.reserve(sorted.size());
    for (const PlacedInterval& interval : sorted) {
        if (merged.empty() ||
            merged.back().rowId != interval.rowId ||
            merged.back().bandSlot != interval.bandSlot ||
            interval.x.xl > merged.back().x.xh) {
            MergedShape shape;
            shape.mergedShapeId = isCandidate ? nextCandShapeId_-- : 0;
            shape.rowId = interval.rowId;
            shape.bandSlot = interval.bandSlot;
            shape.layer = interval.layer;
            shape.x = interval.x;
            shape.ownerIntervalIds.push_back(interval.intervalId);
            shape.ownerInstanceIds.push_back(interval.instanceId);
            shape.ownerShapeIds.push_back(interval.shapeId);
            shape.isCandidate = isCandidate;
            merged.push_back(shape);
            continue;
        }
        MergedShape& active = merged.back();
        active.x.xh = std::max(active.x.xh, interval.x.xh);
        appendUnique(active.ownerIntervalIds, interval.intervalId);
        appendUnique(active.ownerInstanceIds, interval.instanceId);
        appendUnique(active.ownerShapeIds, interval.shapeId);
    }
    return merged;
}

// Checker Entry Interface
bool ImplantLayerChecker::check(const Node* node,
                                GridX x,
                                GridY y,
                                const PhysOrientation& orient) const
{
    (void) y;
    fillerChanges_.clear();
    fillerRepairDiagnostics_.clear();
    if (!node || !network_ || !grid_) {
        return false;
    }
    CheckRequest request;
    request.instanceId = node->getId();
    request.masterId = node->getMaster()->getId();
    request.rowId = grid_->gridSnapDownY(node).v;
    request.colId = x.v;
    request.orientation = orient;

    if (fillerRepairEngine_ == nullptr) {
        return checkPlace(request).isLegal;
    }

    // Do not pre-screen with this checker's committed cache. DePlace may have
    // already changed the live Node master for the candidate, while this
    // checker still represents the pre-commit revision. The engine consumes
    // the explicit request and its private immutable snapshot, and its first
    // empty-filler overlay is the single legality decision for this path.
    const fillerRepair::RepairOutcome outcome
        = fillerRepairEngine_->repair(request);
    fillerRepairDiagnostics_ = outcome.diagnostics;
    if (!outcome.hasSolution) {
        return false;
    }
    fillerChanges_ = outcome.changes;
    return true;
}

// Check one candidate placement against the committed merged-shape indexes.
CheckResult ImplantLayerChecker::checkPlace(const CheckRequest& request) const
{
    CheckResult result;
    if (siteWidth_ <= 0 || request.colId < 0) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(
            {"placement_not_site_aligned",
            makeMessage("placement is not site-aligned for instance ",
                request.instanceId)});
        result.isLegal = false;
        return result;
    }
    const OverlapInfo overlap = overlapInfo(request);
    if (overlap.blockingOverlap) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(*overlap.blockingOverlap);
        result.isLegal = false;
        return result;
    }
    std::set<InstanceId> excludedInstances = overlap.removableFillers;
    excludedInstances.insert(request.instanceId);

    const bool sameCommittedPose = isSameCommittedPose(request);
    const bool needsCandidateTargets =
        !sameCommittedPose ||
        std::any_of(ruleIndex_.rules.begin(), ruleIndex_.rules.end(),
                    [](const Rule& rule) { return isSpacingRule(rule.getSource())
                    || (isWidthRule(rule.getSource()) && rule.getZeroPrl()); });
    std::vector<MergedShape> candidateTargets;
    const std::vector<MergedShape> committedTargets =
        sameCommittedPose ? committedTargetShapes(request.instanceId)
                          : std::vector<MergedShape>{};
    if (needsCandidateTargets) {
        // The candidate is never inserted into committed indexes. It is
        // normalized into temporary intervals and merged on the fly.
        const std::vector<PlacedInterval> targetIntervals =
            instantiate(request.instanceId,
                        request.masterId,
                        request.rowId,
                        request.colId * siteWidth_,
                        request.orientation,
                        true);
        // Skip polarity check for committed poses — already validated during
        // buildPlacedInst (the diagnostic is still recorded for reference).
        if (!sameCommittedPose) {
            for (const PlacedInterval& interval : targetIntervals) {
                if (!slotPolarityOk(interval)) {
                    result.diagnostics = diagnostics_;
                    result.diagnostics.push_back(
                        {"row_slot_polarity_mismatch",
                        makeMessage("row-slot polarity mismatch instance ",
                            interval.instanceId)});
                    result.isLegal = false;
                    return result;
                }
            }
        }
        candidateTargets = mergeShapes(targetIntervals, true);
    }

    std::vector<RuleOutcome> outcomes;
    for (const Rule& rule : ruleIndex_.rules) {
        const CheckMode ruleMode =
            sameCommittedPose && isWidthRule(rule.getSource()) && !rule.getZeroPrl()
                ? CheckMode::Committed
                : CheckMode::Candidate;
        const auto& targets = ruleMode == CheckMode::Committed
                                  ? committedTargets
                                  : candidateTargets;
        std::vector<RuleOutcome> partial =
            evalRule(rule, targets, ruleMode, excludedInstances);
        outcomes.insert(outcomes.end(), partial.begin(), partial.end());
    }
    result.violations = makeViolations(outcomes);
    result.isLegal = result.violations.empty();
    result.diagnostics = diagnostics_;
    return result;
}
//
bool ImplantLayerChecker::isSameCommittedPose(const CheckRequest& request) const
{
    if (siteWidth_ <= 0 || !network_ || !grid_) {
        return false;
    }
    const Node* node = network_->getNode(request.instanceId);
    if (!node) {
        return false;
    }
    const RowId committedRowId = grid_->gridSnapDownY(node).v;
    const ColId committedColId = grid_->gridX(node).v;
    return node->getMaster()->getId() == request.masterId &&
           committedRowId == request.rowId &&
           committedColId == request.colId &&
           node->getOrient() == request.orientation;
}

std::vector<ImplantLayerChecker::MergedShape>
ImplantLayerChecker::committedTargetShapes(InstanceId instanceId) const
{
    std::vector<MergedShape> shapes;
    const auto found = instShapes_.find(instanceId);
    if (found == instShapes_.end()) {
        return shapes;
    }

    std::set<int> seen;
    for (int shapeId : found->second) {
        if (!seen.insert(shapeId).second) {
            continue;
        }
        const MergedShape* shape = findShape(shapeId);
        if (shape == nullptr) {
            continue;
        }
        shapes.push_back(*shape);
    }
    return shapes;
   }

// Oracle Check: check one candidate by rebuilding a
// complete scan snapshot from current committed instances. Not use cache.
CheckResult ImplantLayerChecker::checkDirect(const CheckRequest& request) const
{
    CheckResult result;
    if (siteWidth_ <= 0 || request.colId < 0) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(
            {"placement_not_site_aligned",
            makeMessage("placement is not site-aligned for instance ",
                request.instanceId)});
        result.isLegal = false;
        return result;
    }
    const OverlapInfo overlap = overlapInfo(request);
    if (overlap.blockingOverlap) {
        result.diagnostics = diagnostics_;
        result.diagnostics.push_back(*overlap.blockingOverlap);
        result.isLegal = false;
        return result;
    }
    std::set<InstanceId> excludedInstances = overlap.removableFillers;
    excludedInstances.insert(request.instanceId);

    const bool sameCommittedPose = isSameCommittedPose(request);
    const std::vector<ScanRect> snapshot =
        scanSnapshot(request, excludedInstances);
    // Skip polarity check for committed poses — already validated during
    // buildPlacedInst (the diagnostic is still recorded for reference).
    if (!sameCommittedPose) {
        for (const ScanRect& rect : snapshot) {
            if (rect.isCandidate && !scanSlotPolarityOk(rect)) {
                result.diagnostics = diagnostics_;
                result.diagnostics.push_back({"row_slot_polarity_mismatch",
                    makeMessage("row-slot polarity mismatch instance ",
                        rect.instanceId)});
                result.isLegal = false;
                return result;
            }
        }
    }

    const std::vector<ScanShape> shapes = scanShapes(snapshot);
    std::vector<ScanOutcome> outcomes;
    for (const Rule& rule : ruleIndex_.rules) {
        std::vector<ScanOutcome> partial = scanRule(rule, shapes);
        outcomes.insert(outcomes.end(), partial.begin(), partial.end());
    }

    result.violations = scanViolations(outcomes);
    result.isLegal = result.violations.empty();
    result.diagnostics = diagnostics_;
    return result;
}

std::vector<CheckResult>
ImplantLayerChecker::checkPlaceWithOverlays(
    const CheckRequest& request, const Rect& guardRegion,
    const std::vector<FillerChanges>& fillerChanges) const
{
    std::vector<CheckResult> results;
    results.reserve(fillerChanges.size());
    const std::vector<Violation> oldViolations =
        checkOverlayRegion(request, guardRegion, {}, false).violations;
    for (const FillerChanges& changes : fillerChanges) {
        results.push_back(checkPlaceWithOverlay(request,
            guardRegion, changes, oldViolations));
    }
    return results;
}

CheckResult
ImplantLayerChecker::checkPlaceWithOverlay(
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
                     requestDiagnostics.empty() &&
                     overlay.diagnostics.empty();
    return result;
}

CheckResult
ImplantLayerChecker::checkOverlayRegion(
    const CheckRequest& request,
    const Rect& guardRegion,
    const FillerChanges& fillerChanges,
    bool useNewFillers) const
{
    CheckResult result;
    result.diagnostics = diagnostics_;
    if (siteWidth_ <= 0) {
        result.diagnostics.push_back({"placement_not_site_aligned",
            makeMessage("placement is not site-aligned for instance ",
                request.instanceId)});
        result.isLegal = false;
        return result;
    }

    std::set<InstanceId> excludedInstances;
    excludedInstances.insert(request.instanceId);
    const OverlapInfo overlap = overlapInfo(request);
    if (overlap.blockingOverlap) {
        result.diagnostics.push_back(*overlap.blockingOverlap);
        result.isLegal = false;
        return result;
    }
    excludedInstances.insert(overlap.removableFillers.begin(),
                             overlap.removableFillers.end());
    for (const FillerCellRecord& change : fillerChanges) {
        excludedInstances.insert(network_->getNodeId(change.cell_id_));
    }

    const std::vector<ScanRect> snapshot =
        scanOverlaySnapshot(request,
                            guardRegion,
                            fillerChanges,
                            useNewFillers,
                            excludedInstances);
    for (const ScanRect& rect : snapshot) {
        if (rect.isCandidate && !scanSlotPolarityOk(rect)) {
            result.diagnostics.push_back({"row_slot_polarity_mismatch",
                makeMessage("row-slot polarity mismatch instance ",
                    rect.instanceId)});
            result.isLegal = false;
            return result;
        }
    }

    const std::vector<ScanShape> shapes = scanShapes(snapshot);
    std::vector<ScanOutcome> outcomes;
    for (const Rule& rule : ruleIndex_.rules) {
        std::vector<ScanOutcome> partial = scanRule(rule, shapes);
        outcomes.insert(outcomes.end(), partial.begin(), partial.end());
    }

    result.violations = scanViolations(outcomes);
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

// Evaluate one normalized rule against target merged shapes and collect raw
// outcomes before containment suppression.
auto ImplantLayerChecker::evalRule(const Rule& rule,
    const MergedShapes& targetShapes, CheckMode mode,
    const std::set<InstanceId>& excludedInstances)
    const -> std::vector<RuleOutcome>
{
    std::vector<RuleOutcome> outcomes;
    if (!rule.getUnsupportedClauses().empty()) {
        RuleOutcome outcome;
        outcome.ruleId = rule.getRuleId();
        outcome.status = OutcomeStatus::Skipped;
        outcomes.push_back(outcome);
        return outcomes;
    }

    for (const MergedShape& target : targetShapes) {
        if (target.layer != rule.getPrimaryLayer()) {
            continue;
        }
        std::vector<Relationship> relationships = {Relationship::IntraRow,
                                                   Relationship::InterRow};
        for (Relationship relationship : relationships) {
            if (!ruleAppliesTo(rule, relationship)) {
                continue;
            }
            MergedShape checkTarget = target;
            if (isWidthRule(rule.getSource()) &&
                relationship == Relationship::InterRow) {
                for (const MergedShape& sameRowNeighbor :
                     findNeighbors(target,
                                   rule,
                                   Relationship::IntraRow,
                                   mode,
                                   targetShapes,
                                   excludedInstances)) {
                    if (target.layer == sameRowNeighbor.layer &&
                        touchesOrOverlaps(checkTarget.x, sameRowNeighbor.x)) {
                        checkTarget.x = unite(checkTarget.x, sameRowNeighbor.x);
                    }
                }
            }
            std::vector<MergedShape> neighbors =
                findNeighbors(checkTarget,
                              rule,
                              relationship,
                              mode,
                              targetShapes,
                              excludedInstances);
            // A width rule can fail even without a same-row neighbor. Inter-row
            // width is only meaningful when an adjacent row contributes a
            // merged neighbor shape, so the fallback is limited to intra-row.
            if (isWidthRule(rule.getSource()) && neighbors.empty()
                && relationship == Relationship::IntraRow) {
                if (!isWidthRule(rule.getSource())) {
                    continue;
                }
                RuleOutcome outcome;
                outcome.ruleId = rule.getRuleId();
                outcome.context = {rule.getSource(),
                                   rule.getPrimaryLayer(),
                                   rule.getSecondaryLayer(),
                                   relationship,
                                   checkTarget.mergedShapeId,
                                   std::nullopt,
                                   checkTarget.rowId,
                                   checkTarget.bandSlot,
                                   checkTarget.x,
                                   checkTarget.x};
                outcome.measuredValue = checkTarget.x.xh - checkTarget.x.xl;
                outcome.requiredValue = rule.getMinValue();
                outcome.status = outcome.measuredValue < rule.getMinValue()
                                     ? OutcomeStatus::Violated
                                     : OutcomeStatus::Satisfied;
                outcomes.push_back(outcome);
                continue;
            }
            for (const MergedShape& neighbor : neighbors) {
                RuleOutcome outcome;
                outcome.ruleId = rule.getRuleId();
                outcome.context = {rule.getSource(),
                                   rule.getPrimaryLayer(),
                                   rule.getSecondaryLayer(),
                                   relationship,
                                   checkTarget.mergedShapeId,
                                   neighbor.mergedShapeId,
                                   checkTarget.rowId,
                                   checkTarget.bandSlot,
                                   unite(checkTarget.x, neighbor.x),
                                   checkTarget.x};
                outcome.requiredValue = rule.getMinValue();

                const Dbu projected = prl(checkTarget.x, neighbor.x);
                // LEF58 predicates are filters on applicability. They produce
                // NotApplicable outcomes so containment and diagnostics can
                // still see that the rule was considered.
                if (rule.getExceptAbutted() && projected == 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                                       outcomes.push_back(outcome);
                    continue;
                }
                if (rule.getExceptCornerTouch() && relationship ==
                    Relationship::InterRow &&
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
                    !hasIntersectCoverage(rule, checkTarget, neighbor)) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.getPrl()) {
                    const bool lef58VerticalSpacing =
                        rule.getSource() == RuleSource::Lef58Spacing &&
                        relationship == Relationship::InterRow;
                    const bool lef58HorizontalSpacing =
                        rule.getSource() == RuleSource::Lef58Spacing &&
                        relationship == Relationship::IntraRow;
                    if (!lef58HorizontalSpacing && *rule.getPrl() >= 0 &&
                        projected <= *rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (lef58VerticalSpacing && *rule.getPrl() < 0 &&
                        spacing(checkTarget.x, neighbor.x) >= -*rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (!lef58HorizontalSpacing && !lef58VerticalSpacing &&
                        *rule.getPrl() < 0 &&
                        spacing(target.x, neighbor.x) > -*rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                }

                if (isWidthRule(rule.getSource())) {
                    XInterval effective = checkTarget.x;
                    // Same-row abutment measures the unioned run. Adjacent-row
                    // width measures only the common x-overlap at the row
                    // boundary;
                    if (relationship == Relationship::InterRow) {
                        if (!overlaps(checkTarget.x, neighbor.x)) {
                            outcome.status = OutcomeStatus::NotApplicable;
                            outcomes.push_back(outcome);
                            continue;
                        }
                        effective = intersect(checkTarget.x, neighbor.x);
                    } else if (touchesOrOverlaps(checkTarget.x, neighbor.x)) {
                        effective = unite(checkTarget.x, neighbor.x);
                    }
                    outcome.context.xWindow = effective;
                    if (rule.getCheckGroup() &&
                        !groupFails(rule,
                                    checkTarget,
                                    neighbor,
                                    relationship,
                                    effective,
                                    excludedInstances)) {
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
                        touchesOrOverlaps(checkTarget.x, neighbor.x)) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    outcome.measuredValue =
                        rule.getSource() == RuleSource::Lef58Spacing &&
                                relationship == Relationship::InterRow
                            ? ADJACENT_ROW_VERTICAL_SPACING
                            : spacing(checkTarget.x, neighbor.x);
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

// Query committed neighbor merged shapes for the requested relationship.
std::vector<ImplantLayerChecker::MergedShape>
ImplantLayerChecker::findNeighbors(
    const MergedShape& target, const Rule& rule,
    Relationship relationship, CheckMode mode,
    const MergedShapes& targetShapes,
    const std::set<InstanceId>& excludedInstances) const
{
    std::vector<MergedShape> neighbors;
    const Dbu radius = queryRadius(rule);
    const XInterval queryWindow{target.x.xl - radius, target.x.xh + radius};
    std::vector<LayerId> layers;
    layers.push_back(rule.getSecondaryLayer().value_or(rule.getPrimaryLayer()));

    std::vector<SlotRef> slots;
    if (relationship == Relationship::IntraRow) {
        slots.push_back({target.rowId, target.bandSlot});
    } else {
        // Inter-row width and spacing use the same power-grid adjacency:
        // top-band shapes interact with the bottom band in the row above, and
        // bottom-band shapes interact with the top band in the row below.
        const RowId neighborRowId = target.bandSlot == BandSlot::Top ?
            target.rowId + 1 : target.rowId - 1;
        const auto active = tracks_.activeInterRowKind(target.rowId,
            neighborRowId);
        const auto layerIt = ruleIndex_.layers.find(target.layer);
        bool polarityOk = !(active
            && layerIt != ruleIndex_.layers.end()
            && layerIt->second.getPolar() != *active);
        if (!polarityOk) {
            return neighbors;
        }
        slots = tracks_.adjacentSlots(target.rowId, target.bandSlot);
        // Also search the same band slot in the adjacent row
        // for layers that do not alternate (non-track layers).
        bool alreadyPresent = false;
        for (const auto& s : slots) {
            if (s.rowId == neighborRowId && s.bandSlot == target.bandSlot) {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent) {
            slots.push_back({neighborRowId, target.bandSlot});
        }
    }

    const bool returnSameRowRuns =
        isWidthRule(rule.getSource()) && relationship == Relationship::InterRow;

    for (SlotRef slot : slots) {
        for (LayerId layer : layers) {
            std::vector<MergedShape> bucketNeighbors;
            const BucketKey key{slot.rowId, slot.bandSlot, layer};
            const auto found = shapeIndex_.find(key);
            if (found != shapeIndex_.end()) {
                const std::vector<MergedShape>& shapes = found->second;
                const auto first = std::lower_bound(
                    shapes.begin(),
                    shapes.end(),
                    queryWindow.xl,
                    [](const MergedShape& shape, Dbu xl) {
                        return shape.x.xh < xl;
                    });
                for (auto shapeIt = first; shapeIt != shapes.end(); ++shapeIt) {
                    const MergedShape& shape = *shapeIt;
                    if (shape.x.xl > queryWindow.xh) {
                        break;
                    }
                    if (mode == CheckMode::Committed &&
                        shape.mergedShapeId == target.mergedShapeId) {
                        continue;
                    }
                    std::vector<MergedShape> candidateShapes;
                    bool containsTargetOwner = false;
                    if (mode == CheckMode::Candidate) {
                        for (InstanceId owner : shape.ownerInstanceIds) {
                            if (excludedInstances.find(owner) !=
                                excludedInstances.end()) {
                                containsTargetOwner = true;
                                break;
                            }
                        }
                    }
                    if (!containsTargetOwner) {
                        candidateShapes.push_back(shape);
                    } else {
                        std::vector<PlacedInterval> remainingIntervals;
                        for (int intervalId : shape.ownerIntervalIds) {
                            const auto intervalIt = intervalById_.find(intervalId);
                            if (intervalIt == intervalById_.end()) {
                                continue;
                            }
                            if (excludedInstances.find(
                                    intervalIt->second.instanceId) !=
                                excludedInstances.end()) {
                                continue;
                            }
                            remainingIntervals.push_back(intervalIt->second);
                        }
                        candidateShapes =
                            mergeShapes(remainingIntervals, false);
                        for (MergedShape& candidateShape : candidateShapes) {
                            candidateShape.mergedShapeId = shape.mergedShapeId;
                        }
                    }

                    for (const MergedShape& candidateShape : candidateShapes) {
                        if (candidateShape.x.xh < queryWindow.xl ||
                            candidateShape.x.xl > queryWindow.xh) {
                            continue;
                        }
                        if (relationship == Relationship::IntraRow &&
                            !touchesOrOverlaps(target.x, candidateShape.x) &&
                            isWidthRule(rule.getSource())) {
                            continue;
                        }
                        bucketNeighbors.push_back(candidateShape);
                    }
                }
            }
            if (mode == CheckMode::Candidate) {
                for (const MergedShape& shape : targetShapes) {
                    if (shape.mergedShapeId == target.mergedShapeId ||
                        shape.rowId != slot.rowId ||
                        shape.bandSlot != slot.bandSlot ||
                        shape.layer != layer ||
                        shape.x.xh < queryWindow.xl ||
                        shape.x.xl > queryWindow.xh) {
                        continue;
                    }
                    if (relationship == Relationship::IntraRow &&
                        !touchesOrOverlaps(target.x, shape.x) &&
                        isWidthRule(rule.getSource())) {
                        continue;
                    }
                    bucketNeighbors.push_back(shape);
                }
            }

            if (!returnSameRowRuns) {
                neighbors.insert(neighbors.end(),
                                 bucketNeighbors.begin(),
                                 bucketNeighbors.end());
                continue;
            }

            std::sort(bucketNeighbors.begin(),
                      bucketNeighbors.end(),
                      [](const MergedShape& left, const MergedShape& right) {
                          if (left.x.xl != right.x.xl) {
                              return left.x.xl < right.x.xl;
                          }
                          return left.x.xh < right.x.xh;
                      });
            for (const MergedShape& shape : bucketNeighbors) {
                if (neighbors.empty() ||
                    neighbors.back().rowId != shape.rowId ||
                    neighbors.back().bandSlot != shape.bandSlot ||
                    neighbors.back().layer != shape.layer ||
                    !touchesOrOverlaps(neighbors.back().x, shape.x)) {
                    neighbors.push_back(shape);
                    continue;
                }

                MergedShape& run = neighbors.back();
                run.x = unite(run.x, shape.x);
                run.isCandidate = run.isCandidate || shape.isCandidate;
                for (int id : shape.ownerIntervalIds) {
                    appendUnique(run.ownerIntervalIds, id);
                }
                for (InstanceId id : shape.ownerInstanceIds) {
                    appendUnique(run.ownerInstanceIds, id);
                }
                for (ShapeId id : shape.ownerShapeIds) {
                    appendUnique(run.ownerShapeIds, id);
                }
            }
        }
    }

    if (returnSameRowRuns) {
        neighbors.erase(std::remove_if(neighbors.begin(),
                                       neighbors.end(),
                                       [&target](const MergedShape& shape) {
                                                                            return !overlaps(target.x,
                                                        shape.x);
                                   }),
                       neighbors.end());
    }

    return neighbors;
}

// Convert raw rule outcomes into final violations, suppressing broad-rule
// failures when a contained specific rule is satisfied for the same context.
ViolationVec
ImplantLayerChecker::makeViolations(const RuleOutcomeVec& outcomes) const
{
    std::set<size_t> suppressed;
    // Containment means a narrower/specialized rule can legalize the same
    // context even when the broader rule would otherwise report a violation.
    for (size_t broadIndex = 0; broadIndex < outcomes.size(); ++broadIndex) {
        const RuleOutcome& broad = outcomes[broadIndex];
        if (broad.status != OutcomeStatus::Violated) {
            continue;
        }
        const auto broadRuleIt = ruleIndex_.ruleById.find(broad.ruleId);
        if (broadRuleIt == ruleIndex_.ruleById.end() ||
            !broadRuleIt->second.getContainmentGroup()) {
            continue;
        }
        for (const RuleOutcome& specific : outcomes) {
            if (specific.status != OutcomeStatus::Satisfied) {
                continue;
            }
            const auto specificRuleIt = ruleIndex_.ruleById.find(specific.ruleId);
            if (specificRuleIt == ruleIndex_.ruleById.end()) {
                continue;
            }
            const Rule& specificRule = specificRuleIt->second;
            if (specificRule.getContainmentGroup() !=
                broadRuleIt->second.getContainmentGroup()) {
                continue;
            }
            const std::vector<int> containedByRuleIds
                = specificRule.getContainedByRuleIds();
            if (std::find(containedByRuleIds.begin(),
                          containedByRuleIds.end(),
                          broad.ruleId) == containedByRuleIds.end()) {
                continue;
            }
            if (isContainedContext(specific.context, broad.context)) {
                suppressed.insert(broadIndex);
                break;
            }
        }
    }

    std::vector<Violation> violations;
    for (size_t i = 0; i < outcomes.size(); ++i) {
        const RuleOutcome& outcome = outcomes[i];
        if (outcome.status != OutcomeStatus::Violated ||
            suppressed.find(i) != suppressed.end()) {
            continue;
        }
        const auto ruleIt = ruleIndex_.ruleById.find(outcome.ruleId);
        if (ruleIt == ruleIndex_.ruleById.end()) {
            continue;
        }
        Violation violation;
        violation.ruleId = outcome.ruleId;
        violation.ruleSource = ruleIt->second.getSource();
        violation.primaryLayer = outcome.context.primaryLayer;
        violation.secondaryLayer = outcome.context.secondaryLayer;
        violation.measuredValue = outcome.measuredValue;
        violation.requiredValue = outcome.requiredValue;
        violation.xWindow = outcome.context.xWindow;
        violation.relationship = outcome.context.relationship;
        if (const MergedShape* target =
                findShape(outcome.context.targetMergedShapeId)) {
            violation.instances = target->ownerInstanceIds;
            violation.shapeIds = target->ownerShapeIds;
            violation.mergedShapeIds.push_back(target->mergedShapeId);
            violation.targetInterval = target->x;
            appendUnique(violation.rowIds, target->rowId);
        } else {
            // Candidate target shapes are not stored in shapeById_;
            // use targetX from the outcome context.
            violation.targetInterval = outcome.context.targetX;
        }
        bool hasNeighbor = false;
        if (outcome.context.neighborMergedShapeId) {
            if (const MergedShape* neighbor =
                    findShape(*outcome.context.neighborMergedShapeId)) {
                for (InstanceId id : neighbor->ownerInstanceIds) {
                    appendUnique(violation.instances, id);
                }
                for (ShapeId id : neighbor->ownerShapeIds) {
                    appendUnique(violation.shapeIds, id);
                }
                violation.mergedShapeIds.push_back(neighbor->mergedShapeId);
                violation.neighborInterval = neighbor->x;
                hasNeighbor = true;
                appendUnique(violation.rowIds, neighbor->rowId);
            }
        }
        if (!hasNeighbor) {
            violation.neighborInterval = violation.targetInterval;
        }
        // Look up layer name
        auto layerIt = ruleIndex_.layers.find(outcome.context.primaryLayer);
        if (layerIt != ruleIndex_.layers.end()) {
            violation.layerName = layerIt->second.getName();
        }
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
        for (const auto& nodePtr : network_->getNodes()) {
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
                node->getMaster()->getId(), rowId, colId,
                node->getOrient(), false);
            snapshot.insert(snapshot.end(), rects.begin(), rects.end());
        }
    }

    std::vector<ScanRect> candidateRects = scanInst(request.instanceId,
        request.masterId, request.rowId, request.colId, request.orientation, true);
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
        for (const auto& nodePtr : network_->getNodes()) {
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
                node->getMaster()->getId(), rowId, colId,
                node->getOrient(), false);
            for (ScanRect& rect : rects) {
                if (isInGuard(xOf(rect.rect), {rect.rowId}, guardRegion)) {
                    rect.isCandidate = true;
                }
            }
            snapshot.insert(snapshot.end(), rects.begin(), rects.end());
        }
    }

    std::vector<ScanRect> targetRects = scanInst(request.instanceId,
        request.masterId, request.rowId,
        request.colId, request.orientation, true);
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
            fillerMasterId = network_->getMasterId(change.new_lib_cell_);
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

std::vector<ImplantLayerChecker::ScanRect>
ImplantLayerChecker::scanInst(InstanceId instanceId,
    MasterId masterId, RowId rowId, ColId colId,
    PhysOrientation orientation, bool isCandidate) const
{
    std::vector<ScanRect> rects;
    if (rowHeight_ <= 0 || siteWidth_ <= 0) {
        return rects;
    }
    if (masterId < 0 ||
        masterId >= static_cast<MasterId>(masterItems_.size())) {
        return rects;
    }
    const MasterItem& master = masterItems_[masterId];
    if (master.width == 0) {
        return rects;
    }
    const Dbu originX = colId * siteWidth_;

    for (const MasterShape& shape : master.shapes) {
        // Convert eUTL::Rect coordinates to Dbu for checker use.
        CheckerRect local;
        local.xl = shape.rect._xl.getStorage();
        local.yl = shape.rect._yl.getStorage();
        local.xh = shape.rect._xh.getStorage();
        local.yh = shape.rect._yh.getStorage();

        if (orientation == PhysOrientationE::MY ||
            orientation == PhysOrientationE::R180) {
            CheckerRect mirrored;
            mirrored.xl = master.width - local.xh;
            mirrored.yl = local.yl;
            mirrored.xh = master.width - local.xl;
            mirrored.yh = local.yh;
            local = mirrored;
        }
        if (orientation == PhysOrientationE::MX ||
            orientation == PhysOrientationE::R180) {
            CheckerRect mirrored;
            mirrored.xl = local.xl;
            mirrored.yl = master.height - local.yh;
            mirrored.xh = local.xh;
            mirrored.yh = master.height - local.yl;
            local = mirrored;
        }

        Dbu y = local.yl;
        while (y < local.yh) {
            const int rowOffset = static_cast<int>(y / rowHeight_);
            const Dbu rowBase =
                static_cast<Dbu>(rowOffset) * rowHeight_;
            const Dbu halfRow = rowHeight_ / 2;
            const Dbu bottomEnd = rowBase + halfRow;
            const BandSlot slot = y < bottomEnd ? BandSlot::Bottom
                                                : BandSlot::Top;
            const Dbu slotEnd = slot == BandSlot::Bottom
                                        ? bottomEnd
                                        : rowBase + rowHeight_;
            const Dbu pieceEnd = std::min(local.yh, slotEnd);
            if (pieceEnd <= y) {
                break;
            }
            ScanRect placed;
            placed.instanceId = instanceId;
            placed.masterId = masterId;
            placed.shapeId = shape.shapeId;
            placed.layer = shape.layer;
                          placed.rowId = rowId + rowOffset;
            placed.bandSlot = slot;
            placed.rect = {originX + local.xl,
                           y,
                           originX + local.xh,
                           pieceEnd};
            placed.isCandidate = isCandidate;
            rects.push_back(placed);
            y = pieceEnd;
        }
    }
    return rects;
}

std::vector<ImplantLayerChecker::ScanShape>
ImplantLayerChecker::scanShapes(const ScanRectVec& rects) const
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

    std::vector<ScanShape> shapes;
    int nextId = 1;
    for (const ScanRect& rect : sorted) {
        const XInterval x = xOf(rect.rect);
        if (shapes.empty() ||
            shapes.back().rowId != rect.rowId ||
            shapes.back().bandSlot != rect.bandSlot ||
            shapes.back().layer != rect.layer ||
            shapes.back().containsCandidate != rect.isCandidate ||
            x.xl > shapes.back().bbox.xh) {
            ScanShape shape;
            shape.shapeId = nextId++;
            shape.layer = rect.layer;
            shape.rowId = rect.rowId;
            shape.bandSlot = rect.bandSlot;
            shape.bbox = rect.rect;
            shape.ownerInstanceIds = {rect.instanceId};
            shape.ownerShapeIds = {rect.shapeId};
            shape.containsCandidate = rect.isCandidate;
            shapes.push_back(shape);
            continue;
        }

        ScanShape& active = shapes.back();
        active.bbox = uniteRect(active.bbox, rect.rect);
        appendUnique(active.ownerInstanceIds, rect.instanceId);
        appendUnique(active.ownerShapeIds, rect.shapeId);
        active.containsCandidate = active.containsCandidate || rect.isCandidate;
    }
    return shapes;
}

std::vector<ImplantLayerChecker::ScanOutcome>
ImplantLayerChecker::scanRule(const Rule& rule, const ScanShapeVec& shapes) const
{
    std::vector<ScanOutcome> outcomes;
    if (!rule.getUnsupportedClauses().empty()) {
        ScanOutcome outcome;
        outcome.ruleId = rule.getRuleId();
        outcome.ruleSource = rule.getSource();
        outcome.status = OutcomeStatus::Skipped;
        outcomes.push_back(outcome);
        return outcomes;
    }

    for (const ScanShape& target : shapes) {
        if (!target.containsCandidate || target.layer != rule.getPrimaryLayer()) {
            continue;
        }
        for (Relationship relationship : {Relationship::IntraRow,
                                          Relationship::InterRow}) {
            if (!ruleAppliesTo(rule, relationship)) {
                continue;
            }

            ScanShape checkTarget = target;
            if (isWidthRule(rule.getSource()) &&
                relationship == Relationship::InterRow) {
                for (const ScanShape& sameRowNeighbor :
                     scanNeighbors(target,
                                   rule,
                                   Relationship::IntraRow,
                                   shapes)) {
                    if (target.layer == sameRowNeighbor.layer &&
                        touchesOrOverlaps(xOf(checkTarget.bbox),
                                          xOf(sameRowNeighbor.bbox))) {
                        checkTarget.bbox =
                            uniteRect(checkTarget.bbox, sameRowNeighbor.bbox);
                        for (InstanceId id : sameRowNeighbor.ownerInstanceIds) {
                            appendUnique(checkTarget.ownerInstanceIds, id);
                        }
                        for (ShapeId id : sameRowNeighbor.ownerShapeIds) {
                            appendUnique(checkTarget.ownerShapeIds, id);
                        }
                    }
                }
            }

            const std::vector<ScanShape> neighbors =
                scanNeighbors(checkTarget, rule, relationship, shapes);
            if (isWidthRule(rule.getSource()) && neighbors.empty() &&
                relationship == Relationship::IntraRow) {
                ScanOutcome outcome;
                outcome.ruleId = rule.getRuleId();
                outcome.ruleSource = rule.getSource();
                outcome.primaryLayer = rule.getPrimaryLayer();
                outcome.secondaryLayer = rule.getSecondaryLayer();
                outcome.relationship = relationship;
                outcome.targetShapeId = checkTarget.shapeId;
                outcome.xWindow = xOf(checkTarget.bbox);
                outcome.measuredValue =
                    checkTarget.bbox.xh - checkTarget.bbox.xl;
                outcome.requiredValue = rule.getMinValue();
                outcome.status = outcome.measuredValue < rule.getMinValue()
                                     ? OutcomeStatus::Violated
                                     : OutcomeStatus::Satisfied;
                outcome.instanceIds = checkTarget.ownerInstanceIds;
                outcome.shapeIds = checkTarget.ownerShapeIds;
                outcome.rowIds = {checkTarget.rowId};
                outcomes.push_back(outcome);
                continue;
            }

            for (const ScanShape& neighbor : neighbors) {
                ScanOutcome outcome;
                outcome.ruleId = rule.getRuleId();
                outcome.ruleSource = rule.getSource();
                outcome.primaryLayer = rule.getPrimaryLayer();
                outcome.secondaryLayer = rule.getSecondaryLayer();
                outcome.relationship = relationship;
                outcome.targetShapeId = checkTarget.shapeId;
                outcome.neighborShapeId = neighbor.shapeId;
                outcome.xWindow =
                    unite(xOf(checkTarget.bbox), xOf(neighbor.bbox));
                outcome.requiredValue = rule.getMinValue();
                outcome.instanceIds = checkTarget.ownerInstanceIds;
                outcome.shapeIds = checkTarget.ownerShapeIds;
                outcome.rowIds = {checkTarget.rowId};
                for (InstanceId id : neighbor.ownerInstanceIds) {
                    appendUnique(outcome.instanceIds, id);
                }
                for (ShapeId id : neighbor.ownerShapeIds) {
                    appendUnique(outcome.shapeIds, id);
                }
                appendUnique(outcome.rowIds, neighbor.rowId);

                const Dbu projected =
                    prl(xOf(checkTarget.bbox), xOf(neighbor.bbox));
                if (rule.getExceptAbutted() && projected == 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.getExceptCornerTouch() &&
                    relationship == Relationship::InterRow &&
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
                        relationship == Relationship::InterRow;
                    const bool lef58HorizontalSpacing =
                        rule.getSource() == RuleSource::Lef58Spacing &&
                        relationship == Relationship::IntraRow;
                    if (!lef58HorizontalSpacing && *rule.getPrl() >= 0 &&
                        projected <= *rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (lef58VerticalSpacing && *rule.getPrl() < 0 &&
                        spacing(xOf(checkTarget.bbox), xOf(neighbor.bbox)) >=
                            -*rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (!lef58HorizontalSpacing && !lef58VerticalSpacing &&
                        *rule.getPrl() < 0 &&
                        spacing(xOf(target.bbox), xOf(neighbor.bbox)) >
                            -*rule.getPrl()) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                }

                if (isWidthRule(rule.getSource())) {
                    XInterval effective = xOf(checkTarget.bbox);
                    if (relationship == Relationship::InterRow) {
                        if (!overlaps(xOf(checkTarget.bbox),
                                      xOf(neighbor.bbox))) {
                            outcome.status = OutcomeStatus::NotApplicable;
                            outcomes.push_back(outcome);
                            continue;
                        }
                        effective = intersect(xOf(checkTarget.bbox),
                                              xOf(neighbor.bbox));
                    } else if (touchesOrOverlaps(xOf(checkTarget.bbox),
                                                 xOf(neighbor.bbox))) {
                        effective = unite(xOf(checkTarget.bbox),
                                          xOf(neighbor.bbox));
                    }
                    outcome.xWindow = effective;
                    if (rule.getCheckGroup() &&
                        !scanGroupFails(rule,
                                        checkTarget,
                                        neighbor,
                                        relationship,
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
                        touchesOrOverlaps(xOf(checkTarget.bbox),
                                          xOf(neighbor.bbox))) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    outcome.measuredValue =
                        rule.getSource() == RuleSource::Lef58Spacing &&
                                relationship == Relationship::InterRow
                            ? ADJACENT_ROW_VERTICAL_SPACING
                            : spacing(xOf(checkTarget.bbox),
                                      xOf(neighbor.bbox));
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

std::vector<ImplantLayerChecker::ScanShape>
ImplantLayerChecker::scanNeighbors(const ScanShape& target,
    const Rule& rule, Relationship relationship, const ScanShapeVec& shapes) const
{
    std::vector<ScanShape> neighbors;
    const Dbu radius = queryRadius(rule);
    const XInterval queryWindow{target.bbox.xl - radius,
                                target.bbox.xh + radius};
    const LayerId queryLayer = rule.getSecondaryLayer().value_or(rule.getPrimaryLayer());

    std::vector<SlotRef> slots;
        if (relationship == Relationship::IntraRow) {
        slots.push_back({target.rowId, target.bandSlot});
    } else {
        const RowId neighborRow = target.bandSlot == BandSlot::Top
                                      ? target.rowId + 1
                                      : target.rowId - 1;
        const auto active =
            tracks_.activeInterRowKind(target.rowId, neighborRow);
        const auto layerIt = ruleIndex_.layers.find(target.layer);
        if (active && layerIt != ruleIndex_.layers.end() &&
            layerIt->second.getPolar() != *active) {
            return neighbors;
        }
        slots = tracks_.adjacentSlots(target.rowId, target.bandSlot);
        // Also search the same band slot in the adjacent row
        // for layers that do not alternate (non-track layers).
        bool alreadyPresent = false;
        for (const auto& s : slots) {
            if (s.rowId == neighborRow && s.bandSlot == target.bandSlot) {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent) {
            slots.push_back({neighborRow, target.bandSlot});
        }
    }

    const bool returnSameRowRuns =
        isWidthRule(rule.getSource()) && relationship == Relationship::InterRow;

    for (const ScanShape& shape : shapes) {
        if (shape.shapeId == target.shapeId) {
            continue;
        }
        if (shape.layer != queryLayer) {
            continue;
        }
        if (shape.bbox.xh < queryWindow.xl ||
            shape.bbox.xl > queryWindow.xh) {
            continue;
        }
        bool slotMatches = false;
        for (SlotRef slot : slots) {
            if (shape.rowId == slot.rowId &&
                shape.bandSlot == slot.bandSlot) {
                slotMatches = true;
                break;
            }
        }
        if (!slotMatches) {
            continue;
        }
        if (relationship == Relationship::IntraRow &&
            isWidthRule(rule.getSource()) &&
            !touchesOrOverlaps(xOf(target.bbox), xOf(shape.bbox))) {
            continue;
        }
        neighbors.push_back(shape);
    }

    if (!returnSameRowRuns) {
        return neighbors;
    }

    std::sort(neighbors.begin(),
              neighbors.end(),
              [](const ScanShape& left, const ScanShape& right) {
                  if (left.rowId != right.rowId) {
                      return left.rowId < right.rowId;
                  }
                  if (left.bandSlot != right.bandSlot) {
                      return left.bandSlot < right.bandSlot;
                  }
                  if (left.layer != right.layer) {
                      return left.layer < right.layer;
                  }
                  if (left.bbox.xl != right.bbox.xl) {
                      return left.bbox.xl < right.bbox.xl;
                  }
                  return left.bbox.xh < right.bbox.xh;
              });

    std::vector<ScanShape> runs;
    for (const ScanShape& shape : neighbors) {
        if (runs.empty() ||
            runs.back().rowId != shape.rowId ||
            runs.back().bandSlot != shape.bandSlot ||
            runs.back().layer != shape.layer ||
            !touchesOrOverlaps(xOf(runs.back().bbox), xOf(shape.bbox))) {
            runs.push_back(shape);
            continue;
        }

        ScanShape& run = runs.back();
        run.bbox = uniteRect(run.bbox, shape.bbox);
        run.containsCandidate = run.containsCandidate || shape.containsCandidate;
        for (InstanceId id : shape.ownerInstanceIds) {
            appendUnique(run.ownerInstanceIds, id);
        }
        for (ShapeId id : shape.ownerShapeIds) {
            appendUnique(run.ownerShapeIds, id);
        }
    }

    runs.erase(std::remove_if(runs.begin(),
                              runs.end(),
                              [&target](const ScanShape& shape) {
                                  return !overlaps(xOf(target.bbox),
                                                   xOf(shape.bbox));
                              }),
               runs.end());
    return runs;
}

ViolationVec
ImplantLayerChecker::scanViolations(const ScanOutcomeVec& outcomes) const
{
    std::set<size_t> suppressed;
    for (size_t broadIndex = 0; broadIndex < outcomes.size(); ++broadIndex) {
        const ScanOutcome& broad = outcomes[broadIndex];
        if (broad.status != OutcomeStatus::Violated) {
            continue;
        }
        const auto broadRuleIt = ruleIndex_.ruleById.find(broad.ruleId);
        if (broadRuleIt == ruleIndex_.ruleById.end() ||
            !broadRuleIt->second.getContainmentGroup()) {
            continue;
        }
        for (const ScanOutcome& specific : outcomes) {
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
            const std::vector<int> containedByRuleIds
                = specificRule.getContainedByRuleIds();
            if (std::find(containedByRuleIds.begin(),
                          containedByRuleIds.end(),
                          broad.ruleId) == containedByRuleIds.end()) {
                continue;
            }
            if (scanContained(specific, broad)) {
                suppressed.insert(broadIndex);
                break;
            }
        }
    }

    std::vector<Violation> violations;
    for (size_t i = 0; i < outcomes.size(); ++i) {
        const ScanOutcome& outcome = outcomes[i];
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
        violation.shapeIds = outcome.shapeIds;
        violation.measuredValue = outcome.measuredValue;
        violation.requiredValue = outcome.requiredValue;
        violation.xWindow = outcome.xWindow;
        violation.relationship = outcome.relationship;
        violation.targetInterval = outcome.xWindow;
        violation.neighborInterval = outcome.xWindow;
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

// Replace one committed instance placement and refresh derived merged indexes.
UpdateResult ImplantLayerChecker::commitPlace(const CommitRequest& request)
{
    UpdateResult result;
    if (siteWidth_ <= 0 || request.place.colId < 0) {
        result.success = false;
        result.diagnostics.push_back({"placement_not_site_aligned",
            makeMessage("placement is not site-aligned for instance ",
                request.place.instanceId)});
        return result;
    }
    const OverlapInfo overlap = overlapInfo(request.place);
    if (overlap.blockingOverlap) {
        result.success = false;
        result.diagnostics.push_back(*overlap.blockingOverlap);
        return result;
    }
    std::set<BucketKey> affectedBuckets =
        bucketsForInstance(request.place.instanceId);
    for (InstanceId fillerId : overlap.removableFillers) {
        const std::set<BucketKey> fillerBuckets =
            bucketsForInstance(fillerId);
        affectedBuckets.insert(fillerBuckets.begin(), fillerBuckets.end());
        removeInstance(fillerId);
        removeFootprint(fillerId);
    }
    removeInstance(request.place.instanceId);
    removeFootprint(request.place.instanceId);
    std::vector<PlacedInterval> intervals =
        instantiate(request.place.instanceId,
                    request.place.masterId,
                    request.place.rowId,
                    request.place.colId * siteWidth_,
                    request.place.orientation,
                    false);
    insertIntervals(intervals);
    const std::set<BucketKey> newBuckets = bucketsForIntervals(intervals);
    affectedBuckets.insert(newBuckets.begin(), newBuckets.end());
    const Node* node = network_->getNode(request.place.instanceId);
    if (node) {
        insertFootprint(node, request.place.rowId, request.place.colId *
            siteWidth_, request.place.orientation);
    }
    rebuildBuckets(affectedBuckets);
    return result;
}

// Remove all committed raw and merged index references owned by one instance.
void ImplantLayerChecker::removeInstance(InstanceId instanceId)
{
    const auto found = instIntervals_.find(instanceId);
    if (found != instIntervals_.end()) {
        for (int id : found->second) {
            const auto intervalIt = intervalById_.find(id);
            if (intervalIt == intervalById_.end()) {
                continue;
            }
            const BucketKey key = bucketFor(intervalIt->second);
            auto bucketIt = rowIndex_.find(key);
            if (bucketIt != rowIndex_.end()) {
                std::vector<PlacedInterval>& intervals = bucketIt->second;
                intervals.erase(
                    std::remove_if(intervals.begin(),
                                   intervals.end(),
                                   [id](const PlacedInterval& interval) {
                                       return interval.intervalId == id;
                                   }),
                    intervals.end());
            }
            intervalById_.erase(intervalIt);
        }
    }
    instIntervals_.erase(instanceId);
}

const std::vector<std::unique_ptr<Node>>& ImplantLayerChecker::getNodes() const
{
    return network_->getNodes();
}

// Count cached committed merged shapes across all row/layer/mode buckets.
size_t ImplantLayerChecker::mergedShapeCount() const
{
    size_t count = 0;
    for (const auto& [key, shapes] : shapeIndex_) {
        count += shapes.size(); 
               }
    return count;
}

// Look up committed merged-shape metadata by id. Candidate shapes are temporary
// and are not stored in this map.
const ImplantLayerChecker::MergedShape*
ImplantLayerChecker::findShape(int mergedShapeId) const
{
    const auto found = shapeById_.find(mergedShapeId);
    if (found != shapeById_.end()) {
        return &found->second;
    }
    return nullptr;
}

std::optional<Diagnostic>
ImplantLayerChecker::overlapDiag(const Node* node,
    RowId rowId, Dbu x, std::optional<InstanceId> excludedInstanceId) const
{
    const Dbu width = node->getWidth().v;
    const Dbu height = node->getHeight().v;
    if (rowHeight_ <= 0 || width <= 0) {
        return std::nullopt;
    }

    const int rowSpan = std::max<Dbu>(
        1, (height + rowHeight_ - 1) / rowHeight_);
    const RowId rowEnd = rowId + rowSpan;
    const XInterval xInterval{x, x + width};

    if (network_) {
        for (const auto& nPtr : network_->getNodes()) {
            const Node* existingNode = nPtr.get();
            if (!existingNode) {
                continue;
            }
            const InstanceId existingId = existingNode->getId();
            if (excludedInstanceId && existingId == *excludedInstanceId) {
                continue;
            }
            const RowId existingRowId = grid_ ?
                grid_->gridSnapDownY(existingNode).v : 0;
            const Dbu existingHeight = existingNode->getHeight().v;
            const int existingRowSpan = std::max<Dbu>(
                1, (existingHeight + rowHeight_ - 1) / rowHeight_);
            const RowId existingRowEnd = existingRowId + existingRowSpan;
            if (std::max(rowId, existingRowId) >=
                std::min(rowEnd, existingRowEnd)) {
                continue;
            }

            const Dbu existingX = existingNode->getLeft().v;
            const Dbu existingWidth = existingNode->getWidth().v;
            const XInterval existingInterval{
                existingX, existingX + existingWidth};
            if (overlaps(xInterval, existingInterval)) {
                return Diagnostic{ "placement_overlap_in_input",
                    makeMessage("placement overlaps existing instance ",
                        node->getId())};
            }
        }
    }
    return std::nullopt;
}

ImplantLayerChecker::OverlapInfo
ImplantLayerChecker::overlapInfo(const CheckRequest& request) const
{
    OverlapInfo info;
    if (request.masterId < 0 ||
        request.masterId >= static_cast<MasterId>(masterItems_.size()) ||
        rowHeight_ <= 0) {
        return info;
    }
    const MasterItem& m = masterItems_[request.masterId];
    if (m.width == 0) {
        return info;
    }

    const int rowSpan = std::max<Dbu>(
        1, (m.height + rowHeight_ - 1) / rowHeight_);
    const RowId rowEnd = request.rowId + rowSpan;
    const Dbu originX = request.colId * siteWidth_;
    const XInterval xInterval{originX, originX + m.width};

    std::set<InstanceId> seen;
    for (RowId rowId = request.rowId; rowId < rowEnd; ++rowId) {
        const auto rowIt = footprintIndex_.find(rowId);
        if (rowIt == footprintIndex_.end()) {
            continue;
        }
        const std::vector<Footprint>& row = rowIt->second;
        auto first = std::lower_bound(
            row.begin(),
            row.end(),
            xInterval.xl,
            [](const Footprint& footprint, Dbu x) {
                return footprint.x.xl < x;
            });
        if (first != row.begin()) {
            --first;
        }
        for (auto it = first; it != row.end() && it->x.xl < xInterval.xh;
             ++it) {
            if (it->instanceId == request.instanceId ||
                !seen.insert(it->instanceId).second) {
                continue;
            }
            if (!overlaps(xInterval, it->x)) {
                continue;
            }
            const Node* overlapNode = network_->getNode(it->instanceId);
            if (!overlapNode) {
                continue;
            }
            if (isFillerInstance(overlapNode)) {
                info.removableFillers.insert(it->instanceId);
                continue;
            }
            info.blockingOverlap =
                Diagnostic{"placement_overlap_in_input",
                    makeMessage("placement overlaps existing instance ",
                        request.instanceId)};
            return info;
        }
    }
    return info;
}

// Validate that an instantiated interval fits the row's expected N/P slot. The
// representative layer in TrackPattern carries polarity; it is not required
// to be the exact same implant family as the interval layer.
bool ImplantLayerChecker::slotPolarityOk(
    const ImplantLayerChecker::PlacedInterval& interval) const
{
    const auto expectedLayer =
        tracks_.layerForSlot(interval.rowId, interval.bandSlot);
    if (!expectedLayer) {
        return true;
    }
    const auto expectedLayerIt = ruleIndex_.layers.find(*expectedLayer);
    const auto actualLayerIt = ruleIndex_.layers.find(interval.layer);
    return expectedLayerIt != ruleIndex_.layers.end() &&
           actualLayerIt != ruleIndex_.layers.end() &&
           expectedLayerIt->second.getPolar() == actualLayerIt->second.getPolar();
}

bool ImplantLayerChecker::isFillerInstance(
    const Node* node) const
{
    if (!node) {
        return false;
    }
    if (node->isFiller()) {
        return true;
    }
    const MasterId masterId = node->getMaster()->getId();
    if (masterId < 0 ||
        masterId >= static_cast<MasterId>(masterItems_.size())) {
        return false;
    }
    return masterItems_[masterId].isFiller;
}

DiagVec ImplantLayerChecker::validateOverlayRequest(
    const CheckRequest& request, const FillerChanges& fillerChanges) const
{
    std::vector<Diagnostic> diagnostics;
    if (siteWidth_ <= 0) {
        diagnostics.push_back({"placement_not_site_aligned",
            makeMessage("placement is not site-aligned for instance ",
                request.instanceId)});
    }
    const bool targetHasData =
        request.masterId >= 0 &&
        request.masterId < static_cast<MasterId>(masterItems_.size()) &&
        masterItems_[request.masterId].width > 0;
    if (!targetHasData) {
        diagnostics.push_back({"unknown_target_master",
            makeMessage("unknown target master ", request.masterId)});
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
        if (!isFillerInstance(fillerNode)) {
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
            (masterItems_[oldMasterId].width != masterItems_[newMasterId].width
             ||
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

bool ImplantLayerChecker::isInGuard(
    const XInterval& xWindow, const RowIdVec& rowIds,
    const Rect& guard) const
{
    if (!overlaps(xWindow, XInterval{guard.getXL().getStorage(),
        guard.getXH().getStorage()})
        &&
        !touchesOrOverlaps(xWindow, XInterval
            {guard.getXL().getStorage(), guard.getXH().getStorage()})) {
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
            ||
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
    sortUnique(violation.shapeIds);
    sortUnique(violation.mergedShapeIds);
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
    const auto expectedLayer =
        tracks_.layerForSlot(rect.rowId, rect.bandSlot);
    if (!expectedLayer) {
        return true;
    }
    const auto expectedLayerIt = ruleIndex_.layers.find(*expectedLayer);
    const auto actualLayerIt = ruleIndex_.layers.find(rect.layer);
    return expectedLayerIt != ruleIndex_.layers.end() &&
           actualLayerIt != ruleIndex_.layers.end() &&
           expectedLayerIt->second.getPolar() == actualLayerIt->second.getPolar();
}

// Decide whether a satisfied specific-rule outcome covers the same physical
// context as a broader violation candidate.
bool ImplantLayerChecker::isContainedContext(const RuleContext& specific,
    const RuleContext& broad) const
{
    if (specific.ruleKind != broad.ruleKind ||
        specific.primaryLayer != broad.primaryLayer ||
        specific.secondaryLayer != broad.secondaryLayer ||
        specific.relationship != broad.relationship ||
        specific.targetMergedShapeId != broad.targetMergedShapeId) {
        return false;
    }
    if (broad.neighborMergedShapeId &&
        specific.neighborMergedShapeId != broad.neighborMergedShapeId) {
        return false;
    }
    return specific.xWindow.xl >= broad.xWindow.xl &&
           specific.xWindow.xh <= broad.xWindow.xh;
}

bool ImplantLayerChecker::scanContained(const ScanOutcome& specific,
                                        const ScanOutcome& broad) const
{
    if (specific.ruleSource != broad.ruleSource ||
        specific.primaryLayer != broad.primaryLayer ||
        specific.secondaryLayer != broad.secondaryLayer ||
        specific.relationship != broad.relationship ||
        specific.targetShapeId != broad.targetShapeId) {
        return false;
    }
    if (broad.neighborShapeId &&
        specific.neighborShapeId != broad.neighborShapeId) {
        return false;
    }
    return specific.xWindow.xl >= broad.xWindow.xl &&
           specific.xWindow.xh <= broad.xWindow.xh;
}

// Check whether all INTERSECTLAYER dependencies cover the relevant x span.
bool ImplantLayerChecker::hasIntersectCoverage(
    const Rule& rule,
    const ImplantLayerChecker::MergedShape& target,
    const ImplantLayerChecker::MergedShape& neighbor) const
{
    // INTERSECTLAYER is modeled as requiring the gap/span between the checked
    // shapes to be fully covered by each listed layer in the same row band.
    const XInterval gap{std::min(target.x.xh, neighbor.x.xh),
                        std::max(target.x.xl, neighbor.x.xl)};
    for (LayerId layer : rule.getIntersectLayers()) {
        const BucketKey key{target.rowId, target.bandSlot, layer};
        const auto found = shapeIndex_.find(key);
        if (found == shapeIndex_.end()) {
            return false;
        }
        bool covered = false;
        for (const MergedShape& shape : found->second) {
            if (shape.x.xl <= gap.xl && shape.x.xh >= gap.xh) {
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

bool ImplantLayerChecker::scanIntersectCoverage(const Rule& rule,
    const ScanShape& target, const ScanShape& neighbor,
    const ScanShapeVec& shapes) const
{
    const XInterval gap{std::min(target.bbox.xh, neighbor.bbox.xh),
                        std::max(target.bbox.xl, neighbor.bbox.xl)};
    for (LayerId layer : rule.getIntersectLayers()) {
        bool covered = false;
        for (const ScanShape& shape : shapes) {
            if (shape.rowId == target.rowId &&
                shape.bandSlot == target.bandSlot &&
                shape.layer == layer &&
                shape.bbox.xl <= gap.xl &&
                shape.bbox.xh >= gap.xh) {
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

// checkGroup measures the merged width of all layers in the group.
bool ImplantLayerChecker::groupFails(const Rule& rule,
    const MergedShape& target, const MergedShape& neighbor,
    Relationship relationship, const XInterval& xWindow,
    const std::set<InstanceId>& excludedInstances) const
{
    if (!rule.getCheckGroup()) {
        return true;
    }
    const auto groupIt = groupIds_.find(*rule.getCheckGroup());
    if (groupIt == groupIds_.end()) {
        return false;
    }
    const GroupId groupId = groupIt->second;
    const auto layersIt = groupLayers_.find(groupId);
    if (layersIt == groupLayers_.end()) {
        return false;
    }
    const auto layerInGroup = [&](LayerId layer) {
        return std::find(layersIt->second.begin(),
                         layersIt->second.end(),
                         layer) != layersIt->second.end();
    };

    auto shapesFor = [&](RowId rowId, BandSlot bandSlot) {
        std::vector<MergedShape> shapes;
        const auto found = groupIndex_.find({rowId, bandSlot, groupId});
        if (found == groupIndex_.end()) {
            return shapes;
        }
        for (const MergedShape& shape : found->second) {
            if (!containsAnyInstance(shape.ownerInstanceIds,
                                     excludedInstances)) {
                shapes.push_back(shape);
                continue;
            }
            std::vector<PlacedInterval> remaining;
            for (int intervalId : shape.ownerIntervalIds) {
                const auto intervalIt = intervalById_.find(intervalId);
                if (intervalIt == intervalById_.end() ||
                    excludedInstances.find(intervalIt->second.instanceId) !=
                        excludedInstances.end()) {
                    continue;
                }
                remaining.push_back(intervalIt->second);
            }
            const std::vector<MergedShape> split =
                mergeGroupShapes(remaining, false);
            shapes.insert(shapes.end(), split.begin(), split.end());
        }
        return shapes;
    };

    Dbu maxWidth = 0;
    std::vector<MergedShape> targetShapes =
        shapesFor(target.rowId, target.bandSlot);
    if (layerInGroup(target.layer)) {
        targetShapes.push_back(target);
        targetShapes = mergeGroupShapes([&]() {
            std::vector<PlacedInterval> intervals;
            for (const MergedShape& shape : targetShapes) {
                PlacedInterval interval;
                interval.layer = shape.layer;
                interval.rowId = shape.rowId;
                interval.bandSlot = shape.bandSlot;
                interval.x = shape.x;
                intervals.push_back(interval);
            }
            return intervals;
        }(), false);
    }
    if (relationship == Relationship::InterRow) {
        const std::vector<MergedShape> neighborShapes =
            shapesFor(neighbor.rowId, neighbor.bandSlot);
        for (const MergedShape& left : targetShapes) {
            for (const MergedShape& right : neighborShapes) {
                if (!touchesOrOverlaps(left.x, right.x)) {
                    continue;
                }
                const XInterval common = intersect(left.x, right.x);
                maxWidth = std::max(maxWidth, common.xh - common.xl);
            }
        }
    } else {
        for (const MergedShape& shape : targetShapes) {
            if (!touchesOrOverlaps(shape.x, xWindow)) {
                continue;
            }
            maxWidth = std::max(maxWidth, shape.x.xh - shape.x.xl);
        }
    }
    return maxWidth < rule.getMinValue();
}

bool ImplantLayerChecker::scanGroupFails(const Rule& rule,
    const ScanShape& target, const ScanShape& neighbor,
    Relationship relationship, const XInterval& xWindow,
    const ScanShapeVec& shapes) const
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
        for (const ScanShape& shape : shapes) {
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
                                   ? 0
                                   : shape.ownerShapeIds.front();
            interval.layer = shape.layer;
            interval.rowId = shape.rowId;
            interval.bandSlot = shape.bandSlot;
            interval.x = xOf(shape.bbox);
            intervals.push_back(interval);
        }
        return mergeGroupShapes(intervals, false);
    };

    Dbu maxWidth = 0;
    const std::vector<MergedShape> targetShapes =
        groupShapesFor(target.rowId, target.bandSlot);
    if (relationship == Relationship::InterRow) {
        const std::vector<MergedShape> neighborShapes =
            groupShapesFor(neighbor.rowId, neighbor.bandSlot);
        for (const MergedShape& left : targetShapes) {
            for (const MergedShape& right : neighborShapes) {
                if (!touchesOrOverlaps(left.x, right.x)) {
                    continue;
                }
                const XInterval common = intersect(left.x, right.x);
                maxWidth = std::max(maxWidth, common.xh - common.xl);
            }
        }
    } else {
                for (const MergedShape& shape : targetShapes) {
            if (!touchesOrOverlaps(shape.x, xWindow)) {
                continue;
            }
            maxWidth = std::max(maxWidth, shape.x.xh - shape.x.xl);
        }
    }
    return maxWidth < rule.getMinValue();
}

// Map rule direction and ZEROPRL semantics to intra-row or inter-row checks.
bool ImplantLayerChecker::ruleAppliesTo(const Rule& rule,
    Relationship relationship) const
{
    // LEF58 spacing directions describe the spacing direction: horizontal is
    // same-row x spacing, and vertical is adjacent-row spacing gated by x PRL.
    if (rule.getSource() == RuleSource::Lef58Spacing) {
        if (rule.getDirection() == RuleDirection::Vertical) {
            return relationship == Relationship::InterRow;
        }
        return relationship == Relationship::IntraRow;
    }
    if (rule.getDirection() == RuleDirection::Horizontal) {
        return relationship == Relationship::IntraRow ||
               (isSpacingRule(rule.getSource()) &&
                relationship == Relationship::InterRow);
    }
    if (rule.getDirection() == RuleDirection::Vertical || rule.getZeroPrl()) {
        return relationship == Relationship::InterRow;
    }
    return relationship == Relationship::IntraRow ||
           relationship == Relationship::InterRow;
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

// Parse layer name to extract VT and polarity.
// Expected format: "<FAMILY>_<POLARITY>" e.g. "VTUL_N", "VTL_P", "VTH_N"
void ImplantLayerChecker::parseLayerName(const std::string& name,
    Layer::Vt& vt, Layer::Polar& polar)
{
    vt = Layer::Vt::Unknown;
    polar = Layer::Polar::N;

    auto pos = name.rfind('_');
    if (pos == std::string::npos) {
        return;
    }

    std::string famStr = name.substr(0, pos);
    std::string polStr = name.substr(pos + 1);

    polar = ((polStr == "P" || polStr == "p") ? Layer::Polar::P
                                                : Layer::Polar::N);

    if (famStr == "VTS" || famStr == "vts") {
        vt = Layer::Vt::S;
    } else if (famStr == "VTL" || famStr == "vtl") {
        vt = Layer::Vt::L;
    } else if (famStr == "VTH" || famStr == "vth") {
        vt = Layer::Vt::H;
    } else if (famStr == "VTUL" || famStr == "vtul") {
        vt = Layer::Vt::UL;
    } else {
        vt = Layer::Vt::Unknown;
    }
}

LayerId ImplantLayerChecker::findLayerId(eLIB::TechLayerRelativeID relId) const
{
    auto it = techLayerToCheckerId_.find(relId);
    return (it != techLayerToCheckerId_.end()) ? it->second : -1;
}

// Build row track pattern from rows and implant layers.
void ImplantLayerChecker::buildTrackPattern()
{
    std::vector<LayerId> nLayers, pLayers;
    for (const auto& layer : layers_) {
        if (layer.getPolar() == Layer::Polar::N) {
            nLayers.push_back(layer.getId());
        } else {
            pLayers.push_back(layer.getId());
        }
    }

    auto& pattern = tracks_;

    for (const RowId r : rows_) {
        bool isEven = ((r % 2) == 0);

        if (isEven) {
            if (!pLayers.empty())
                pattern.layerBySlot[{r, BandSlot::Bottom}] = pLayers[0];
            if (!nLayers.empty())
                pattern.layerBySlot[{r, BandSlot::Top}] = nLayers[0];
        } else {
            if (!nLayers.empty())
                pattern.layerBySlot[{r, BandSlot::Bottom}] = nLayers[0];
            if (!pLayers.empty())
                pattern.layerBySlot[{r, BandSlot::Top}] = pLayers[0];
        }

        if (r + 1 < static_cast<RowId>(rows_.size())) {
            pattern.activeKindByBoundary[{r, r + 1}] =
                isEven ? Layer::Polar::N : Layer::Polar::P;
        }
    }
}

// Rebuild master shapes into canonical band-level shapes.
// For each row the master spans, produces 2 shapes (bottom band, top band),
// each spanning the full master width with height = rowHeight / 2.
void ImplantLayerChecker::rebuildMasterShapes()
{
    for (auto& item : masterItems_) {
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
        if (numRows < 1) numRows = 1;

        // Determine the VT from raw shapes (all should have the same VT).
        Layer::Vt vt = Layer::Vt::Unknown;
        for (const auto& rs : item.rawShapes) {
            auto layerIt = std::find_if(layers_.begin(), layers_.end(),
                [&](const Layer& layer) { return layer.getId() == rs.layer; });
            if (layerIt != layers_.end()) {
                vt = layerIt->getVt();
                break;
            }
        }
        if (vt == Layer::Vt::Unknown) {
            diagnostics_.push_back({"skipped_rebuild_unknown_family",
                "skipped_rebuild_unknown_family: master " +
                std::to_string(item.masterId)});
            item.shapes = item.rawShapes;
            continue;
        }

        // Determine the base band polarity from the bottommost raw shape
        Dbu minY = std::numeric_limits<Dbu>::max();
        Layer::Polar bottomPolarity = Layer::Polar::N;
        for (const auto& rs : item.rawShapes) {
            Dbu yl = rs.rect._yl.getStorage();
            if (yl < minY) {
                minY = yl;
                auto layerIt = std::find_if(layers_.begin(), layers_.end(),
                    [&](const Layer& layer) {
                        return layer.getId() == rs.layer;
                    });
                if (layerIt != layers_.end()) {
                    bottomPolarity = layerIt->getPolar();
                }
            }
        }

        // Find N and P layer IDs for this VT.
        LayerId familyNLayer = -1;
        LayerId familyPLayer = -1;
        for (const Layer& layer : layers_) {
            if (layer.getVt() == vt) {
                if (layer.getPolar() == Layer::Polar::N) {
                    familyNLayer = layer.getId();
                } else {
                    familyPLayer = layer.getId();
                }
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
            Layer::Polar bottomBandPol;
            Layer::Polar topBandPol;
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

// Build masterItems_ (implant shapes) from Network
void ImplantLayerChecker::buildMasters()
{
    int masterCount = network_->getMasters().size();
    masterItems_.resize(masterCount);

    for (const auto& masterPtr : network_->getMasters()) {
        const Master* nm = masterPtr.get();
        if (!nm) {
            continue;
        }
        MasterId mid = nm->getId();
        uvAssert(mid >= 0 && mid < masterCount);
        const eLIB::PhysLibCell* physCell = nm->getPhysLibCell();
        uvAssert(physCell);

        auto& item = masterItems_[mid];
        item.width = physCell->getWidth().getStorage();
        item.height = physCell->getHeight().getStorage();
        item.siteHeight = physCell->getTechSite()->getHeight().getStorage();
        item.masterId = mid;
        item.isFiller = physCell->getType().isCoreFiller()
            || physCell->getType().isPadFiller();

        // Check if this master has implant shapes
        bool hasImplant = false;
        const auto& obsVec = physCell->getObstruction();
        for (const auto& obs : obsVec) {
            const auto& shapes = obs.getShapes(eUTL::PhysOrientationE::R0);
            for (const auto& [layerRelId, shapeVec] : shapes) {
                if (findLayerId(layerRelId) >= 0) {
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
        for (const auto& obs : obsVec) {
            const auto& shapes = obs.getShapes(eUTL::PhysOrientationE::R0);
            for (const auto& [layerRelId, shapeVec] : shapes) {
                LayerId checkerLayerId = findLayerId(layerRelId);
                if (checkerLayerId < 0) {
                    continue;
                }
                for (const auto& techShape : shapeVec) {
                    if (techShape.getType() != eLIB::TechShape::RECT) {
                        diagnostics_.push_back({"skipped_unsupported_geometry",
                            "skipped_unsupported_geometry: master non-RECT shape"});
                        continue;
                    }
                    const eUTL::Rect& mRect = techShape.getRect();
                    MasterShape mis;
                    mis.masterId = mid;
                    mis.shapeId = shapeId++;
                    mis.layer = checkerLayerId;
                    mis.rect = mRect;
                    item.shapes.push_back(mis);
                }
            }
        }
        item.rawShapes = item.shapes;
    }

    rebuildMasterShapes();
}

// Initialize by extracting data from UDM PhysDesMgr.
bool ImplantLayerChecker::init(PhysDesMgr* desMgr)
{
    // Reset all state
    ruleIndex_ = RuleIndex{};
    masterItems_.clear();
    groupIds_.clear();
    groupLayers_.clear();
    layerGroups_.clear();
    rowIndex_.clear();
    shapeIndex_.clear();
    groupIndex_.clear();
    footprintIndex_.clear();
    footRowsByInst_.clear();
    instIntervals_.clear();
    instShapes_.clear();
    shapeById_.clear();
    intervalById_.clear();
    diagnostics_.clear();
    layers_.clear();
    rules_.clear();
    groups_.clear();
    rows_.clear();
    tracks_ = TrackPattern{};
    rowHeight_ = 0;
    siteWidth_ = 0;
    techLayerToCheckerId_.clear();
    nextIntervalId_ = 1;
    nextMergedShapeId_ = 1;

    const eLIB::TechLib& tech = desMgr->getTopTech();

    // Step 1: Extract implant layers from TechLib
    LayerId nextLayerId = 0;
    for (const eLIB::TechLayer& layer : tech.getLayerIter()) {
        if (!layer.isImplant()) {
            continue;
        }
        Layer::Vt vt;
        Layer::Polar polar;
        parseLayerName(layer.getName(), vt, polar);
        Layer il(nextLayerId, layer.getName(), vt, polar);
        il.setTechLayerId(layer.getId().getLocalId());
        techLayerToCheckerId_[il.getTechLayerId()] = nextLayerId;
        layers_.push_back(il);
        nextLayerId++;
    }

    // Step 2: Extract simple rules from implant layers
    int nextRuleId = 0;
    for (const auto& il : layers_) {
        const eLIB::TechLayerRelativeID relId = il.getTechLayerId();
        const eLIB::TechLayer& techLayer = tech.getTechLayer(relId);
        bool hasWidth = false, hasSpacing = false;
        Dbu implantWidthVal = techLayer.getWidth().getStorage();
        if (implantWidthVal > 0) {
            Rule wRule(nextRuleId++,
                       RuleSource::Width,
                       il.getId(),
                       implantWidthVal);
            rules_.push_back(wRule);
            hasWidth = true;
        } else {
            diagnostics_.push_back({"missing_rule_parameter", "layer "
                + il.getName() + " has no WIDTH value"});
        }
        Dbu minSpacingVal = techLayer.getMinSpacing().getStorage();
        if (minSpacingVal > 0) {
            Rule sRule(nextRuleId++,
                       RuleSource::Spacing,
                       il.getId(),
                       minSpacingVal);
            rules_.push_back(sRule);
            hasSpacing = true;
        } else {
            diagnostics_.push_back({"missing_rule_parameter", "layer "
                + il.getName() + " has no SPACING value via MinSpacing"});
        }
        if (!hasWidth && !hasSpacing) {
            diagnostics_.push_back({"skipped_missing_rule_parameter",
                "skipped_missing_rule_parameter: layer " + il.getName()
                + " has no WIDTH or SPACING rule"});
        }
    }

    // todo: extract lef58_width/spacing

    // Step 3: Extract rows
    {
        RowId nextRowId = 0;
        for (const PhysRow& row : desMgr->getPhysRowIter()) {
            rows_.push_back(nextRowId++);
            if (!row.getSite().getIsPad()) {
                auto w = row.getSite().getWidth().getStorage();
                if (siteWidth_ == 0 || w < siteWidth_) {
                    siteWidth_ = w;
                }
                auto h = row.getSite().getHeight().getStorage();
                if (rowHeight_ == 0 || h < rowHeight_) {
                    rowHeight_ = h;
                }
            }
        }
    }

    // Step 4: Build row track pattern
    buildTrackPattern();
    buildMasters();

    // Build rules and masters
    bool ok = true;
    ok = buildRules(layers_, groups_, rules_) && ok;
    ok = buildMstIntervals() && ok;

    // Step 6: Extract and build placed instances from UDM.
    // Iterate network_->getNodes() instead of UDM hierMgr.
    {
        std::vector<std::pair<eUTL::UvDist, eUTL::UvDist>> rowYBounds;
        std::vector<eUTL::UvDist> rowOriginsX;
        for (const PhysRow& row : desMgr->getPhysRowIter()) {
            eUTL::UvDist yLo = row.getOrigin().getY();
            rowYBounds.emplace_back(yLo, yLo + row.getSite().getHeight());
            rowOriginsX.push_back(row.getOrigin().getX());
        }

        for (const auto& nodePtr : network_->getNodes()) {
            Node* node = nodePtr.get();
            const MasterId mid = node ? node->getMaster()->getId() : -1;
            if (!node || mid < 0 ||
                mid >= static_cast<MasterId>(masterItems_.size()) ||
                masterItems_[mid].width == 0) {
                continue;
            }

            const eUNL::LeafCellID lcId = node->getDbInst();
            PhysCell physCell = desMgr->getPhysCell(lcId);
            if (!physCell.isValid()) {
                continue;
            }

            eUNL::PhysObjStatus status = physCell.getStatus();
            if (status != eUNL::PhysObjStatus::PLACED &&
                status != eUNL::PhysObjStatus::LOC_FIXED) {
                diagnostics_.push_back({"skipped_phys_status",
                    "skipped_phys_status: instance "
                    + std::to_string(lcId.getIndexValue())
                    + " unsupported status"});
                continue;
            }

            eUTL::Point2D origin = physCell.getOrigin();
            eUTL::PhysOrientation orient = physCell.getOrient();

            if (orient != eUTL::PhysOrientationE::R0 &&
                orient != eUTL::PhysOrientationE::MX &&
                orient != eUTL::PhysOrientationE::MY &&
                orient != eUTL::PhysOrientationE::R180) {
                diagnostics_.push_back({"skipped_unsupported_geometry",
                    "skipped_unsupported_geometry: instance "
                    + std::to_string(lcId.getIndexValue())
                    + " unsupported orientation"});
                continue;
            }

            eUTL::UvDist y = origin.getY();
            RowId rowId = -1;
            for (size_t ri = 0; ri < rowYBounds.size(); ++ri) {
                if (y >= rowYBounds[ri].first && y < rowYBounds[ri].second) {
                    rowId = static_cast<RowId>(ri);
                    break;
                }
            }
            if (rowId < 0) {
                diagnostics_.push_back({"placement_not_site_aligned",
                    "placement_not_site_aligned: instance "
                    + std::to_string(lcId.getIndexValue())
                    + " not in any row"});
                continue;
            }
            if (siteWidth_ <= 0) {
                diagnostics_.push_back({"missing_site_width",
                    "missing site width"});
                continue;
            }

            eUTL::UvDist x = origin.getX();
            eUTL::UvDist rowOriginX = rowOriginsX[rowId];
            Dbu xOffset = (x - rowOriginX).getStorage();
            if (xOffset < 0) {
                diagnostics_.push_back({"placement_not_site_aligned",
                    "placement_not_site_aligned: instance "
                    + std::to_string(lcId.getIndexValue())
                    + " x before row origin"});
                continue;
            }
            if (xOffset % siteWidth_ != 0) {
                diagnostics_.push_back({"placement_not_site_aligned",
                    "placement_not_site_aligned: instance "
                    + std::to_string(lcId.getIndexValue())
                    + " x not site-aligned"});
            }

            const Dbu instX = static_cast<Dbu>(xOffset);
            ok = buildPlacedInst(node, rowId, instX) && ok;
        }
    }

    rebuildShapes();
    if (!ok) {
        for (auto& diag : diagnostics_) {
            std::cout << diag.status << " " << diag.message << std::endl;
        }
    }
    return ok;
}

// Print checker state summary to output stream.
void ImplantLayerChecker::printStats(std::ostream& os) const
{
    os << " Implant Layer Data Extraction Summary\n";

    os << "Implant Layers: " << layers_.size() << "\n";
    for (const auto& il : layers_) {
        os << "  LayerId=" << il.getId()
           << " name=\"" << il.getName() << "\""
           << " vt=";
        switch (il.getVt()) {
            case Layer::Vt::S:  os << "VTS"; break;
            case Layer::Vt::L:  os << "VTL"; break;
            case Layer::Vt::H:  os << "VTH"; break;
            case Layer::Vt::UL: os << "VTUL"; break;
            default:           os << "Unknown"; break;
        }
        os << " polarity="
           << (il.getPolar() == Layer::Polar::N ? "N" : "P") << "\n";
    }

    os << "Rules: " << rules_.size() << "\n";
    for (const auto& rule : rules_) {
        os << "  RuleId=" << rule.getRuleId() << " source=";
        switch (rule.getSource()) {
            case RuleSource::Width:       os << "WIDTH"; break;
            case RuleSource::Spacing:     os << "SPACING"; break;
            case RuleSource::Lef58Width:  os << "LEF58_WIDTH"; break;
            case RuleSource::Lef58Spacing: os << "LEF58_SPACING"; break;
            case RuleSource::Count:       os << "COUNT"; break;
        }
        os << " primaryLayer=" << rule.getPrimaryLayer()
           << " minValue=" << rule.getMinValue();
        if (rule.getSecondaryLayer()) os << " secondaryLayer=" << *rule.getSecondaryLayer();
        os << "\n";
    }

    os << "Implant Groups: " << groups_.size() << "\n";
    const size_t populatedMasters = std::count_if(
        masterItems_.begin(), masterItems_.end(),
        [](const MasterItem& item) { return item.width > 0; });
    os << "Masters with Implant Shapes: " << populatedMasters << "\n";
    for (const auto& m : masterItems_) {
        if (m.width == 0) continue;
        os << "  MasterId=" << m.masterId
           << " width=" << m.width << " height=" << m.height
           << " siteHeight=" << m.siteHeight
           << " rawShapes=" << m.rawShapes.size()
           << " rebuiltShapes=" << m.shapes.size()
           << " : <";
        for (const auto& shape : m.shapes) {
            os << "(" << shape.shapeId << ",L" << shape.layer << ")"
               << shape.rect.toString() << " ";
        }
        os << ">\n";
        if (!m.rawShapes.empty()) {
            os << "    raw: ";
            for (const auto& rs : m.rawShapes) {
                os << "(L" << rs.layer << ")" << rs.rect.toString() << " ";
            }
            os << "\n";
        }
    }

    int fillerNum = 0;
    size_t placedCount = 0;
    if (network_) {
        placedCount = network_->getNodes().size();
        for (const auto& nodePtr : network_->getNodes()) {
            const Node* node = nodePtr.get();
            if (node && node->isFiller()) fillerNum++;
        }
    }
    os << "Placed Instances: " << placedCount << " filler: " << fillerNum << "\n";
    if (network_) {
        for (const auto& nodePtr : network_->getNodes()) {
            const Node* node = nodePtr.get();
            if (!node) continue;
            const RowId rowId = grid_ ? grid_->gridSnapDownY(node).v : 0;
            const ColId colId = grid_ ? grid_->gridX(node).v : 0;
            os << "  InstanceId=" << node->getId()
               << " masterId=" << node->getMaster()->getId()
               << " coord=<" << rowId << ", " << colId << ">"
               << " orient=";
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
            os << "\n";
        }
    }
    os << "Rows: " << rows_.size() << "\n";
    os << "Row Height: " << rowHeight_ << "\n";
    os << "Site Width: " << siteWidth_ << "\n";

    os << "Row Track Pattern layerBySlot: "
       << tracks_.layerBySlot.size() << "\n";
    for (const auto& [key, layerId] : tracks_.layerBySlot) {
        auto [rowId, slot] = key;
        os << "  Row=" << rowId
           << " Slot=" << (slot == BandSlot::Bottom ? "Bottom" : "Top")
           << " -> LayerId=" << layerId << "\n";
    }

    os << "Row Track Pattern activeKindByBoundary: "
       << tracks_.activeKindByBoundary.size() << "\n";
    for (const auto& [key, pol] : tracks_.activeKindByBoundary) {
        auto [rowA, rowB] = key;
        os << "  Boundary Row(" << rowA << "," << rowB
           << ") -> " << (pol == Layer::Polar::N ? "N" : "P") << "\n";
    }

    os << "========================================\n";
}

} // namespace ipl
} // namespace dpl2
