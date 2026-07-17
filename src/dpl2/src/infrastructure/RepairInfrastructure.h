// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Production infrastructure snapshot used by fillerRepair.
//
// The caller supplies the design's leaf-cell IDs; every physical property is
// read from PhysDesMgr.  This keeps hierarchy traversal (which is owned by the
// embedding application) outside the repair module while ensuring tests and
// production use exactly the same Network/Grid construction code.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Grid.h"
#include "Padding.h"
#include "fillerSetting.h"
#include "network.h"

namespace dpl2 {

class RepairInfrastructure
{
 public:
  struct Config
  {
    // Grid's hopeless-region calculation is only a search optimization.  A
    // generous default keeps the repair snapshot usable without coupling it
    // to detailed-placement displacement policy.
    int maxDisplacementX = 100;
    int maxDisplacementY = 100;
  };

  bool build(eUNL::PhysDesMgr* desMgr,
             const std::vector<eUNL::LeafCellID>& leafCells,
             const fillerSetting& fillerSettings,
             const eLIB::PhysLibCell& targetNewMaster,
             Config config);

  bool build(eUNL::PhysDesMgr* desMgr,
             const std::vector<eUNL::LeafCellID>& leafCells,
             const fillerSetting& fillerSettings,
             const eLIB::PhysLibCell& targetNewMaster)
  {
    return build(desMgr, leafCells, fillerSettings, targetNewMaster, Config{});
  }

  bool isReady() const { return ready_; }
  const std::vector<std::string>& diagnostics() const { return diagnostics_; }

  Grid* grid() { return &grid_; }
  const Grid* grid() const { return &grid_; }
  Network* network() { return &network_; }
  const Network* network() const { return &network_; }

 private:
  void fail(const std::string& message);

  bool built_ = false;
  bool ready_ = false;
  std::vector<std::string> diagnostics_;
  std::shared_ptr<Padding> padding_ = std::make_shared<Padding>();
  Grid grid_;
  Network network_;
};

}  // namespace dpl2
