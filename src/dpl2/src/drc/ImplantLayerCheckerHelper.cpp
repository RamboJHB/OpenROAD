#include "drc/ImplantLayerCheckerHelper.h"

#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"
#include <dpl2/network.h>
#include "util/performance.hh"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

namespace dpl2 {
namespace ipl {

template <typename T>
void dumpOptional(std::ostream& os, const std::optional<T>& opt)
{
    if (opt.has_value()) {
        os << "1 " << opt.value() << ' ';
    } else {
        os << "0 ";
    }
}

void dumpOptional(std::ostream& os, const std::optional<std::string>& opt)
{
    if (opt.has_value()) {
        os << "1 " << std::quoted(opt.value()) << ' ';
    } else {
        os << "0 ";
    }
}

template <typename T>
void dumpVector(std::ostream& os, const std::vector<T>& vec)
{
    os << vec.size() << ' ';
    for (const T& v : vec) {
        os << v << ' ';
    }
}

void dumpStringVector(std::ostream& os, const std::vector<std::string>& vec)
{
    os << vec.size() << ' ';
    for (const std::string& s : vec) {
        os << std::quoted(s) << ' ';
    }
}

template <typename T>
bool loadOptional(std::istream& is, std::optional<T>& opt)
{
    int hasValue = 0;
    if (!(is >> hasValue)) {
        return false;
    }

    if (hasValue) {
        T val;
        if (!(is >> val)) {
            return false;
        }
        opt = val;
    } else {
        opt.reset();
    }
    return true;
}

bool loadOptional(std::istream& is, std::optional<std::string>& opt)
{
    int hasValue = 0;
    if (!(is >> hasValue)) {
        return false;
    }
    if (hasValue) {
        std::string val;
        if (!(is >> std::quoted(val))) {
            return false;
        }
        opt = val;
    } else {
        opt.reset();
    }
    return true;
}

template <typename T>
bool loadVector(std::istream& is, std::vector<T>& vec)
{
    size_t count = 0;
    if (!(is >> count)) {
        return false;
    }
    vec.resize(count);
    for (size_t i = 0; i < count; ++i) {
        if (!(is >> vec[i])) {
            return false;
        }
    }
    return true;
}

bool loadStringVector(std::istream& is, std::vector<std::string>& vec)
{
    size_t count = 0;
    if (!(is >> count)) {
        return false;
    }
    vec.resize(count);
    for (size_t i = 0; i < count; ++i) {
        if (!(is >> std::quoted(vec[i]))) {
            return false;
        }
    }
    return true;
}

template <typename EnumT>
int enumInt(EnumT e)
{
    return static_cast<int>(e);
}

template <typename EnumT>
EnumT enumValue(int v)
{
    return static_cast<EnumT>(v);
}

ImplantLayerCheckerHelper::ImplantLayerCheckerHelper() = default;
ImplantLayerCheckerHelper::~ImplantLayerCheckerHelper() = default;

void ImplantLayerCheckerHelper::initialize(const ImplantInput& input)
{
    eUTL::PerfLogger perfLogger("dpl2.helper.initialize", true /* singleLine */);
    grid_ = std::make_unique<Grid>();
    network_ = std::make_unique<Network>();

    // Preserve input data for initChecker
    inputMasters_ = input.masters;
    inputLayers_ = input.layers;
    inputGroups_ = input.groups;
    inputRules_ = input.rules;
    inputBasePolar_ = input.basePolar;
    inputSiteWidth_ = input.siteWidth;
    inputRowHeight_ = input.rowHeight;

    // create grid layout
    const DbuX siteWidth(input.siteWidth);
    const DbuY rowHeight(input.rowHeight);
    const GridY rowCount(input.rowCount);
    GridX siteCount = GridX(input.colCount);

    grid_->resize(rowCount);
    for (GridY y(0); y.v < rowCount.v; ++y.v) {
        grid_->resize(y, siteCount);
    }
    grid_->row_y_dbu_to_index_.clear();
    grid_->row_index_to_y_dbu_.resize(rowCount.v);
    grid_->row_index_to_pixel_height_.resize(rowCount.v);
    for (RowId rowId = 0; rowId <  input.rowCount; rowId++) {
        const DbuY yBase(rowId * input.rowHeight);
        grid_->row_y_dbu_to_index_[yBase] = GridY(rowId);
        grid_->row_index_to_y_dbu_[rowId] = yBase;
        grid_->row_index_to_pixel_height_[rowId] = DbuY(input.rowHeight);
    }
    grid_->uniform_row_height_ = DbuY(input.rowHeight);
    grid_->site_width_ = siteWidth;
    grid_->row_count_ = rowCount;
    grid_->row_site_count_ = siteCount;
    grid_->core_ = eUTL::Rect(
        eUTL::UvDist(0), eUTL::UvDist(0),
        eUTL::UvDist(siteWidth.v * siteCount.v),
        eUTL::UvDist(rowHeight.v * rowCount.v));
    grid_->desMgr_ = nullptr;
    grid_->logger_ = nullptr;

    // Create Network Masters from input masters
    for (size_t i = 0; i < input.masters.size(); ++i) {
        std::unique_ptr<Master> master = std::make_unique<Master>();
        master->setId(i);
        master->setDbMaster(LibCellID(0, static_cast<int>(i)));
        const bool isFiller = std::any_of(
            input.placedInsts.begin(), input.placedInsts.end(),
            [i](const PlacedInst& placed) {
                return placed.masterId == static_cast<MasterId>(i)
                       && placed.isFiller;
            });
        master->setFiller(isFiller);
        network_->addMaster(std::move(master));
    }

    // Create Network Nodes from input placedInsts
    for (const PlacedInst& pi : input.placedInsts) {
        std::unique_ptr<Node> nodePtr = std::make_unique<Node>();
        Node* node = nodePtr.get();
        node->setId(pi.instanceId);
        node->setLeft(DbuX(pi.colId * input.siteWidth));
        node->setBottom(DbuY(pi.rowId * input.rowHeight));
        Dbu masterWidth = input.masters[pi.masterId].width;
        node->setWidth(DbuX(masterWidth));
        node->setHeight(DbuY(input.rowHeight));
        node->setOrient(pi.orientation);
        node->setPlaced(true);
        node->setType(pi.isFiller ? Node::FILLER : Node::CELL);
        if (pi.masterId >= 0 &&
            pi.masterId < static_cast<MasterId>(network_->getMasters().size())) {
            node->setMaster(network_->getMasters()[pi.masterId].get());
        }
        node->setDbInst(LeafCellID(0, pi.instanceId));

        // mark place
        for (GridX x = grid_->gridX(node); x < grid_->gridEndX(node); x++) {
            for (GridY y = grid_->gridSnapDownY(node);
                y < grid_->gridEndY(node); y++) {
                Pixel* pixel = grid_->gridPixel(x, y);
                if (pixel == nullptr) {
                    continue;
                }
                pixel->cell = node;
            }
        }
        network_->addNode(std::move(nodePtr));
    }
    bool isFullUtil = grid_->isFullUtil();
    std::cout << "Fully utilized grid: " << isFullUtil << std::endl;
}

void ImplantLayerCheckerHelper::initChecker(ImplantLayerChecker& checker)
{
    eUTL::PerfLogger perfLogger("dpl2.helper.initChecker", true /* singleLine */);
    // Set checker state that buildRules/buildMasters/buildInst depend on
    checker.layers_ = inputLayers_;
    checker.rules_ = inputRules_;
    // [fillerRepair-fix] follow the checker rename/removal: groups_ ->
    // layerGroups_; the interval/shape id counters are gone.
    checker.layerGroups_ = inputGroups_;
    checker.basePolar_ = inputBasePolar_;
    checker.rowHeight_ = inputRowHeight_;
    checker.siteWidth_ = inputSiteWidth_;
    // This helper supplies the complete synthetic checker model without UDM.
    // Remove the production-only Grid-manager diagnostic after that setup.
    checker.diagnostics_.erase(
        std::remove_if(checker.diagnostics_.begin(), checker.diagnostics_.end(),
            [](const Diagnostic& diagnostic) {
                return diagnostic.status == "missing_grid_phys_des_mgr";
            }),
        checker.diagnostics_.end());
    checker.infrastructureReady_ = true;

    // Populate masterItems_ indexed by MasterId (aligned with Network::masters_)
    const size_t masterCount = network_->getMasters().size();
    checker.masterItems_.resize(masterCount);
    for (size_t i = 0; i < masterCount && i < inputMasters_.size(); ++i) {
        MasterItem item = inputMasters_[i];
        // Override width/height from the MasterItem (from input),
        // but also set siteHeight from rowHeight
        if (item.siteHeight == 0) {
            item.siteHeight = inputRowHeight_;
        }
        if (item.width == 0) {
            item.width = inputSiteWidth_;
        }
        if (item.height == 0) {
            item.height = inputRowHeight_;
        }
        checker.masterItems_[i] = std::move(item);
    }

    // [fillerRepair-fix] buildRules() now reads the members set above.
    checker.buildRules();
    checker.setMaxRuleValue();
    checker.buildMstIntervals();
}

bool ImplantLayerCheckerHelper::dump(const std::string& filePath,
                                     const ImplantLayerChecker& checker) const
{
    std::ofstream out(filePath);
    if (!out) {
        return false;
    }

    out << "ImplantLayerCheckerDump 1\n";
    out << "row_count " << checker.grid_->getRowCount().v << "\n";
    out << "col_count " << checker.grid_->getRowSiteCount().v << "\n";
    out << "row_height " << checker.rowHeight_ << "\n";
    out << "site_width " << checker.siteWidth_ << "\n";

    // -- layers --
    out << "layers " << checker.layers_.size() << "\n";
    for (const Layer& layer : checker.layers_) {
        out << layer.getId() << ' ' << std::quoted(layer.getName()) << ' '
            << enumInt(layer.getVt()) << ' ' << enumInt(layer.getPolar()) << "\n";
    }

    // -- groups --
    out << "implant_groups " << checker.layerGroups_.size() << "\n";
    for (const auto& [name, layers] : checker.layerGroups_) {
        out << std::quoted(name) << ' ';
        dumpVector(out, layers);
        out << "\n";
    }

    // -- rules --
    out << "rules " << checker.rules_.size() << "\n";
    for (const Rule& rule : checker.rules_) {
        out << rule.getRuleId() << ' ' << enumInt(rule.getSource()) << ' '
            << rule.getPrimaryLayer() << ' ';
        dumpOptional(out, rule.getSecondaryLayer());
        out << ' ' << rule.getMinValue() << ' ' << enumInt(rule.getDirection())
          << ' ';
        dumpOptional(out, rule.getPrl());
        out << ' ' << rule.getZeroPrl() << ' ' << rule.getExceptAbutted() << ' '
            << rule.getExceptCornerTouch() << ' ';
        dumpOptional(out, rule.getLength());
        out << ' ';
        dumpOptional(out, rule.getCheckGroup());
        out << ' ';
        dumpVector(out, rule.getIntersectLayers());
        out << ' ';
        dumpStringVector(out, rule.getUnsupportedClauses());
        out << ' ';
        dumpOptional(out, rule.getContainmentGroup());
        out << ' ';
        dumpVector(out, rule.getContainedByRuleIds());
        out << ' ' << rule.getSpecificityRank() << "\n";
    }

    // -- masters --
    out << "masters " << checker.masterItems_.size() << "\n";
    for (const MasterItem& master : checker.masterItems_) {
        out << master.masterId << ' ' << master.width << ' ' << master.height
            << ' ' << master.isFiller << ' ' << master.siteHeight << ' '
            << master.shapes.size() << "\n";
        for (const MasterShape& shape : master.shapes) {
            out << shape.masterId << ' ' << shape.shapeId << ' ' << shape.layer
                << ' ' << shape.rect.getXL().getStorage() << ' '
                << shape.rect.getYL().getStorage() << ' '
                << shape.rect.getXH().getStorage() << ' '
                << shape.rect.getYH().getStorage() << "\n";
        }
    }

    // -- placed instances (reconstructed from network nodes) --
    // Build placedInsts from checker's network nodes.
    // For the production-flow case, checker runs against a real design
    // accessed via its Network.  The PlacedInst fields (rowId, colId) are
    // derived from node coordinates.
    std::vector<PlacedInst> placedInsts;
    if (checker.network_) {
        for (const std::unique_ptr<Node>& nodePtr : checker.network_->getNodes()) {
            const Node* node = nodePtr.get();
            if (!node) {
                continue;
            }
            PlacedInst pi;
            pi.instanceId = node->getId();
            pi.masterId = node->getMaster()
                          ? node->getMaster()->getId()
                          : MasterId(-1);
            const Dbu nodeLeft = node->getLeft().v;
            const Dbu nodeBottom = node->getBottom().v;
            pi.rowId = nodeBottom / checker.rowHeight_;
            pi.colId = nodeLeft / checker.siteWidth_;
            pi.orientation = node->getOrient();
            pi.isFiller = (node->getType() == Node::FILLER);
            placedInsts.push_back(pi);
        }
    }

    out << "placed " << placedInsts.size() << "\n";
    for (const PlacedInst& inst : placedInsts) {
        out << inst.instanceId << ' ' << inst.masterId << ' '
            << inst.rowId << ' ' << inst.colId << ' '
            << enumInt(inst.orientation) << ' ' << inst.isFiller << "\n";
    }
    return true;
}

ImplantInput ImplantLayerCheckerHelper::load(const std::string& filePath)
{
    std::ifstream file(filePath);
    if (!file) {
        return ImplantInput();
    }

    // Filter out comment lines
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
        (version != 1 && version != 2 && version != 3)) {
        return ImplantInput();
    }

    ImplantInput input;
    std::string section;
    size_t count = 0;

    if (!(in >> section >> input.rowCount) || section != "row_count") {
        return ImplantInput();
    }
    if (!(in >> section >> input.colCount) || section != "col_count") {
        return ImplantInput();
    }
    if (!(in >> section >> input.rowHeight) || section != "row_height") {
        return ImplantInput();
    }
    if (!(in >> section >> input.siteWidth) || section != "site_width") {
        return ImplantInput();
    }

    // -- layers --
    if (!(in >> section >> count) || section != "layers") {
        return ImplantInput();
    }
    input.layers.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Layer layer;
        int id = 0;
        int family = 0;
        int polarity = 0;
        std::string name;
        if (!(in >> id >> std::quoted(name) >> family >> polarity)) {
            return ImplantInput();
        }
        layer.setId(id);
        layer.setName(name);
        layer.setVt(enumValue<Layer::Vt>(family));
        layer.setPolar(enumValue<Layer::Polar>(polarity));
        input.layers.push_back(layer);
    }

    // -- groups --
    if (!(in >> section >> count) || section != "implant_groups") {
        return ImplantInput();
    }
    for (size_t i = 0; i < count; ++i) {
        std::string name;
        std::vector<LayerId> layers;
        if (!(in >> std::quoted(name)) || !loadVector(in, layers)) {
            return ImplantInput();
        }
        input.groups[name] = layers;
    }

    // -- rules --
    if (!(in >> section >> count) || section != "rules") {
        return ImplantInput();
    }
    input.rules.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Rule rule;
        int ruleId = 0;
        int source = 0;
        int direction = 0;
        LayerId primaryLayer = 0;
        Dbu minValue = 0;
        bool zeroPrl = false;
        bool exceptAbutted = false;
        bool exceptCornerTouch = false;
        int specificityRank = 0;
        std::optional<LayerId> secondaryLayer;
        std::optional<Dbu> prl;
        std::optional<Dbu> length;
        std::optional<std::string> checkGroup;
        std::vector<LayerId> intersectLayers;
        if (!(in >> ruleId >> source >> primaryLayer) ||
            !loadOptional(in, secondaryLayer) ||
            !(in >> minValue >> direction) ||
            !loadOptional(in, prl) ||
            !(in >> zeroPrl >> exceptAbutted >> exceptCornerTouch) ||
            !loadOptional(in, length) ||
            !loadOptional(in, checkGroup) ||
            !loadVector(in, intersectLayers)) {
            return ImplantInput();
        }
        rule.setRuleId(ruleId);
        rule.setSource(enumValue<RuleSource>(source));
        rule.setPrimaryLayer(primaryLayer);
        rule.setSecondaryLayer(secondaryLayer);
        rule.setMinValue(minValue);
        rule.setDirection(enumValue<RuleDirection>(direction));
        rule.setPrl(prl);
        rule.setZeroPrl(zeroPrl);
        rule.setExceptAbutted(exceptAbutted);
        rule.setExceptCornerTouch(exceptCornerTouch);
        rule.setLength(length);
        rule.setCheckGroup(checkGroup);
        rule.setIntersectLayers(intersectLayers);
        std::vector<std::string> unsupportedClauses;
        std::optional<int> containmentGroup;
        std::vector<int> containedByRuleIds;
        if (!loadStringVector(in, unsupportedClauses) ||
            !loadOptional(in, containmentGroup) ||
            !loadVector(in, containedByRuleIds) ||
            !(in >> specificityRank)) {
            return ImplantInput();
        }
        rule.setUnsupportedClauses(unsupportedClauses);
        rule.setContainmentGroup(containmentGroup);
        rule.setContainedByRuleIds(containedByRuleIds);
        rule.setSpecificityRank(specificityRank);
        input.rules.push_back(rule);
    }

    // -- masters --
    if (!(in >> section >> count) || section != "masters") {
        return ImplantInput();
    }
    input.masters.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        MasterItem master;
        size_t shapeCount = 0;
        bool isFiller = false;
        Dbu siteHeight = 0;
        if (!(in >> master.masterId >> master.width >> master.height >>
            isFiller >> siteHeight >> shapeCount)) {
            return ImplantInput();
        }
        master.isFiller = isFiller;
        master.siteHeight = siteHeight;
        master.shapes.reserve(shapeCount);
        for (size_t s = 0; s < shapeCount; ++s) {
            MasterShape shape;
            Dbu xl = 0, yl = 0, xh = 0, yh = 0;
            if (!(in >> shape.masterId >> shape.shapeId >> shape.layer >>
                xl >> yl >> xh >> yh)) {
                return ImplantInput();
            }
            shape.rect = ::Rect(eUTL::UvDist(xl), eUTL::UvDist(yl),
                                eUTL::UvDist(xh), eUTL::UvDist(yh));
            master.shapes.push_back(shape);
        }
        input.masters.push_back(master);
    }

    // -- placed instances --
    if (!(in >> section >> count) || section != "placed") {
        return ImplantInput();
    }
    input.placedInsts.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        PlacedInst inst;
        int orientation = 0;
        bool isFiller = false;
        if (!(in >> inst.instanceId >> inst.masterId >>
            inst.rowId >> inst.colId >> orientation >> isFiller)) {
            return ImplantInput();
        }
        inst.orientation = enumValue<PhysOrientation>(orientation);
        inst.isFiller = isFiller;
        input.placedInsts.push_back(inst);
    }
    return input;
}

} // namespace ipl
} // namespace dpl2
