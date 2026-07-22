#pragma once

#include "drc/ImplantLayerChecker.h"

#include <memory>
#include <string>
#include <vector>

namespace dpl2 {
class Grid;
class Network;

namespace ipl {

// Backward-compatible alias for code not yet migrated to MasterItem
using MasterInput = MasterItem;

struct PlacedInst
{
    InstanceId instanceId = 0;
    MasterId masterId = 0;
    RowId rowId = 0;
    ColId colId = 0;
    PhysOrientation orientation = PhysOrientationE::R0;
    bool isFiller = false;
};

struct ImplantInput
{
    std::vector<Layer> layers;
    std::vector<Rule> rules;
    std::unordered_map<std::string, std::vector<LayerId>> groups;
    std::vector<MasterItem> masters;
    std::vector<PlacedInst> placedInsts;
    // Number of standard rows; the helper builds rows 0..rowCounts-1 with
    // y = rowId * rowHeight (previously an explicit 0..N-1 id list).
    int rowCounts = 0;
    TrackPattern tracks;
    Dbu rowHeight = 0;
    Dbu siteWidth = 0;
};

// ImplantLayerCheckerHelper is used for test flow
class ImplantLayerCheckerHelper {
public:
    ImplantLayerCheckerHelper();
    ~ImplantLayerCheckerHelper();

    void initialize(const ImplantInput& input);
    Grid* getGrid() const { return grid_.get(); }
    Network* getNetwork() const { return network_.get(); }
    void initChecker(ImplantLayerChecker& checker);

private:
    std::unique_ptr<Grid> grid_;
    std::unique_ptr<Network> network_;
    // Preserve input data needed by initChecker
    std::vector<MasterItem> inputMasters_;
    TrackPattern inputTracks_;
    std::vector<Layer> inputLayers_;
    std::unordered_map<std::string, std::vector<LayerId>> inputGroups_;
    std::vector<Rule> inputRules_;
    Dbu inputSiteWidth_ = 0;
    Dbu inputRowHeight_ = 0;
};

} // namespace ipl
} // namespace dpl2