#include "drc/ImplantLayerCheckerHelper.h"
#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"
#include <dpl2/network.h>

#include <memory>

namespace dpl2 {
namespace ipl {

ImplantLayerCheckerHelper::ImplantLayerCheckerHelper() = default;
ImplantLayerCheckerHelper::~ImplantLayerCheckerHelper() = default;

void ImplantLayerCheckerHelper::initialize(const ImplantInput& input)
{
    grid_ = std::make_unique<Grid>();
    network_ = std::make_unique<Network>();

    // Preserve input data for initChecker
    inputMasters_ = input.masters;
    inputLayers_ = input.layers;
    inputGroups_ = input.groups;
    inputRules_ = input.rules;
    inputTracks_ = input.tracks;
    inputSiteWidth_ = input.siteWidth;
    inputRowHeight_ = input.rowHeight;

    // As friend of Grid, populate its private row data directly.
    const DbuX siteWidth(input.siteWidth);
    const DbuY rowHeight(input.rowHeight);
    const GridY rowCount(static_cast<int>(input.rows.size()));
    // Compute site count from max site width
    ColId maxColId = 0;
    for (const PlacedInst& pi : input.placedInsts) {
        if (pi.colId > maxColId) {
            maxColId = pi.colId;
        }
    }
    const GridX siteCount(static_cast<int>(maxColId + 1));

    grid_->resize(rowCount);
    for (GridY y(0); y.v < rowCount.v; ++y.v) {
        grid_->resize(y, siteCount);
    }

    grid_->row_y_dbu_to_index_.clear();
    grid_->row_index_to_y_dbu_.resize(static_cast<size_t>(rowCount.v));
    grid_->row_index_to_pixel_height_.resize(static_cast<size_t>(rowCount.v));
    for (RowId rowId : input.rows) {
        const DbuY yBase(static_cast<int>(rowId) * input.rowHeight);
        grid_->row_y_dbu_to_index_[yBase] = GridY(static_cast<int>(rowId));
        grid_->row_index_to_y_dbu_[static_cast<size_t>(rowId)] = yBase;
        grid_->row_index_to_pixel_height_[static_cast<size_t>(rowId)] = DbuY(input.rowHeight);
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
        auto master = std::make_unique<Master>();
        master->setId(static_cast<int>(i));
        master->setDbMaster(LibCellID(0, static_cast<int>(i)));
        network_->addMaster(std::move(master));
    }

    // Create Network Nodes from input placedInsts
    for (const PlacedInst& pi : input.placedInsts) {
        auto node = std::make_unique<Node>();
        node->setId(pi.instanceId);
        node->setLeft(DbuX(static_cast<int>(pi.colId * input.siteWidth)));
        node->setBottom(DbuY(static_cast<int>(pi.rowId * input.rowHeight)));
        Dbu masterWidth = 100;
        if (pi.masterId >= 0 &&
            pi.masterId < static_cast<MasterId>(input.masters.size())) {
            masterWidth = input.masters[static_cast<size_t>(pi.masterId)].width;
        }
        node->setWidth(DbuX(masterWidth));
        node->setHeight(DbuY(input.rowHeight));
        node->setOrient(pi.orientation);
        node->setPlaced(true);
        node->setType(pi.isFiller ? Node::FILLER : Node::CELL);
        if (pi.masterId >= 0 &&
            pi.masterId < static_cast<MasterId>(network_->getMasters().size())) {
            node->setMaster(network_->getMasters()[static_cast<size_t>(pi.masterId)].get());
        }
        node->setDbInst(LeafCellID(0, pi.instanceId));
        network_->addNode(std::move(node));
    }
}

void ImplantLayerCheckerHelper::initChecker(ImplantLayerChecker& checker)
{
    // Set checker state that buildRules/buildMasters/buildPlacedInst depend on
    checker.layers_ = inputLayers_;
    checker.rules_ = inputRules_;
    checker.groups_ = inputGroups_;
    for (const auto& [y, gridY] : grid_->row_y_dbu_to_index_) {
        checker.rows_.push_back(static_cast<RowId>(gridY.v));
    }

    std::sort(checker.rows_.begin(), checker.rows_.end());
    checker.tracks_ = inputTracks_;
    checker.rowHeight_ = inputRowHeight_;
    checker.siteWidth_ = inputSiteWidth_;
    checker.nextIntervalId_ = 1;
    checker.nextMergedShapeId_ = 1;

    // Clear any existing checker state
    checker.ruleIndex_ = {};
    checker.masterItems_.clear();
    checker.groupIds_.clear();
    checker.groupLayers_.clear();
    checker.layerGroups_.clear();
    checker.rowIndex_.clear();
    checker.shapeIndex_.clear();
    checker.groupIndex_.clear();
    checker.footprintIndex_.clear();
    checker.footRowsByInst_.clear();
    checker.instIntervals_.clear();
    checker.instShapes_.clear();
    checker.shapeById_.clear();
    checker.intervalById_.clear();
    checker.diagnostics_.clear();

    // Populate MasterItems indexed by MasterId (aligned with Network::masters_)
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

    checker.buildRules(inputLayers_, inputGroups_, inputRules_);
    checker.buildMstIntervals();

    for (const auto& nodePtr : network_->getNodes()) {
        Node* node = nodePtr.get();
        const Dbu x = node->getLeft().v;
        // Find row from grid y
        const DbuY y = node->getBottom();
        RowId rowId = 0;
        // Look up row from grid
        auto rowIt = grid_->row_y_dbu_to_index_.find(y);
        if (rowIt != grid_->row_y_dbu_to_index_.end()) {
            rowId = static_cast<RowId>(rowIt->second.v);
        }
        checker.buildPlacedInst(node, rowId, x);
    }

    checker.rebuildShapes();
}

} // namespace ipl
} // namespace dpl2