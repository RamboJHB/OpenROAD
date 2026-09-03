#include "fillerSetting.h"
#include <algorithm>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace dpl2 {

fillerSetting::fillerSetting(eUNL::Design* design)
{
    this->design_ = design;
    this->follow_order_ = true;
    this->check_drc_ = true;
    this->fit_space_ = true;
    this->prefix_ = "ECOFILLER";
}

void fillerSetting::setFollowOrder(bool value)
{
    if (follow_order_ != value) {
        follow_order_ = value;
        ++revision_;
    }
}

void fillerSetting::setCheckDRC(bool value)
{
    if (check_drc_ != value) {
        check_drc_ = value;
        ++revision_;
    }
}

void fillerSetting::setFitSpace(bool value)
{
    if (fit_space_ != value) {
        fit_space_ = value;
        ++revision_;
    }
}

void fillerSetting::setPrefix(std::string value)
{
    if (prefix_ != value) {
        prefix_ = std::move(value);
        ++revision_;
    }
}

void
fillerSetting::addFillerCell(std::string fillerCellName)
{
    if (design_ == nullptr) {
        throw std::logic_error("fillerSetting has no design");
    }
    std::istringstream iss(fillerCellName);
    std::vector<std::string> nameVec{
        std::istream_iterator<std::string>(iss),
        std::istream_iterator<std::string>()
    };
    for (const auto& cellName : nameVec) {
        eFNL::ModuleID moduleId = design_->getLibAcc().findModule(cellName);
        auto* libCell = design_->getLibAcc().getLibCell(moduleId);
        if (libCell == nullptr) {
            throw std::invalid_argument("unknown filler cell: " + cellName);
        }
        const PhysLibCell& pell = design_->getLibAcc().getPhysLibCell(libCell->getId());
        const eLIB::LibCellID id = pell.getLibCellId();
        if (std::find(core_.begin(), core_.end(), id) == core_.end()) {
            core_.push_back(id);
            ++revision_;
        }
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
    std::vector<std::pair<int, int>> parsedPatterns;
    parsedPatterns.reserve(nameVec.size());
    for (const auto& cellName : nameVec) {
        size_t pos = cellName.find(':');
        if (pos == std::string::npos || pos == 0 || pos + 1 == cellName.size()
            || cellName.find(':', pos + 1) != std::string::npos) {
            throw std::invalid_argument(
                "avoid pattern must use <width>:<width>: " + cellName);
        }

        auto parseWidth = [&cellName](const std::string& text) {
            std::size_t consumed = 0;
            int value = 0;
            try {
                value = std::stoi(text, &consumed);
            } catch (const std::exception&) {
                throw std::invalid_argument(
                    "invalid avoid pattern width: " + cellName);
            }
            if (consumed != text.size() || value <= 0) {
                throw std::invalid_argument(
                    "invalid avoid pattern width: " + cellName);
            }
            return value;
        };
        const int first = parseWidth(cellName.substr(0, pos));
        const int second = parseWidth(cellName.substr(pos + 1));
        parsedPatterns.emplace_back(first, second);
    }

    for (const auto& [first, second] : parsedPatterns) {
        const bool changed = !avoid_pattern_[{first, second}]
                             || !avoid_pattern_[{second, first}];
        avoid_pattern_[{first, second}] = true;
        avoid_pattern_[{second, first}] = true;
        if (changed) {
            ++revision_;
        }
    }
}

bool
fillerSetting::needAvoidAbut(std::pair<int, int> twoLibCell) const
{
    return avoid_pattern_.find(twoLibCell) != avoid_pattern_.end();
}

// [FRPORT] Resolve the configured IDs to physical masters for engine init.
std::vector<const eLIB::PhysLibCell*> fillerSetting::getFillerPhysCells() const
{
    std::vector<const eLIB::PhysLibCell*> result;
    if (design_ == nullptr) {
        return result;
    }
    result.reserve(core_.size());
    for (const eLIB::LibCellID id : core_) {
        result.push_back(&design_->getLibAcc().getPhysLibCell(id));
    }
    return result;
}

// [FRPORT] Single configuration predicate used while importing Network masters.
bool fillerSetting::isFillerCell(eLIB::LibCellID libCellId) const
{
    return std::find(core_.begin(), core_.end(), libCellId) != core_.end();
}

} // namespace dpl2
