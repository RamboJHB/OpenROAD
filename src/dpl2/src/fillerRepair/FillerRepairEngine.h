// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Filler-repair entry point. It borrows the Grid/Network already owned
// by dpl2, and privately owns the checker/snapshot needed for one physical-
// design revision. repair() never mutates UDM.

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
  // UDM/infrastructure objects must outlive the engine. init() is one-shot.
  bool init(eUNL::PhysDesMgr* desMgr, const fillerSetting& fillerSetting);

  // Atomically replaces the private engine snapshot after infrastructure has
  // synchronized Network with UDM. This method never changes Network Nodes.
  // Rebuild Grid/Network first if rows, blockages or the instance set changed.
  // A stale/incomplete Network makes update fail closed.
  // Do not call concurrently with repair().
  bool update(eUNL::PhysDesMgr* desMgr, const fillerSetting& fillerSetting);

  // Pre-commit implant overlay query. The only placement gate here is
  // regional: repair refuses to run on top of a gap/overlap inside the rows
  // it can edit. Whole-design placement legality stays with infrastructure.
  // This is the checker-facing entry: the candidate pose/master are consumed
  // from the exact CheckRequest built by ImplantLayerChecker::check().
  RepairOutcome repair(const ipl::CheckRequest& request);

  // Direct entry retained for focused engine tests and callers that already
  // have UDM handles. Normal placement checking enters through
  // ImplantLayerChecker::check().
  RepairOutcome repair(eUNL::LeafCellID targetCell,
                       const eLIB::PhysLibCell& newMaster);

 private:
  class Impl;
  Grid* grid_ = nullptr;
  Network* network_ = nullptr;
  bool debug_logging_ = false;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fillerRepair
}  // namespace dpl2
