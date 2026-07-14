// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2021-2025, The OpenROAD Authors

#pragma once

#include <vector>

namespace dpl2 {

class Group;

class Architecture
{
public:
    class Row;

    ~Architecture();

    const std::vector<Group*>& getRegions() const { return regions_; }
    int getNumRegions() const { return (int) regions_.size(); }
    Group* getRegion(int r) const { return regions_[r]; }
    Group* createAndAddRegion();
private:
    // Regions...
    std::vector<Group*> regions_;
};

class Architecture::Row
{
public:
    enum PowerType
    {
        Power_UNK,
        Power_VDD,
        Power_VSS
    };
};

} // namespace dpl2
