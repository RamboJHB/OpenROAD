#include "fillerSetting.h"

#include <iterator>
#include <sstream>

namespace dpl2 {

fillerSetting::fillerSetting(eUNL::Design* design)
{
    this->design_ = design;
    this->follow_order_ = true;
    this->check_drc_ = true;
    this->fit_space_ = true;
    this->prefix_ = "ECOFILLER";
}

fillerSetting::~fillerSetting()
{
}

void
fillerSetting::addFillerCell(std::string fillerCellName)
{
    std::istringstream iss(fillerCellName);
    std::vector<std::string> nameVec{
        std::istream_iterator<std::string>(iss),
        std::istream_iterator<std::string>()
    };
    for (const auto& cellName : nameVec) {
        eFNL::ModuleID moduleId = design_->getLibAcc().findModule(cellName);
        auto* libCell = design_->getLibAcc().getLibCell(moduleId);
        const PhysLibCell& pell = design_->getLibAcc().getPhysLibCell(libCell->getId());
        this->core_.push_back(pell.getLibCellId());
    }
}

void
fillerSetting::addAvoidPattern(std::string avoidPattern)
{
    std::istringstream iss(avoidPattern);
    std::vector<std::string> nameVec{
        std::istream_iterator<std::string>(iss),
        std::istream_iterator<std::string>()
    };
    for (const auto& cellName : nameVec) {
        size_t pos = cellName.find(':');
        if (pos == std::string::npos) {
            // throw std::invalid_argument("String does not contain ':'");
        }
        int first = std::stoi(cellName.substr(0, pos));
        int second = std::stoi(cellName.substr(pos + 1));

        this->avoid_pattern_[{first, second}] = true;
        this->avoid_pattern_[{second, first}] = true;
    }
}

bool
fillerSetting::needAvoidAbut(std::pair<int, int> twoLibCell)
{
    return avoid_pattern_[twoLibCell];
}

} // namespace dpl2
