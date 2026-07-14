#pragma once
#include "dpl2/DePlace.h"

#include <vector>
#include <map>

namespace dpl2 {

class fillerSetting
{
public:
    fillerSetting(eUNL::Design* design);
    ~fillerSetting();

    ADD_SETTER_GETTER_PP(bool, FollowOrder, follow_order_);
    ADD_SETTER_GETTER_PP(bool, CheckDRC, check_drc_);
    ADD_SETTER_GETTER_PP(bool, FitSpace, fit_space_);
    ADD_SETTER_GETTER_PP(std::string, Prefix, prefix_);

    // setter
    void addFillerCell(std::string fillerCellName);
    void addAvoidPattern(std::string avoidPattern);

    // getter
    std::vector<eLIB::LibCellID> getFillerCells() {return core_;};
    std::map<std::pair<int, int>, bool> getAvoidPattern() {return avoid_pattern_;};
    eUNL::Design* getDesign() {return design_;};

    bool needAvoidAbut(std::pair<int, int> twoLibCell);

private:
    bool follow_order_;
    bool check_drc_;
    bool fit_space_;
    std::string prefix_;
    std::vector<eLIB::LibCellID> core_;
    std::map<std::pair<int, int>, bool> avoid_pattern_;
    eUNL::Design* design_;
};

} //namespace dpl2
