// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2021-2025, The OpenROAD Authors

#include "architecture.h"
#include "Objects.h"

namespace dpl2 {

Architecture::~Architecture()
{
    for (auto& region : regions_) {
        delete region;
    }
    regions_.clear();
}

Group* Architecture::createAndAddRegion()
{
    auto region = new Group();
    regions_.push_back(region);
    return region;
}

} //namespace dpl2
