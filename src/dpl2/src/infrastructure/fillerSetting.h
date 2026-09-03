#pragma once
#include "dpl2/DePlace.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dpl2 {

class fillerSetting
{
public:
    explicit fillerSetting(eUNL::Design* design);
    ~fillerSetting() = default;

    // Engine policy and generated-cell naming consume these settings. The
    // explicit setters publish a monotonic revision so DePlace can rebuild a
    // stale checker after set_filler_option changes any value.
    void setFollowOrder(bool value);
    bool getFollowOrder() const { return follow_order_; }
    void setCheckDRC(bool value);
    bool getCheckDRC() const { return check_drc_; }
    void setFitSpace(bool value);
    bool getFitSpace() const { return fit_space_; }
    void setPrefix(std::string value);
    std::string getPrefix() const { return prefix_; }
    uint64_t getRevision() const { return revision_; }

    // setter
    void addFillerCell(std::string fillerCellName);
    void addAvoidPattern(std::string avoidPattern);

    // [FRPORT] Engine initialization and DePlace master registration consume
    // these read-only views.
    // getter
    const std::vector<eLIB::LibCellID>& getFillerCells() const { return core_; }
    std::vector<const eLIB::PhysLibCell*> getFillerPhysCells() const;
    bool isFillerCell(eLIB::LibCellID libCellId) const;
    const std::map<std::pair<int, int>, bool>& getAvoidPattern() const
    {
      return avoid_pattern_;
    }
    eUNL::Design* getDesign() const { return design_; }

    bool needAvoidAbut(std::pair<int, int> twoLibCell) const;

private:
    bool follow_order_;
    bool check_drc_;
    bool fit_space_;
    std::string prefix_;
    std::vector<eLIB::LibCellID> core_;
    std::map<std::pair<int, int>, bool> avoid_pattern_;
    eUNL::Design* design_{nullptr};
    uint64_t revision_{0};
};

} //namespace dpl2
