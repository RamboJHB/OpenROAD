// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Production filler-repair facade. Like ImplantLayerChecker, callers bind
// Grid/Network once, initialize against one physical-design revision, then
// issue const pre-commit queries. Neither precheck() nor repair() mutates UDM.

#pragma once

#include <memory>
#include <vector>

#include "drc/ImplantLayerChecker.h"

namespace dpl2 {

class Grid;
class Network;
class fillerSetting;

namespace fillerRepair {

struct RepairOutcome
{
  bool hasSolution = false;
  ipl::FillerChanges changes;
  std::vector<ipl::Diagnostic> diagnostics;
};

class FillerRepairEngine
{
 public:
  FillerRepairEngine(Grid* grid, Network* network);
  ~FillerRepairEngine();

  FillerRepairEngine(const FillerRepairEngine&) = delete;
  FillerRepairEngine& operator=(const FillerRepairEngine&) = delete;

  bool init(eUNL::PhysDesMgr* desMgr,
            const ipl::ImplantLayerChecker* checker,
            const fillerSetting* fillerSetting);

  // Placement-only gate for opto to call before any cell mutation.
  // isLegal=false blocks opto and diagnostics contain Gap/Overlap warnings.
  ipl::CheckResult precheck() const;

  // Pre-commit implant overlay query. Gap/overlap precheck is deliberately
  // not called here; opto owns that sequencing.
  RepairOutcome repair(eUNL::LeafCellID targetCell,
                       const eLIB::PhysLibCell& newMaster);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fillerRepair
}  // namespace dpl2
