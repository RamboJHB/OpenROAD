// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The runtime half of filler repair: everything that touches the database.
//
// It borrows Grid, Network and the caller-owned checker, builds a snapshot
// pinned to one design revision, and feeds the pure search (RepairPlanner) by
// implementing its two seams: PlacementView and RepairOracle.
//
// repair() answers with one atomic filler edit transaction and changes
// nothing. It may Replace existing fillers or Delete/Add fillers while
// retiling space released by a size-changing target.
// UDM, Grid and Network come out exactly as they went in; committing is the
// caller's decision.

#pragma once

#include <memory>
#include <vector>

#include <drc/ImplantLayerChecker.h>
#include <fillerRepair/Debug.h>

namespace dpl2 {

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

  // [PORT-DROP] Per-engine override for the [fr][stage] transcript, which is
  // ON by default and silenced globally by FR_VERBOSE=0. Nothing in the
  // payload or in normal runtime calls this -- only the repository-local
  // regression, which does not travel. DELETE it and the Impl method behind
  // it (~10 lines) unless you want per-engine control that the environment
  // variable cannot give you.
  void setDebugLogging(bool enabled);

  // Gets PhysDesMgr from Grid and the active fillerSetting from Network;
  // their designs must agree. The caller owns both objects and supplies the
  // already initialized checker that remains the sole DRC oracle. Configured
  // masters must already be registered by infrastructure with real edge
  // data. UDM/infrastructure/checker objects must outlive the engine. init()
  // is one-shot and must finish before worker threads start.
  bool init(const ipl::ImplantLayerChecker& checker);

  // [PORT-DROP] Rebuilds the private snapshot in place. No normal call path
  // reaches it: an infrastructure revision requires its owner to construct
  // and initialize a new checker/engine pair. Only the
  // repository-local regression drives snapshot refresh through here, and
  // that does not travel. DELETE it (~30 lines with its Impl half).
  //
  // Contract while it exists: never changes Network Nodes; rebuild
  // Grid/Network first if rows, blockages or the instance set changed; a
  // stale or incomplete Network makes it fail closed; not concurrent with
  // repair().
  bool update(const ipl::ImplantLayerChecker& checker);

  // Pre-commit implant overlay query. The only placement gate here is
  // regional: repair refuses to run on top of a gap/overlap inside the rows
  // it can edit. Whole-design placement legality stays with infrastructure.
  // This is the checker-facing entry: the candidate pose/master are consumed
  // from the exact CheckRequest built by ImplantLayerChecker::check().
  RepairOutcome repair(const ipl::CheckRequest& request);

  // [PORT-DROP] The same repair, entered with raw UDM handles instead of a
  // CheckRequest. The normal checker path already has this request;
  // ImplantLayerChecker::check(), which has the CheckRequest already built;
  // this overload exists so the repository-local regression can call the
  // engine without a checker, and that does not travel. DELETE it (~25 lines)
  // unless you have a caller holding UDM handles and no CheckRequest.
  RepairOutcome repair(eUNL::LeafCellID targetCell,
                       const eLIB::PhysLibCell& newMaster);

 private:
  class Impl;
  Grid* grid_ = nullptr;
  Network* network_ = nullptr;
  bool debug_logging_ = debugLoggingDefault();
  std::unique_ptr<Impl> impl_;
};

}  // namespace fillerRepair
}  // namespace dpl2
