// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Filler-repair entry point. It borrows the Grid/Network already owned
// by dpl2, and privately owns the checker/snapshot needed for one physical-
// design revision. Neither precheck() nor repair() mutates UDM.

#pragma once

#include <memory>
#include <vector>

#include "drc/ImplantLayerChecker.h"

namespace dpl2 {

class fillerSetting;
class Grid;
class Network;

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
  // grid and network are the initialized dpl2 objects (normally
  // DePlace::getGrid()/getNetwork()) and must outlive this engine.
  FillerRepairEngine(Grid* grid, Network* network);
  ~FillerRepairEngine();

  FillerRepairEngine(const FillerRepairEngine&) = delete;
  FillerRepairEngine& operator=(const FillerRepairEngine&) = delete;

  // Enables the [fr][stage] decision transcript. Disabled by default; the
  // switch changes diagnostics output only and never changes search order or
  // acceptance. Configure it outside concurrent precheck()/repair() calls.
  void setDebugLogging(bool enabled);

  // Binds the existing infrastructure to one design and registers configured
  // filler masters. The target replacement master is registered lazily by
  // repair(), so opto does not need to predict it during initialization.
  // UDM/infrastructure objects must outlive the engine. init() is one-shot;
  // construct a new engine after a placement commit.
  bool init(eUNL::PhysDesMgr* desMgr, const fillerSetting& fillerSetting);

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
