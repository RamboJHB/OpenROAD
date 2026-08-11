#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "drc/ImplantLayerChecker.h"

namespace dpl2 {
class Grid;
class Network;

namespace ipl {

struct PlacedInst
{
    InstanceId instanceId = 0;
    MasterId masterId = 0;
    RowId rowId = 0;
    ColId colId = 0;
    PhysOrientation orientation = PhysOrientationE::R0;
    bool isFiller = false;
};

// [fillerRepair-fix] Serializable projection of fillerSetting. A checker dump
// has no UDM Design,
// so configured cells are stored by checker MasterId while every scalar option
// and avoid-pattern entry is preserved.
struct FillerSettingData
{
    bool present = false;
    bool followOrder = true;
    bool checkDrc = true;
    bool fitSpace = true;
    std::string prefix = "ECOFILLER";
    std::vector<MasterId> fillerMasterIds;
    std::map<std::pair<int, int>, bool> avoidPatterns;
};

struct ImplantInput
{
    std::vector<Layer> layers;
    std::vector<Rule> rules;
    std::unordered_map<std::string, std::vector<LayerId>> groups;
    std::vector<MasterItem> masters;
    std::vector<PlacedInst> placedInsts;
    int rowCount = 0;
    int colCount = 0;
    Layer::Polar basePolar = Layer::Polar::P;
    Dbu rowHeight = 0;
    Dbu siteWidth = 0;
    FillerSettingData fillerSetting;
};

// ImplantLayerCheckerHelper is used for test flow
class ImplantLayerCheckerHelper
{
public:
    ImplantLayerCheckerHelper();
    ~ImplantLayerCheckerHelper();

    void initialize(const ImplantInput& input);
    Grid* getGrid() const { return grid_.get(); }
    eUNL::Design* getDesign() const { return nullptr; }
    Network* getNetwork() const { return network_.get(); }
    void initChecker(ImplantLayerChecker& checker);

    bool dump(const std::string& filePath,
              const ImplantLayerChecker& checker) const;
    static ImplantInput load(const std::string& filePath);

private:
    std::unique_ptr<Grid> grid_;
    std::unique_ptr<Network> network_;
    // Preserve input data needed by initChecker
    std::vector<MasterItem> inputMasters_;
    Layer::Polar inputBasePolar_ = Layer::Polar::P;
    std::vector<Layer> inputLayers_;
    std::unordered_map<std::string, std::vector<LayerId>> inputGroups_;
    std::vector<Rule> inputRules_;
    Dbu inputSiteWidth_ = 0;
    Dbu inputRowHeight_ = 0;
    FillerSettingData inputFillerSetting_;
};

}  // namespace ipl
}  // namespace dpl2
