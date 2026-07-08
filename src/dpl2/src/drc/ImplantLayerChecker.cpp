#include "drc/ImplantLayerChecker.h"

#include <algorithm>
#include <cstdlib>

#include "infrastructure/Grid.h"
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace dpl2 {
namespace ipl {

constexpr DbCoord ZERO = 0;
constexpr DbCoord ADJACENT_ROW_VERTICAL_SPACING = 1;

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

DbCoord spacing(const XInterval& left, const XInterval& right)
{
    return std::max<DbCoord>(ZERO, std::max(left.xl, right.xl) -
                                      std::min(left.xh, right.xh));
}

DbCoord prl(const XInterval& left, const XInterval& right)
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

template <typename Enum>
Enum enumValue(int value)
{
    return static_cast<Enum>(value);
}

template <typename Value>
void dumpOptional(std::ostream& out, const std::optional<Value>& value)
{
    out << value.has_value();
    if (value) {
        out << ' ' << *value;
    }
}

template <>
void dumpOptional<std::string>(std::ostream& out,
                               const std::optional<std::string>& value)
{
    out << value.has_value();
    if (value) {
        out << ' ' << std::quoted(*value);
    }
}

template <typename Value>
bool loadOptional(std::istream& in, std::optional<Value>& value)
{
    bool hasValue = false;
    if (!(in >> hasValue)) {
        return false;
    }
    if (!hasValue) {
        value.reset();
        return true;
    }
    Value parsed{};
    if (!(in >> parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

template <>
bool loadOptional<std::string>(std::istream& in,
                               std::optional<std::string>& value)
{
    bool hasValue = false;
    if (!(in >> hasValue)) {
        return false;
    }
    if (!hasValue) {
        value.reset();
        return true;
    }
    std::string parsed;
    if (!(in >> std::quoted(parsed))) {
        return false;
    }
    value = parsed;
    return true;
}

template <typename Value>
void dumpVector(std::ostream& out, const std::vector<Value>& values)
{
    out << values.size();
    for (const Value& value : values) {
        out << ' ' << value;
    }
}

template <typename Value>
bool loadVector(std::istream& in, std::vector<Value>& values)
{
    size_t count = 0;
    if (!(in >> count)) {
        return false;
    }
    values.clear();
    values.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Value value{};
        if (!(in >> value)) {
            return false;
        }
        values.push_back(value);
    }
    return true;
}

void dumpStringVector(std::ostream& out, const std::vector<std::string>& values)
{
    out << values.size();
    for (const std::string& value : values) {
        out << ' ' << std::quoted(value);
    }
}

bool loadStringVector(std::istream& in, std::vector<std::string>& values)
{
    size_t count = 0;
    if (!(in >> count)) {
        return false;
    }
    values.clear();
    values.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        std::string value;
        if (!(in >> std::quoted(value))) {
            return false;
        }
        values.push_back(value);
    }
    return true;
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

std::optional<Polarity> TrackPattern::activeInterRowKind(RowId rowA,
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

ImplantLayerChecker::ImplantLayerChecker(Grid* grid) : DRCChecker(grid)
{
    eUNL::Session& sess = eUNL::Session::getSession();
    eUNL::Design* design = sess.getCurrentDesign();
    if (design) {
        DePlace* dePlace = DePlace::get();
        eUNL::PhysDesMgr* desMgr = dePlace->getDesMgr();
        helper_ = new ImplantLayerCheckerHelper();
        helper_->init(*desMgr);
        initialize(helper_->getImplantInput());
    }
}

ImplantLayerChecker::~ImplantLayerChecker()
{
    if (helper_) {
        delete helper_;
    }
}

// Build all normalized checker state from raw technology, master, and placement
// inputs. After this point, checks use cached intervals and merged shapes.
bool ImplantLayerChecker::initialize(const ImplantInput& input)
{
    // Initialization is the only place raw LEF/DEF-like data is normalized.
    // Runtime checks operate on row-band x-interval indexes and merged shapes.
    ruleIndex_ = RuleIndex{};
    masterCache_.clear();
    masterWidths_.clear();
    masterHeights_.clear();
    masterIsFiller_.clear();
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
    instances_.clear();
    diagnostics_.clear();
    tracks_ = input.tracks;
    rowHeight_ = input.rowHeight;
    siteWidth_ = input.siteWidth;
    input_ = input;
    nextIntervalId_ = 1;
    nextMergedShapeId_ = 1;
    if (!helper_) {
        helper_ = new ImplantLayerCheckerHelper(input);
    }

    bool ok = true;
    ok = buildRules(input) && ok;
    ok = buildMasters(input) && ok;
    ok = buildPlaced(input) && ok;
    rebuildShapes();
    if (!ok) {
        for (auto& diag : diagnostics_) {
            std::cout << diag.status << " " << diag.message << std::endl;
        }
        std::cout << helper_->toString();
    }
    return ok;
}

// Dump the normalized input needed to rebuild all derived checker caches. This
// is a debug format, not a stable interchange format.
bool ImplantLayerChecker::dump(const std::string& filePath) const
{
    std::ofstream out(filePath);
    if (!out) {
        return false;
    }

    out << "ImplantLayerCheckerDump 2\n";
    out << "row_height " << input_.rowHeight << "\n";
    out << "site_width " << input_.siteWidth << "\n";

    out << "layers " << input_.layers.size() << "\n";
    for (const ImplantLayer& layer : input_.layers) {
        out << layer.id << ' ' << std::quoted(layer.name) << ' '
            << enumInt(layer.family) << ' ' << enumInt(layer.polarity) << "\n";
    }

    out << "implant_groups " << input_.groups.size() << "\n";
    for (const auto& [name, layers] : input_.groups) {
        out << std::quoted(name) << ' ';
        dumpVector(out, layers);
        out << "\n";
    }

    out << "rules " << input_.rules.size() << "\n";
    for (const Rule& rule : input_.rules) {
        out << rule.ruleId << ' ' << enumInt(rule.source) << ' '
            << rule.primaryLayer << ' ';
        dumpOptional(out, rule.secondaryLayer);
        out << ' ' << rule.minValue << ' ' << enumInt(rule.direction) << ' ';
        dumpOptional(out, rule.prl);
        out << ' ' << rule.zeroPrl << ' ' << rule.exceptAbutted << ' '
            << rule.exceptCornerTouch << ' ';
        dumpOptional(out, rule.length);
        out << ' ';
        dumpOptional(out, rule.checkGroup);
        out << ' ';
        dumpVector(out, rule.intersectLayers);
        out << ' ';
        dumpStringVector(out, rule.unsupportedClauses);
        out << ' ';
        dumpOptional(out, rule.containmentGroup);
        out << ' ';
        dumpVector(out, rule.containedByRuleIds);
        out << ' ' << rule.specificityRank << "\n";
    }

    out << "masters " << input_.masters.size() << "\n";
    for (const MasterInput& master : input_.masters) {
        out << master.masterId << ' ' << master.width << ' ' << master.height
            << ' ' << master.isFiller << ' ' << master.shapes.size() << "\n";
        for (const MasterShape& shape : master.shapes) {
            out << shape.masterId << ' ' << shape.shapeId << ' ' << shape.layer
                << ' ' << shape.rect._xl.getStorage()
                << ' ' << shape.rect._yl.getStorage()
                << ' ' << shape.rect._xh.getStorage()
                << ' ' << shape.rect._yh.getStorage() << "\n";
        }
    }

    out << "placed " << input_.placedInsts.size() << "\n";
    for (const PlacedInst& instance : input_.placedInsts) {
        const DbCoord x = instance.columnId * input_.siteWidth;
        out << instance.instanceId << ' ' << instance.masterId << ' '
            << instance.rowId << ' ' << x << ' '
            << enumInt(instance.orientation) << ' ' << instance.isFiller
            << "\n";
    }

    out << "rows " << input_.rows.size() << "\n";
    for (const RowInput& row : input_.rows) {
        out << row.rowId << "\n";
    }

    out << "slot_layers " << input_.tracks.layerBySlot.size() << "\n";
    for (const auto& [slot, layer] : input_.tracks.layerBySlot) {
        out << slot.first << ' ' << enumInt(slot.second) << ' ' << layer << "\n";
    }

    out << "active_kinds "
        << input_.tracks.activeKindByBoundary.size() << "\n";
    for (const auto& [boundary, polarity] :
         input_.tracks.activeKindByBoundary) {
        out << boundary.first << ' ' << boundary.second << ' '
            << enumInt(polarity) << "\n";
    }
    return true;
}

bool ImplantLayerChecker::load(const std::string& filePath)
{
    std::ifstream file(filePath);
    if (!file) {
        return false;
    }

    std::ostringstream filtered;
    std::string line;
    while (std::getline(file, line)) {
        const size_t first = line.find_first_not_of(" \t\r\n");
        if (first != std::string::npos && line[first] == '#') {
            continue;
        }
        filtered << line << '\n';
    }
    std::istringstream in(filtered.str());

    std::string tag;
    int version = 0;
    if (!(in >> tag >> version) || tag != "ImplantLayerCheckerDump" ||
        (version != 1 && version != 2)) {
        return false;
    }

    ImplantInput input;
    std::string section;

    if (!(in >> section >> input.rowHeight) || section != "row_height") {
        return false;
    }
    if (!(in >> section >> input.siteWidth) || section != "site_width") {
        return false;
    }

    size_t count = 0;
    if (!(in >> section >> count) || section != "layers") {
        return false;
    }
    input.layers.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ImplantLayer layer;
        int family = 0;
        int polarity = 0;
        if (!(in >> layer.id >> std::quoted(layer.name) >> family >> polarity)) {
            return false;
        }
        layer.family = enumValue<Family>(family);
        layer.polarity = enumValue<Polarity>(polarity);
        input.layers.push_back(layer);
    }

    if (!(in >> section >> count) || section != "implant_groups") {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        std::string name;
        std::vector<LayerId> layers;
        if (!(in >> std::quoted(name)) || !loadVector(in, layers)) {
            return false;
        }
        input.groups[name] = layers;
    }

    if (!(in >> section >> count) || section != "rules") {
        return false;
    }
    input.rules.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Rule rule;
        int source = 0;
        int direction = 0;
        if (!(in >> rule.ruleId >> source >> rule.primaryLayer) ||
            !loadOptional(in, rule.secondaryLayer) ||
            !(in >> rule.minValue >> direction) ||
            !loadOptional(in, rule.prl) ||
            !(in >> rule.zeroPrl >> rule.exceptAbutted >>
              rule.exceptCornerTouch) ||
            !loadOptional(in, rule.length) ||
            !loadOptional(in, rule.checkGroup) ||
            !loadVector(in, rule.intersectLayers)) {
            return false;
        }
        if (version == 1) {
            int ignoredMergeMode = 0;
            if (!(in >> ignoredMergeMode)) {
                return false;
            }
        }
        if (!loadStringVector(in, rule.unsupportedClauses) ||
            !loadOptional(in, rule.containmentGroup) ||
            !loadVector(in, rule.containedByRuleIds) ||
            !(in >> rule.specificityRank)) {
            return false;
        }
        rule.source = enumValue<RuleSource>(source);
        rule.direction = enumValue<RuleDirection>(direction);
        input.rules.push_back(rule);
    }

    if (!(in >> section >> count) || section != "masters") {
        return false;
    }
    input.masters.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        MasterInput master;
        size_t shapeCount = 0;
        bool isFiller = false;
        if (!(in >> master.masterId >> master.width >> master.height >>
              isFiller >> shapeCount)) {
            return false;
        }
        master.isFiller = isFiller;
        master.shapes.reserve(shapeCount);
        for (size_t j = 0; j < shapeCount; ++j) {
            MasterShape shape;
            int32_t xl = 0, yl = 0, xh = 0, yh = 0;
            if (!(in >> shape.masterId >> shape.shapeId >> shape.layer >>
                  xl >> yl >> xh >> yh)) {
                return false;
            }
            shape.rect._xl = UvDist(xl);
            shape.rect._yl = UvDist(yl);
            shape.rect._xh = UvDist(xh);
            shape.rect._yh = UvDist(yh);
            master.shapes.push_back(shape);
        }
        input.masters.push_back(master);
    }

    if (!(in >> section >> count) || section != "placed") {
        return false;
    }
    input.placedInsts.reserve(count);
    std::vector<InstanceId> offSiteInstances;
    for (size_t i = 0; i < count; ++i) {
        PlacedInst instance;
        int orientation = 0;
        DbCoord x = 0;
        bool isFiller = false;
        if (!(in >> instance.instanceId >> instance.masterId >> instance.rowId >>
              x >> orientation >> isFiller)) {
            return false;
        }
        instance.isFiller = isFiller;
        if (input.siteWidth <= 0 || x % input.siteWidth != 0) {
            offSiteInstances.push_back(instance.instanceId);
        } else {
            instance.columnId = x / input.siteWidth;
        }
        instance.orientation = PhysOrientation(static_cast<PhysOrientationE>(orientation));
        input.placedInsts.push_back(instance);
    }

    if (!(in >> section >> count) || section != "rows") {
        return false;
    }
    input.rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        RowInput row;
        if (!(in >> row.rowId)) {
            return false;
        }
        input.rows.push_back(row);
    }

    if (!(in >> section >> count) || section != "slot_layers") {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        RowId rowId = 0;
        int bandSlot = 0;
        LayerId layer = 0;
        if (!(in >> rowId >> bandSlot >> layer)) {
            return false;
        }
        input.tracks.layerBySlot[{rowId, enumValue<BandSlot>(bandSlot)}] =
            layer;
    }

    if (!(in >> section >> count) || section != "active_kinds") {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        RowId rowA = 0;
        RowId rowB = 0;
        int polarity = 0;
        if (!(in >> rowA >> rowB >> polarity)) {
            return false;
        }
        input.tracks.activeKindByBoundary[{rowA, rowB}] =
            enumValue<Polarity>(polarity);
    }

    const bool initialized = initialize(input);
    if (!offSiteInstances.empty()) {
        for (InstanceId instanceId : offSiteInstances) {
            diagnostics_.push_back(
                {"placement_not_site_aligned",
                 makeMessage("placement is not site-aligned for instance ",
                             instanceId)});
        }
        return false;
    }
    return initialized;
}

// Validate and index implant layers, groups, and normalized rules. Rule ordering
// is prepared here so containment handling can reason about specificity.
bool ImplantLayerChecker::buildRules(const ImplantInput& input)
{
    bool ok = true;
    for (const ImplantLayer& layer : input.layers) {
        ruleIndex_.layers[layer.id] = layer;
    }
    ruleIndex_.groups = input.groups;

    for (Rule rule : input.rules) {
        // Keep unsupported rules visible to callers, but do not let them
        // participate in violation generation.
        if (ruleIndex_.layers.find(rule.primaryLayer) == ruleIndex_.layers.end()) {
            diagnostics_.push_back(
                {"skipped_missing_rule_parameter",
                 makeMessage("unknown primary layer for rule ", rule.ruleId)});
            ok = false;
            continue;
        }
        if (rule.secondaryLayer &&
            ruleIndex_.layers.find(*rule.secondaryLayer) == ruleIndex_.layers.end()) {
            diagnostics_.push_back(
                {"skipped_missing_rule_parameter",
                 makeMessage("unknown secondary layer for rule ", rule.ruleId)});
            ok = false;
            continue;
        }
        if (!rule.unsupportedClauses.empty()) {
            diagnostics_.push_back(
                {"skipped_unsupported_rule_clause",
                 makeMessage("unsupported LEF58 clause in rule ", rule.ruleId)});
        }
        if (rule.checkGroup) {
            const auto groupIt =
                ruleIndex_.groups.find(*rule.checkGroup);
            if (groupIt == ruleIndex_.groups.end()) {
                diagnostics_.push_back(
                    {"skipped_missing_rule_parameter",
                     "unknown implant group " + *rule.checkGroup});
            } else if (groupIds_.find(*rule.checkGroup) ==
                       groupIds_.end()) {
                const GroupId groupId =
                    static_cast<GroupId>(groupIds_.size() + 1);
                groupIds_[*rule.checkGroup] = groupId;
                groupLayers_[groupId] = groupIt->second;
                for (LayerId layer : groupIt->second) {
                    layerGroups_[layer].push_back(groupId);
                }
            }
        }
        ruleIndex_.ruleById[rule.ruleId] = rule;
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
                  if (left.specificityRank != right.specificityRank) {
                      return left.specificityRank < right.specificityRank;
                  }
                  return left.ruleId < right.ruleId;
              });
    return ok;
}

// Convert every master rectangle into row-band x intervals. This is where
// y-height geometry is reduced to row offsets and top/bottom band slots.
bool ImplantLayerChecker::buildMasters(const ImplantInput& input)
{
    bool ok = true;
    const DbCoord halfRow = rowHeight_ / 2;
    // Macro-internal pieces can be skipped for plain width/spacing after
    // row-band restructuring, but LEF58 group/intersect clauses may still need
    // them as context.
    const bool keepInternal =
        std::any_of(ruleIndex_.rules.begin(),
                    ruleIndex_.rules.end(),
                    [](const Rule& rule) {
                        return rule.checkGroup.has_value() ||
                               !rule.intersectLayers.empty();
                    });

    for (const MasterInput& master : input.masters) {
        masterWidths_[master.masterId] = master.width;
        masterHeights_[master.masterId] = master.height;
        masterIsFiller_[master.masterId] = master.isFiller;
        if (siteWidth_ <= 0 || master.width <= 0 ||
            master.width % siteWidth_ != 0) {
            diagnostics_.push_back(
                {"master_width_not_site_aligned",
                 makeMessage("master width is not site-aligned for master ",
                             master.masterId)});
            ok = false;
        }
        std::optional<Family> masterFamily;
        for (size_t i = 0; i < master.shapes.size(); ++i) {
            const MasterShape& shape = master.shapes[i];
            // Extract rect coordinates as DbCoord upfront to avoid UvDist
            // incompatibilities with DbCoord arithmetic and comparisons.
            const DbCoord shapeXl = shape.rect._xl.getStorage();
            const DbCoord shapeXh = shape.rect._xh.getStorage();
            const DbCoord shapeYl = shape.rect._yl.getStorage();
            const DbCoord shapeYh = shape.rect._yh.getStorage();

            const auto layerIt = ruleIndex_.layers.find(shape.layer);
            if (layerIt == ruleIndex_.layers.end()) {
                diagnostics_.push_back(
                    {"skipped_missing_rule_parameter",
                     makeMessage("unknown implant layer in shape ",
                                 shape.shapeId)});
                ok = false;
                continue;
            }
            if (!masterFamily) {
                masterFamily = layerIt->second.family;
            } else if (*masterFamily != layerIt->second.family) {
                diagnostics_.push_back(
                    {"master_implant_family_mismatch",
                     makeMessage("mixed implant families in master ",
                                 master.masterId)});
                ok = false;
            }
            if (shapeXl >= shapeXh || shapeYl >= shapeYh) {
                diagnostics_.push_back(
                    {"skipped_unsupported_geometry",
                     makeMessage("invalid rectangle shape ", shape.shapeId)});
                ok = false;
                continue;
            }
            if (shapeXl != 0 || shapeXh != master.width) {
                diagnostics_.push_back(
                    {"implant_shape_width_mismatch",
                     makeMessage("implant shape does not span master width ",
                                 shape.shapeId)});
                ok = false;
            }
            if (shapeXl < 0 || shapeXh > master.width ||
                shapeYl < 0 || shapeYh > master.height) {
                diagnostics_.push_back(
                    {"shape_outside_macro_boundary",
                     makeMessage("shape outside macro ", shape.shapeId)});
                ok = false;
                continue;
            }
            for (size_t j = i + 1; j < master.shapes.size(); ++j) {
                const MasterShape& other = master.shapes[j];
                if (shape.layer != other.layer) {
                    continue;
                }
                const bool xOverlap = std::max(shape.rect._xl, other.rect._xl) <
                                      std::min(shape.rect._xh, other.rect._xh);
                const bool yOverlap = std::max(shape.rect._yl, other.rect._yl) <
                                      std::min(shape.rect._yh, other.rect._yh);
                if (xOverlap && yOverlap) {
                    diagnostics_.push_back(
                        {"shape_overlap_in_input",
                         makeMessage("overlap at shape ", shape.shapeId)});
                    ok = false;
                }
            }

            const bool macroInternal = shapeXl > 0 &&
                                       shapeXh < master.width &&
                                       shapeYl > 0 &&
                                       shapeYh < master.height;

            // Restructure rectangles along the orthogonal axis into canonical
            // half-row bands. After this step the checker only stores x
            // intervals; y/height are represented by (rowOffset, bandSlot).
            DbCoord y = shapeYl;
            while (y < shapeYh) {
                const int rowOffset = static_cast<int>(y / rowHeight_);
                const DbCoord rowBase = static_cast<DbCoord>(rowOffset) * rowHeight_;
                const DbCoord bottomEnd = rowBase + halfRow;
                const BandSlot slot = y < bottomEnd ? BandSlot::Bottom
                                                    : BandSlot::Top;
                const DbCoord slotEnd = slot == BandSlot::Bottom
                                            ? bottomEnd
                                            : rowBase + rowHeight_;
                const DbCoord pieceEnd = std::min(shapeYh, slotEnd);
                if (pieceEnd <= y) {
                    break;
                }
                const bool fullSlot = y == (slot == BandSlot::Bottom ? rowBase
                                                                     : bottomEnd) &&
                                      pieceEnd == slotEnd;

                if (!fullSlot) {
                    // The current stage assumes regular implant tracks. Partial
                    // bands are still normalized, but are reported because they
                    // violate that modeling assumption.
                    diagnostics_.push_back(
                        {"unsupported_row_band_slot",
                         makeMessage("partial band slot shape ", shape.shapeId)});
                }
                MasterInterval interval;
                interval.masterId = master.masterId;
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
                    masterCache_[master.masterId].push_back(interval);
                }
                y = pieceEnd;
            }
        }
    }
    return ok;
}

// Instantiate the initially placed cells, check the no-overlap input invariant,
// and populate the committed raw interval index.
bool ImplantLayerChecker::buildPlaced(const ImplantInput& input)
{
    bool ok = true;
    for (const PlacedInst& instance : input.placedInsts) {
        const DbCoord x = instance.columnId * siteWidth_;
        if (const std::optional<Diagnostic> diagnostic =
                overlapDiag(instance.instanceId,
                            instance.masterId,
                            instance.rowId,
                            x,
                            std::nullopt)) {
            diagnostics_.push_back(*diagnostic);
            ok = false;
        }
        instances_[instance.instanceId] = instance;
        std::vector<PlacedInterval> intervals =
            instantiate(instance.instanceId,
                        instance.masterId,
                        instance.rowId,
                        x,
                        instance.orientation,
                        false);
        // Input placement is expected not to create same-layer overlaps. Check
        // that invariant early so later merge logic can stay simple.
        for (const PlacedInterval& interval : intervals) {
            if (!slotPolarityOk(interval)) {
                diagnostics_.push_back(
                    {"row_slot_polarity_mismatch",
                     makeMessage("row-slot polarity mismatch instance ",
                                 interval.instanceId)});
                ok = false;
            }
            const BucketKey key{interval.rowId, interval.bandSlot, interval.layer};
            for (const PlacedInterval& existing : rowIndex_[key]) {
                if (overlaps(existing.x, interval.x)) {
                    diagnostics_.push_back(
                        {"shape_overlap_in_input",
                         makeMessage("placed overlap instance ",
                                     interval.instanceId)});
                    ok = false;
                }
            }
        }
        insertIntervals(intervals);
        insertFootprint(instance);
    }
    return ok;
}

// Project cached master intervals into placement coordinates for either a
// committed instance or a temporary candidate.
std::vector<ImplantLayerChecker::PlacedInterval>
ImplantLayerChecker::instantiate(
    InstanceId instanceId,
    MasterId masterId,
    RowId rowId,
    DbCoord x,
    PhysOrientation orientation,
    bool isCandidate) const
{
    std::vector<PlacedInterval> intervals;
    const auto found = masterCache_.find(masterId);
    if (found == masterCache_.end()) {
        return intervals;
    }

    const DbCoord masterWidth = masterWidths_.at(masterId);
    const DbCoord masterHeight = masterHeights_.at(masterId);
    const int masterRows = static_cast<int>(masterHeight / rowHeight_);
    for (const MasterInterval& masterInterval : found->second) {
        XInterval transformed = masterInterval.x;
        // Master preprocessing encodes y as row offsets and band slots, so
        // vertical mirroring swaps those normalized fields instead of carrying
        // raw y coordinates into runtime checking.
        if (orientation == PhysOrientationE::MY ||
            orientation == PhysOrientationE::R180) {
            transformed = {masterWidth - masterInterval.x.xh,
                           masterWidth - masterInterval.x.xl};
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
void ImplantLayerChecker::insertIntervals(
    const std::vector<PlacedInterval>& intervals)
{
    for (const PlacedInterval& interval : intervals) {
        const BucketKey key = bucketFor(interval);
        rowIndex_[key].push_back(interval);
        intervalById_[interval.intervalId] = interval;
        instIntervals_[interval.instanceId].push_back(interval.intervalId);
    }
}

void ImplantLayerChecker::insertFootprint(
    const PlacedInst& instance)
{
    const auto widthIt = masterWidths_.find(instance.masterId);
    const auto heightIt = masterHeights_.find(instance.masterId);
    if (widthIt == masterWidths_.end() || heightIt == masterHeights_.end() ||
        rowHeight_ <= 0) {
        return;
    }

    const int rowSpan = std::max<DbCoord>(
        1, (heightIt->second + rowHeight_ - 1) / rowHeight_);
    const DbCoord x = instance.columnId * siteWidth_;
    const Footprint footprint{
        instance.instanceId, instance.masterId, XInterval{x, x + widthIt->second}};

    std::vector<RowId>& rows = footRowsByInst_[instance.instanceId];
    rows.clear();
    rows.reserve(static_cast<size_t>(rowSpan));
    for (int rowOffset = 0; rowOffset < rowSpan; ++rowOffset) {
        const RowId rowId = instance.rowId + rowOffset;
        rows.push_back(rowId);
        std::vector<Footprint>& row = footprintIndex_[rowId];
        const auto insertIt = std::lower_bound(
            row.begin(),
            row.end(),
            footprint.x.xl,
            [](const Footprint& left, DbCoord xLeft) {
                return left.x.xl < xLeft;
            });
        row.insert(insertIt, footprint);
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

ImplantLayerChecker::BucketKey ImplantLayerChecker::bucketFor(
    const PlacedInterval& interval) const
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
ImplantLayerChecker::bucketsForIntervals(
    const std::vector<PlacedInterval>& intervals) const
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
ImplantLayerChecker::mergeShapes(
    const std::vector<PlacedInterval>& intervals,
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
ImplantLayerChecker::mergeSortedShapes(
    const std::vector<PlacedInterval>& intervals,
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

std::vector<ImplantLayerChecker::MergedShape>
ImplantLayerChecker::mergeGroupShapes(
    const std::vector<PlacedInterval>& intervals,
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

// Adapter for the base DRCChecker interface.
bool ImplantLayerChecker::check(const Node*,
                                GridX x,
                                GridY,
                                const PhysOrientation& orient) const
{
    if (instances_.empty()) {
        return true;
    }
    const auto first = instances_.begin()->second;
    CheckRequest request;
    request.instanceId = first.instanceId;
    request.masterId = first.masterId;
    request.rowId = first.rowId;
    request.x = static_cast<DbCoord>(x.v);
    request.orientation = orient;
    return checkPlace(request).isLegal;
}

// Check one candidate placement against the committed merged-shape indexes.
CheckResult ImplantLayerChecker::checkPlace(const CheckRequest& request) const
{
    CheckResult result;
    if (siteWidth_ <= 0 || request.x % siteWidth_ != 0) {
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
        std::any_of(ruleIndex_.rules.begin(),
                    ruleIndex_.rules.end(),
                    [](const Rule& rule) {
                        return isSpacingRule(rule.source) ||
                               (isWidthRule(rule.source) && rule.zeroPrl);
                    });
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
                        request.x,
                        request.orientation,
                        true);
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
        candidateTargets = mergeShapes(targetIntervals, true);
    }

    std::vector<RuleOutcome> outcomes;
    for (const Rule& rule : ruleIndex_.rules) {
        const CheckMode ruleMode =
            sameCommittedPose && isWidthRule(rule.source) && !rule.zeroPrl
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

bool ImplantLayerChecker::isSameCommittedPose(
    const CheckRequest& request) const
{
    if (siteWidth_ <= 0) {
        return false;
    }
    const auto found = instances_.find(request.instanceId);
    if (found == instances_.end()) {
        return false;
    }
    const PlacedInst& instance = found->second;
    return instance.masterId == request.masterId &&
           instance.rowId == request.rowId &&
           instance.columnId * siteWidth_ == request.x &&
           instance.orientation == request.orientation;
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

// Check one candidate by rebuilding a complete scan snapshot from current
// committed instances. This path avoids the fast merged-shape indexes.
CheckResult ImplantLayerChecker::checkDirect(const CheckRequest& request) const
{
    CheckResult result;
    if (siteWidth_ <= 0 || request.x % siteWidth_ != 0) {
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

    const std::vector<ScanRect> snapshot =
        scanSnapshot(request, excludedInstances);
    for (const ScanRect& rect : snapshot) {
        if (rect.isCandidate && !scanSlotPolarityOk(rect)) {
            result.diagnostics = diagnostics_;
            result.diagnostics.push_back(
                {"row_slot_polarity_mismatch",
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
    result.isLegal = result.violations.empty();
    result.diagnostics = diagnostics_;
    return result;
}

// Evaluate one normalized rule against target merged shapes and collect raw
// outcomes before containment suppression.
std::vector<ImplantLayerChecker::RuleOutcome>
ImplantLayerChecker::evalRule(
    const Rule& rule,
    const std::vector<MergedShape>& targetShapes,
    CheckMode mode,
    const std::set<InstanceId>& excludedInstances) const
{
    std::vector<RuleOutcome> outcomes;
    if (!rule.unsupportedClauses.empty()) {
        RuleOutcome outcome;
        outcome.ruleId = rule.ruleId;
        outcome.status = OutcomeStatus::Skipped;
        outcomes.push_back(outcome);
        return outcomes;
    }

    for (const MergedShape& target : targetShapes) {
        if (target.layer != rule.primaryLayer) {
            continue;
        }
        std::vector<Relationship> relationships = {Relationship::IntraRow,
                                                   Relationship::InterRow};

        for (Relationship relationship : relationships) {
            if (!ruleAppliesTo(rule, relationship)) {
                continue;
            }
            MergedShape checkTarget = target;
            if (isWidthRule(rule.source) &&
                relationship == Relationship::IntraRow) {
                for (const MergedShape& sameRowNeighbor :
                     findNeighbors(target,
                                   rule,
                                   Relationship::IntraRow,
                                   mode,
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
                              excludedInstances);

            // A width rule can still even without a same-row neighbor. Inter-row
            // width is only meaningful when an adjacent row contributes a
            // merged neighbor shape, so the fallback is limited to intra-row.
            if (relationship == Relationship::IntraInstance ||
                (isWidthRule(rule.source) && neighbors.empty() &&
                 relationship == Relationship::IntraRow)) {
                if (!isWidthRule(rule.source)) {
                    continue;
                }
                RuleOutcome outcome;
                outcome.ruleId = rule.ruleId;
                outcome.context = {rule.source,
                                   rule.primaryLayer,
                                   rule.secondaryLayer,
                                   relationship,
                                   checkTarget.mergedShapeId,
                                   std::nullopt,
                                   checkTarget.rowId,
                                   checkTarget.bandSlot,
                                   checkTarget.x,
                                   checkTarget.x};
                outcome.measuredValue = checkTarget.x.xh - checkTarget.x.xl;
                outcome.requiredValue = rule.minValue;
                outcome.status = outcome.measuredValue < rule.minValue
                                     ? OutcomeStatus::Violated
                                     : OutcomeStatus::Satisfied;
                outcomes.push_back(outcome);
                continue;
            }
            for (const MergedShape& neighbor : neighbors) {
                RuleOutcome outcome;
                outcome.ruleId = rule.ruleId;
                outcome.context = {rule.source,
                                   rule.primaryLayer,
                                   rule.secondaryLayer,
                                   relationship,
                                   checkTarget.mergedShapeId,
                                   neighbor.mergedShapeId,
                                   checkTarget.rowId,
                                   checkTarget.bandSlot,
                                   unite(checkTarget.x, neighbor.x),
                                   checkTarget.x};
                outcome.requiredValue = rule.minValue;

                const DbCoord projected = prl(checkTarget.x, neighbor.x);
                // LEF58 predicates are filters on applicability. They produce
                // NotApplicable outcomes so containment and diagnostics can
                // still see that the rule was considered.
                if (rule.zeroPrl && projected != 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.source == RuleSource::Lef58Width && rule.zeroPrl &&
                    hasSameRowTouch(target,
                                    rule,
                                    excludedInstances)) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.exceptAbutted && projected == 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.exceptCornerTouch &&
                    relationship == Relationship::InterRow &&
                    projected == 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.length && rowHeight_ / 2 >= *rule.length) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (!rule.intersectLayers.empty() &&
                    !hasIntersectCoverage(rule, checkTarget, neighbor)) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.prl) {
                    const bool lef58VerticalSpacing =
                        rule.source == RuleSource::Lef58Spacing &&
                        relationship == Relationship::InterRow;
                    const bool lef58HorizontalSpacing =
                        rule.source == RuleSource::Lef58Spacing &&
                        relationship == Relationship::IntraRow;
                    if (!lef58HorizontalSpacing && *rule.prl >= 0 &&
                        projected <= *rule.prl) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (lef58VerticalSpacing && *rule.prl < 0 &&
                        spacing(checkTarget.x, neighbor.x) >=
                            -*rule.prl) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (!lef58HorizontalSpacing && !lef58VerticalSpacing &&
                        *rule.prl < 0 &&
                        spacing(target.x, neighbor.x) > -*rule.prl) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                }

                if (isWidthRule(rule.source)) {
                    XInterval effective = checkTarget.x;
                    // Same-row abutment measures the unioned run. Adjacent-row
                    // width measures only the common x-overlap at the row
                    // boundary; non-overlapping adjacent-row shapes do not form
                    // an inter-row width context unless a ZEROPRL rule
                    // explicitly asks to evaluate the zero-width touch.
                    if (relationship == Relationship::InterRow) {
                        if (!overlaps(checkTarget.x, neighbor.x) &&
                            !(rule.zeroPrl &&
                              touchesOrOverlaps(checkTarget.x,
                                                neighbor.x))) {
                            outcome.status = OutcomeStatus::NotApplicable;
                            outcomes.push_back(outcome);
                            continue;
                        }
                        effective = intersect(checkTarget.x, neighbor.x);
                    } else if (touchesOrOverlaps(checkTarget.x, neighbor.x)) {
                        effective = unite(checkTarget.x, neighbor.x);
                    }
                    outcome.context.xWindow = effective;
                    if (rule.checkGroup &&
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
                    outcome.status = outcome.measuredValue < rule.minValue
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
                        rule.source == RuleSource::Lef58Spacing &&
                                relationship == Relationship::InterRow
                            ? ADJACENT_ROW_VERTICAL_SPACING
                            : spacing(checkTarget.x, neighbor.x);
                    outcome.status = outcome.measuredValue < rule.minValue
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
    const MergedShape& target,
    const Rule& rule,
    Relationship relationship,
    CheckMode mode,
    const std::set<InstanceId>& excludedInstances) const
{
    std::vector<MergedShape> neighbors;
    const DbCoord radius = queryRadius(rule);
    const XInterval queryWindow{target.x.xl - radius, target.x.xh + radius};
    std::vector<LayerId> layers;
    layers.push_back(rule.secondaryLayer.value_or(rule.primaryLayer));

    std::vector<SlotRef> slots;
    if (relationship == Relationship::IntraInstance ||
        relationship == Relationship::IntraRow) {
        slots.push_back({target.rowId, target.bandSlot});
    } else {
        // Inter-row width and spacing use the same power-grid adjacency:
        // top-band shapes interact with the bottom band in the row above, and
        // bottom-band shapes interact with the top band in the row below.
        const auto active = tracks_.activeInterRowKind(
            target.rowId,
            target.bandSlot == BandSlot::Top ? target.rowId + 1
                                             : target.rowId - 1);
        const auto layerIt = ruleIndex_.layers.find(target.layer);
        if (active && layerIt != ruleIndex_.layers.end() &&
            layerIt->second.polarity != *active) {
            return neighbors;
        }
        slots = tracks_.adjacentSlots(target.rowId, target.bandSlot);
    }

    for (SlotRef slot : slots) {
        for (LayerId layer : layers) {
            const BucketKey key{slot.rowId, slot.bandSlot, layer};
            const auto found = shapeIndex_.find(key);
            if (found == shapeIndex_.end()) {
                continue;
            }
            const std::vector<MergedShape>& shapes = found->second;
            const auto first = std::lower_bound(
                shapes.begin(),
                shapes.end(),
                queryWindow.xl,
                [](const MergedShape& shape, DbCoord xl) {
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
                        isWidthRule(rule.source)) {
                        continue;
                    }
                    neighbors.push_back(candidateShape);
                }
            }
        }
    }
    return neighbors;
}

// Convert raw rule outcomes into final violations, suppressing broad-rule
// failures when a contained specific rule is satisfied for the same context.
std::vector<Violation> ImplantLayerChecker::makeViolations(
    const std::vector<RuleOutcome>& outcomes) const
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
            !broadRuleIt->second.containmentGroup) {
            continue;
        }
        for (const RuleOutcome& specific : outcomes) {
            if (specific.status != OutcomeStatus::Satisfied) {
                continue;
            }
            const auto specificRuleIt =
                ruleIndex_.ruleById.find(specific.ruleId);
            if (specificRuleIt == ruleIndex_.ruleById.end()) {
                continue;
            }
            const Rule& specificRule = specificRuleIt->second;
            if (specificRule.containmentGroup !=
                broadRuleIt->second.containmentGroup) {
                continue;
            }
            if (std::find(specificRule.containedByRuleIds.begin(),
                          specificRule.containedByRuleIds.end(),
                          broad.ruleId) == specificRule.containedByRuleIds.end()) {
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
        violation.ruleSource = ruleIt->second.source;
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
            }
        }
        if (!hasNeighbor) {
            violation.neighborInterval = violation.targetInterval;
        }
        // Look up layer name
        auto layerIt = ruleIndex_.layers.find(outcome.context.primaryLayer);
        if (layerIt != ruleIndex_.layers.end()) {
            violation.layerName = layerIt->second.name;
        }
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

std::vector<ImplantLayerChecker::ScanRect>
ImplantLayerChecker::scanSnapshot(const CheckRequest& request) const
{
    return scanSnapshot(request, {request.instanceId});
}

std::vector<ImplantLayerChecker::ScanRect>
ImplantLayerChecker::scanSnapshot(
    const CheckRequest& request,
    const std::set<InstanceId>& excludedInstances) const
{
    std::vector<ScanRect> snapshot;
    for (const auto& [instanceId, instance] : instances_) {
        if (excludedInstances.find(instanceId) != excludedInstances.end()) {
            continue;
        }
        std::vector<ScanRect> rects = scanInst(instance, false);
        snapshot.insert(snapshot.end(), rects.begin(), rects.end());
    }

    PlacedInst candidate;
    candidate.instanceId = request.instanceId;
    candidate.masterId = request.masterId;
    candidate.rowId = request.rowId;
    candidate.columnId = request.x / siteWidth_;
    candidate.orientation = request.orientation;
    std::vector<ScanRect> candidateRects = scanInst(candidate, true);
    snapshot.insert(snapshot.end(),
                    candidateRects.begin(),
                    candidateRects.end());

    return snapshot;
}

std::vector<ImplantLayerChecker::ScanRect>
ImplantLayerChecker::scanInst(const PlacedInst& instance,
                              bool isCandidate) const
{
    std::vector<ScanRect> rects;
    const auto masterIt = std::find_if(
        input_.masters.begin(),
        input_.masters.end(),
        [&](const MasterInput& master) {
            return master.masterId == instance.masterId;
        });
    if (masterIt == input_.masters.end() || rowHeight_ <= 0 ||
        siteWidth_ <= 0) {
        return rects;
    }
    const MasterInput& master = *masterIt;
    const DbCoord originX = instance.columnId * siteWidth_;

    for (const MasterShape& shape : master.shapes) {
        // Convert eUTL::Rect coordinates to DbCoord for checker use.
        CheckerRect local;
        local.xl = shape.rect._xl.getStorage();
        local.yl = shape.rect._yl.getStorage();
        local.xh = shape.rect._xh.getStorage();
        local.yh = shape.rect._yh.getStorage();

        if (instance.orientation == PhysOrientationE::MY ||
            instance.orientation == PhysOrientationE::R180) {
            CheckerRect mirrored;
            mirrored.xl = master.width - local.xh;
            mirrored.yl = local.yl;
            mirrored.xh = master.width - local.xl;
            mirrored.yh = local.yh;
            local = mirrored;
        }
        if (instance.orientation == PhysOrientationE::MX ||
            instance.orientation == PhysOrientationE::R180) {
            CheckerRect mirrored;
            mirrored.xl = local.xl;
            mirrored.yl = master.height - local.yh;
            mirrored.xh = local.xh;
            mirrored.yh = master.height - local.yl;
            local = mirrored;
        }

        DbCoord y = local.yl;
        while (y < local.yh) {
            const int rowOffset = static_cast<int>(y / rowHeight_);
            const DbCoord rowBase =
                static_cast<DbCoord>(rowOffset) * rowHeight_;
            const DbCoord halfRow = rowHeight_ / 2;
            const DbCoord bottomEnd = rowBase + halfRow;
            const BandSlot slot = y < bottomEnd ? BandSlot::Bottom
                                                : BandSlot::Top;
            const DbCoord slotEnd = slot == BandSlot::Bottom
                                        ? bottomEnd
                                        : rowBase + rowHeight_;
            const DbCoord pieceEnd = std::min(local.yh, slotEnd);
            if (pieceEnd <= y) {
                break;
            }
            ScanRect placed;
            placed.instanceId = instance.instanceId;
            placed.masterId = instance.masterId;
            placed.shapeId = shape.shapeId;
            placed.layer = shape.layer;
            placed.rowId = instance.rowId + rowOffset;
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
ImplantLayerChecker::scanShapes(const std::vector<ScanRect>& rects) const
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
ImplantLayerChecker::scanRule(
    const Rule& rule,
    const std::vector<ScanShape>& shapes) const
{
    std::vector<ScanOutcome> outcomes;
    if (!rule.unsupportedClauses.empty()) {
        ScanOutcome outcome;
        outcome.ruleId = rule.ruleId;
        outcome.ruleSource = rule.source;
        outcome.status = OutcomeStatus::Skipped;
        outcomes.push_back(outcome);
        return outcomes;
    }

    for (const ScanShape& target : shapes) {
        if (!target.containsCandidate || target.layer != rule.primaryLayer) {
            continue;
        }
        for (Relationship relationship : {Relationship::IntraRow,
                                          Relationship::InterRow}) {
            if (!ruleAppliesTo(rule, relationship)) {
                continue;
            }

            ScanShape checkTarget = target;
            if (isWidthRule(rule.source) &&
                relationship == Relationship::IntraRow) {
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
            if (isWidthRule(rule.source) && neighbors.empty() &&
                relationship == Relationship::IntraRow) {
                ScanOutcome outcome;
                outcome.ruleId = rule.ruleId;
                outcome.ruleSource = rule.source;
                outcome.primaryLayer = rule.primaryLayer;
                outcome.secondaryLayer = rule.secondaryLayer;
                outcome.relationship = relationship;
                outcome.targetShapeId = checkTarget.shapeId;
                outcome.xWindow = xOf(checkTarget.bbox);
                outcome.measuredValue =
                    checkTarget.bbox.xh - checkTarget.bbox.xl;
                outcome.requiredValue = rule.minValue;
                outcome.status = outcome.measuredValue < rule.minValue
                                     ? OutcomeStatus::Violated
                                     : OutcomeStatus::Satisfied;
                outcome.instanceIds = checkTarget.ownerInstanceIds;
                outcome.shapeIds = checkTarget.ownerShapeIds;
                outcomes.push_back(outcome);
                continue;
            }

            for (const ScanShape& neighbor : neighbors) {
                ScanOutcome outcome;
                outcome.ruleId = rule.ruleId;
                outcome.ruleSource = rule.source;
                outcome.primaryLayer = rule.primaryLayer;
                outcome.secondaryLayer = rule.secondaryLayer;
                outcome.relationship = relationship;
                outcome.targetShapeId = checkTarget.shapeId;
                outcome.neighborShapeId = neighbor.shapeId;
                outcome.xWindow =
                    unite(xOf(checkTarget.bbox), xOf(neighbor.bbox));
                outcome.requiredValue = rule.minValue;
                outcome.instanceIds = checkTarget.ownerInstanceIds;
                for (InstanceId id : neighbor.ownerInstanceIds) {
                    appendUnique(outcome.instanceIds, id);
                }
                for (ShapeId id : neighbor.ownerShapeIds) {
                    appendUnique(outcome.shapeIds, id);
                }

                const DbCoord projected =
                    prl(xOf(checkTarget.bbox), xOf(neighbor.bbox));
                if (rule.zeroPrl && projected != 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.source == RuleSource::Lef58Width && rule.zeroPrl &&
                    scanSameRowTouchingShape(target, rule, shapes)) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.exceptAbutted && projected == 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.exceptCornerTouch &&
                    relationship == Relationship::InterRow &&
                    projected == 0) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.length && rowHeight_ / 2 >= *rule.length) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (!rule.intersectLayers.empty() &&
                    !scanIntersectCoverage(rule,
                                           checkTarget,
                                           neighbor,
                                           shapes)) {
                    outcome.status = OutcomeStatus::NotApplicable;
                    outcomes.push_back(outcome);
                    continue;
                }
                if (rule.prl) {
                    const bool lef58VerticalSpacing =
                        rule.source == RuleSource::Lef58Spacing &&
                        relationship == Relationship::InterRow;
                    const bool lef58HorizontalSpacing =
                        rule.source == RuleSource::Lef58Spacing &&
                        relationship == Relationship::IntraRow;
                    if (!lef58HorizontalSpacing && *rule.prl >= 0 &&
                        projected <= *rule.prl) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (lef58VerticalSpacing && *rule.prl < 0 &&
                        spacing(xOf(checkTarget.bbox), xOf(neighbor.bbox)) >=
                            -*rule.prl) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                    if (!lef58HorizontalSpacing && !lef58VerticalSpacing &&
                        *rule.prl < 0 &&
                        spacing(xOf(target.bbox), xOf(neighbor.bbox)) >
                            -*rule.prl) {
                        outcome.status = OutcomeStatus::NotApplicable;
                        outcomes.push_back(outcome);
                        continue;
                    }
                }

                if (isWidthRule(rule.source)) {
                    XInterval effective = xOf(checkTarget.bbox);
                    if (relationship == Relationship::InterRow) {
                        if (!overlaps(xOf(checkTarget.bbox),
                                      xOf(neighbor.bbox)) &&
                            !(rule.zeroPrl &&
                              touchesOrOverlaps(xOf(checkTarget.bbox),
                                                xOf(neighbor.bbox)))) {
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
                    if (rule.checkGroup &&
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
                    outcome.status = outcome.measuredValue < rule.minValue
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
                        rule.source == RuleSource::Lef58Spacing &&
                                relationship == Relationship::InterRow
                            ? ADJACENT_ROW_VERTICAL_SPACING
                            : spacing(xOf(checkTarget.bbox),
                                      xOf(neighbor.bbox));
                    outcome.status = outcome.measuredValue < rule.minValue
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
ImplantLayerChecker::scanNeighbors(
    const ScanShape& target,
    const Rule& rule,
    Relationship relationship,
    const std::vector<ScanShape>& shapes) const
{
    std::vector<ScanShape> neighbors;
    const DbCoord radius = queryRadius(rule);
    const XInterval queryWindow{target.bbox.xl - radius,
                                target.bbox.xh + radius};

    const LayerId queryLayer = rule.secondaryLayer.value_or(rule.primaryLayer);

    std::vector<SlotRef> slots;
    if (relationship == Relationship::IntraRow ||
        relationship == Relationship::IntraInstance) {
        slots.push_back({target.rowId, target.bandSlot});
    } else {
        const RowId neighborRow = target.bandSlot == BandSlot::Top
                                      ? target.rowId + 1
                                      : target.rowId - 1;
        const auto active =
            tracks_.activeInterRowKind(target.rowId, neighborRow);
        const auto layerIt = ruleIndex_.layers.find(target.layer);
        if (active && layerIt != ruleIndex_.layers.end() &&
            layerIt->second.polarity != *active) {
            return neighbors;
        }
        slots = tracks_.adjacentSlots(target.rowId, target.bandSlot);
    }

    for (const ScanShape& shape : shapes) {
        if (shape.shapeId == target.shapeId || shape.containsCandidate) {
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
            isWidthRule(rule.source) &&
            !touchesOrOverlaps(xOf(target.bbox), xOf(shape.bbox))) {
            continue;
        }
        neighbors.push_back(shape);
    }
    return neighbors;
}

std::vector<Violation> ImplantLayerChecker::scanViolations(
    const std::vector<ScanOutcome>& outcomes) const
{
    std::set<size_t> suppressed;
    for (size_t broadIndex = 0; broadIndex < outcomes.size(); ++broadIndex) {
        const ScanOutcome& broad = outcomes[broadIndex];
        if (broad.status != OutcomeStatus::Violated) {
            continue;
        }
        const auto broadRuleIt = ruleIndex_.ruleById.find(broad.ruleId);
        if (broadRuleIt == ruleIndex_.ruleById.end() ||
            !broadRuleIt->second.containmentGroup) {
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
            if (specificRule.containmentGroup !=
                broadRuleIt->second.containmentGroup) {
                continue;
            }
            if (std::find(specificRule.containedByRuleIds.begin(),
                          specificRule.containedByRuleIds.end(),
                          broad.ruleId) == specificRule.containedByRuleIds.end()) {
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
            violation.layerName = layerIt->second.name;
        }
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
    if (siteWidth_ <= 0 || request.place.x % siteWidth_ != 0) {
        result.success = false;
        result.diagnostics.push_back(
            {"placement_not_site_aligned",
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
        instances_.erase(fillerId);
    }
    removeInstance(request.place.instanceId);
    removeFootprint(request.place.instanceId);
    std::vector<PlacedInterval> intervals =
        instantiate(request.place.instanceId,
                    request.place.masterId,
                    request.place.rowId,
                    request.place.x,
                    request.place.orientation,
                    false);
    insertIntervals(intervals);
    const std::set<BucketKey> newBuckets = bucketsForIntervals(intervals);
    affectedBuckets.insert(newBuckets.begin(), newBuckets.end());
    PlacedInst placed{request.place.instanceId,
                      request.place.masterId,
                      request.place.rowId,
                      request.place.x / siteWidth_,
                      request.place.orientation};
    instances_[request.place.instanceId] = placed;
    insertFootprint(placed);
    input_.placedInsts.erase(
        std::remove_if(input_.placedInsts.begin(),
                       input_.placedInsts.end(),
                       [&](const PlacedInst& instance) {
                           return instance.instanceId ==
                                  request.place.instanceId;
                       }),
        input_.placedInsts.end());
    input_.placedInsts.erase(
        std::remove_if(input_.placedInsts.begin(),
                       input_.placedInsts.end(),
                       [&](const PlacedInst& instance) {
                           return overlap.removableFillers.find(
                                      instance.instanceId) !=
                                  overlap.removableFillers.end();
                       }),
        input_.placedInsts.end());
    input_.placedInsts.push_back(placed);
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

// Return diagnostics accumulated while normalizing rules and geometry.
const std::vector<Diagnostic>& ImplantLayerChecker::initDiagnostics()
    const
{
    return diagnostics_;
}

// Expose the loaded placement list for debug replay tools.
const std::vector<PlacedInst>& ImplantLayerChecker::placedInsts()
    const
{
    return input_.placedInsts;
}

DbCoord ImplantLayerChecker::siteWidth() const
{
    return siteWidth_;
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
const ImplantLayerChecker::MergedShape* ImplantLayerChecker::findShape(
    int mergedShapeId) const
{
    const auto found = shapeById_.find(mergedShapeId);
    if (found != shapeById_.end()) {
        return &found->second;
    }
    return nullptr;
}

std::optional<Diagnostic> ImplantLayerChecker::overlapDiag(
    InstanceId instanceId,
    MasterId masterId,
    RowId rowId,
    DbCoord x,
    std::optional<InstanceId> excludedInstanceId) const
{
    const auto widthIt = masterWidths_.find(masterId);
    const auto heightIt = masterHeights_.find(masterId);
    if (widthIt == masterWidths_.end() || heightIt == masterHeights_.end() ||
        rowHeight_ <= 0) {
        return std::nullopt;
    }

    const int rowSpan = std::max<DbCoord>(
        1, (heightIt->second + rowHeight_ - 1) / rowHeight_);
    const RowId rowEnd = rowId + rowSpan;
    const XInterval xInterval{x, x + widthIt->second};

    for (const auto& [existingId, existing] : instances_) {
        if (excludedInstanceId && existingId == *excludedInstanceId) {
            continue;
        }
        const auto existingWidthIt = masterWidths_.find(existing.masterId);
        const auto existingHeightIt = masterHeights_.find(existing.masterId);
        if (existingWidthIt == masterWidths_.end() ||
            existingHeightIt == masterHeights_.end()) {
            continue;
        }
        const int existingRowSpan = std::max<DbCoord>(
            1, (existingHeightIt->second + rowHeight_ - 1) / rowHeight_);
        const RowId existingRowEnd = existing.rowId + existingRowSpan;
        if (std::max(rowId, existing.rowId) >=
            std::min(rowEnd, existingRowEnd)) {
            continue;
        }

        const DbCoord existingX = existing.columnId * siteWidth_;
        const XInterval existingInterval{
            existingX, existingX + existingWidthIt->second};
        if (overlaps(xInterval, existingInterval)) {
            return Diagnostic{
                "placement_overlap_in_input",
                makeMessage("placement overlaps existing instance ",
                            instanceId)};
        }
    }
    return std::nullopt;
}

ImplantLayerChecker::OverlapInfo
ImplantLayerChecker::overlapInfo(const CheckRequest& request) const
{
    OverlapInfo info;
    const auto widthIt = masterWidths_.find(request.masterId);
    const auto heightIt = masterHeights_.find(request.masterId);
    if (widthIt == masterWidths_.end() || heightIt == masterHeights_.end() ||
        rowHeight_ <= 0) {
        return info;
    }

    const int rowSpan = std::max<DbCoord>(
        1, (heightIt->second + rowHeight_ - 1) / rowHeight_);
    const RowId rowEnd = request.rowId + rowSpan;
    const XInterval xInterval{request.x, request.x + widthIt->second};

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
            [](const Footprint& footprint, DbCoord x) {
                return footprint.x.xh < x;
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
            const auto instIt = instances_.find(it->instanceId);
            if (instIt == instances_.end()) {
                continue;
            }
            if (isFillerInstance(instIt->second)) {
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
           expectedLayerIt->second.polarity == actualLayerIt->second.polarity;
}

bool ImplantLayerChecker::isFillerInstance(
    const PlacedInst& instance) const
{
    if (instance.isFiller) {
        return true;
    }
    const auto masterIt = masterIsFiller_.find(instance.masterId);
    return masterIt != masterIsFiller_.end() && masterIt->second;
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
           expectedLayerIt->second.polarity == actualLayerIt->second.polarity;
}

// Resolve a layer's N/P polarity from the rule index.
bool ImplantLayerChecker::layerPolarityMatches(LayerId layer,
                                               Polarity polarity) const
{
    const auto found = ruleIndex_.layers.find(layer);
    return found != ruleIndex_.layers.end() && found->second.polarity == polarity;
}

// Decide whether a satisfied specific-rule outcome covers the same physical
// context as a broader violation candidate.
bool ImplantLayerChecker::isContainedContext(
    const ImplantLayerChecker::RuleContext& specific,
    const ImplantLayerChecker::RuleContext& broad) const
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

    for (LayerId layer : rule.intersectLayers) {
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

bool ImplantLayerChecker::scanIntersectCoverage(
    const Rule& rule,
    const ScanShape& target,
    const ScanShape& neighbor,
    const std::vector<ScanShape>& shapes) const
{
    const XInterval gap{std::min(target.bbox.xh, neighbor.bbox.xh),
                        std::max(target.bbox.xl, neighbor.bbox.xl)};

    for (LayerId layer : rule.intersectLayers) {
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
bool ImplantLayerChecker::groupFails(
    const Rule& rule,
    const MergedShape& target,
    const MergedShape& neighbor,
    Relationship relationship,
    const XInterval& xWindow,
    const std::set<InstanceId>& excludedInstances) const
{
    if (!rule.checkGroup) {
        return true;
    }
    const auto groupIt = groupIds_.find(*rule.checkGroup);
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

    DbCoord maxWidth = 0;
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
    return maxWidth < rule.minValue;
}

bool ImplantLayerChecker::scanGroupFails(
    const Rule& rule,
    const ScanShape& target,
    const ScanShape& neighbor,
    Relationship relationship,
    const XInterval& xWindow,
    const std::vector<ScanShape>& shapes) const
{
    if (!rule.checkGroup) {
        return true;
    }
    const auto layersIt = ruleIndex_.groups.find(*rule.checkGroup);
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

    DbCoord maxWidth = 0;
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
    return maxWidth < rule.minValue;
}

bool ImplantLayerChecker::hasSameRowTouch(
    const MergedShape& target,
    const Rule& rule,
    const std::set<InstanceId>& excludedInstances) const
{
    const LayerId queryLayer = rule.secondaryLayer.value_or(rule.primaryLayer);
    const BucketKey key{target.rowId, target.bandSlot, queryLayer};
    const auto found = rowIndex_.find(key);
    if (found == rowIndex_.end()) {
        return false;
    }
    for (const PlacedInterval& interval : found->second) {
        if (excludedInstances.find(interval.instanceId) !=
            excludedInstances.end()) {
            continue;
        }
        if (touchesOrOverlaps(target.x, interval.x)) {
            return true;
        }
    }
    return false;
}

bool ImplantLayerChecker::scanSameRowTouchingShape(
    const ScanShape& target,
    const Rule& rule,
    const std::vector<ScanShape>& shapes) const
{
    const LayerId queryLayer = rule.secondaryLayer.value_or(rule.primaryLayer);
    for (const ScanShape& shape : shapes) {
        if (shape.containsCandidate || shape.shapeId == target.shapeId ||
            shape.rowId != target.rowId ||
            shape.bandSlot != target.bandSlot ||
            shape.layer != queryLayer) {
            continue;
        }
        if (touchesOrOverlaps(xOf(target.bbox), xOf(shape.bbox))) {
            return true;
        }
    }
    return false;
}

// Map rule direction and ZEROPRL semantics to intra-row or inter-row checks.
bool ImplantLayerChecker::ruleAppliesTo(
    const Rule& rule,
    Relationship relationship) const
{
    // LEF58 spacing directions describe the spacing direction: horizontal is
    // same-row x spacing, and vertical is adjacent-row spacing gated by X PRL.
    if (relationship == Relationship::IntraInstance) {
        return isWidthRule(rule.source);
    }
    if (rule.source == RuleSource::Lef58Spacing) {
        if (rule.direction == RuleDirection::Vertical) {
            return relationship == Relationship::InterRow;
        }
        return relationship == Relationship::IntraRow;
    }
    if (rule.direction == RuleDirection::Horizontal) {
        return relationship == Relationship::IntraRow ||
               (isSpacingRule(rule.source) &&
                relationship == Relationship::InterRow);
    }
    if (rule.direction == RuleDirection::Vertical || rule.zeroPrl) {
        return relationship == Relationship::InterRow;
    }
    return relationship == Relationship::IntraRow ||
           relationship == Relationship::InterRow;
}

// Compute the x search radius needed to find all possible neighbors for a rule.
DbCoord ImplantLayerChecker::queryRadius(const Rule& rule) const
{
    DbCoord radius = rule.minValue;
    if (rule.prl) {
        radius = std::max(radius, static_cast<DbCoord>(std::llabs(*rule.prl)));
    }
    if (rule.length) {
        radius = std::max(radius, *rule.length);
    }
    return radius;
}

// toString
std::string ImplantLayerChecker::toString(RuleSource source)
{
    switch (source) {
        case RuleSource::Width:
            return "WIDTH";
        case RuleSource::Spacing:
            return "SPACING";
        case RuleSource::Lef58Width:
            return "LEF58_WIDTH";
        case RuleSource::Lef58Spacing:
            return "LEF58_SPACING";
    }
    return "UNKNOWN";
}

std::string ImplantLayerChecker::toString(Relationship relationship)
{
    switch (relationship) {
        case Relationship::IntraInstance:
            return "intra_instance";
        case Relationship::IntraRow:
            return "intra_row";
        case Relationship::InterRow:
            return "inter_row";
    }
    return "unknown";
}

std::string Violation::toString(DbCoord siteWidth) const
{
    auto toSites = [siteWidth](DbCoord v) -> DbCoord { return v / siteWidth; };

    std::stringstream ss;
    ss << "  "
       << layerName
       << " " << ImplantLayerChecker::toString(ruleSource)
       << " " << ImplantLayerChecker::toString(relationship);
    if (!instances.empty()) {
        ss << " merged=[" << toSites(targetInterval.xl)
           << ',' << toSites(targetInterval.xh) << ']';
        if (neighborInterval.xl != targetInterval.xl ||
            neighborInterval.xh != targetInterval.xh) {
            ss << " neighbor=[" << toSites(neighborInterval.xl)
               << ',' << toSites(neighborInterval.xh) << ']';
        }
    }
    ss << " measured=" << toSites(measuredValue) << 's'
       << " required=" << toSites(requiredValue) << 's'
       << " x=[" << toSites(xWindow.xl) << ','
       << toSites(xWindow.xh) << ']';

    if (!instances.empty()) {
        ss << " neighbors=";
        for (size_t i = 0; i < instances.size(); ++i) {
            if (i != 0) {
                ss << ',';
            }
            ss << instances[i];
        }
    }
    return ss.str();
}

// ===========================================================================
// Filler VT overlay repair — spec section 5.2 overlay API (STUB).
// -----------------------------------------------------------------------------
// These are intentionally fake, per the current task: they lock the request /
// result protocol the filler repair engine depends on, without wiring the
// real DRC. A later change will route these through the actual implant check
// (reusing checkDirect / the merged-shape machinery above) so that each
// OverlayCheckRequest is applied as an atomic overlay and re-checked.
//
// Protocol honored here (spec 5.2):
//   - every CheckResult echoes its request's requestId;
//   - a request whose fillerChanges are malformed (here: the same instance
//     twice) becomes InvalidOverlay and only affects its own result;
//   - the batch form evaluates each request independently and returns results
//     in input order (repair correctness must not depend on the order).
// The stub always reports "clean" (isLegal = true, no violations) for a valid
// overlay -- it does NOT run any real rule check yet.
// ===========================================================================

CheckResult ImplantLayerChecker::checkPlaceWithOverlay(
    const OverlayCheckRequest& request) const
{
    CheckResult result;
    result.requestId = request.requestId;  // echo, always

    // Minimal validation so the InvalidOverlay path is exercised. The real
    // checker will additionally verify same-size / orientation compatibility.
    std::set<InstanceId> seen;
    for (const FillerChange& change : request.fillerChanges) {
        if (!seen.insert(change.instanceId).second) {
            result.status = CheckStatus::InvalidOverlay;
            result.isLegal = false;
            result.diagnostics.push_back(
                {"InvalidOverlay",
                 makeMessage("duplicate filler instance in overlay: ",
                             change.instanceId)});
            return result;
        }
    }

    // STUB: no real rule evaluation yet -> report the overlay as clean.
    result.status = CheckStatus::Checked;
    result.isLegal = true;
    result.diagnostics.push_back(
        {"Stub", "checkPlaceWithOverlay stub: no real DRC performed"});
    return result;
}

std::vector<CheckResult> ImplantLayerChecker::checkPlaceWithOverlays(
    const std::vector<OverlayCheckRequest>& requests) const
{
    std::vector<CheckResult> results;
    results.reserve(requests.size());
    // Each request is independent; a bad one only affects its own result.
    for (const OverlayCheckRequest& request : requests) {
        results.push_back(checkPlaceWithOverlay(request));
    }
    return results;
}

} // namespace ipl
} // namespace dpl2
