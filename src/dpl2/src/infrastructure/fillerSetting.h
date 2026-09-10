#pragma once
#include "dpl2/DePlace.h"

#include <vector>
#include <map>

namespace dpl2 {

class fillerSetting
{
public:
    explicit fillerSetting(eUNL::Design* design);
    ~fillerSetting() = default;

    // [FRPORT] Engine policy and generated-cell naming consume these settings.
    ADD_SETTER_GETTER_PP(bool, FollowOrder, follow_order_);
    ADD_SETTER_GETTER_PP(bool, CheckDRC, check_drc_);
    ADD_SETTER_GETTER_PP(bool, FitSpace, fit_space_);
    ADD_SETTER_GETTER_PP(std::string, Prefix, prefix_);

    // setter
    void addFillerCell(std::string fillerCellName);
    // Whitespace-separated positive site-width pairs, e.g. "1:2 1:1".
    // Each pair forbids horizontal filler abutment in both directions, not a
    // ratio. Invalid input throws without installing any part of that input.
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

    bool needAvoidAbut(std::pair<int, int> siteWidths) const;

   private:
    bool follow_order_;
    bool check_drc_;
    bool fit_space_;
    std::string prefix_;
    std::vector<eLIB::LibCellID> core_;
    std::map<std::pair<int, int>, bool> avoid_pattern_;
    eUNL::Design* design_{nullptr};
};

} //namespace dpl2
