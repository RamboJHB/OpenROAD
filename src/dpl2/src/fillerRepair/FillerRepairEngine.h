// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The runtime half of filler repair: everything that touches the database.
//
// It borrows the Grid and Network dpl2 already owns, builds a snapshot of
// them, and privately owns one checker to ask questions of -- all pinned to a
// single design revision. It then feeds the pure search (RepairPlanner) by
// implementing its two seams: PlacementView and RepairOracle.
//
// repair() answers with a list of proposed filler swaps and changes nothing.
// UDM, Grid and Network come out exactly as they went in; committing is the
// caller's decision.

#pragma once

#include <memory>
#include <vector>

#include <drc/ImplantLayerChecker.h>
#include <fillerRepair/Debug.h>

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

// Why a failing check produced no repair attempt at all. Emitted on the
// [fr] transcript so a run that silently repairs nothing is diagnosable
// without a rebuild; the message text stays inside this module rather than
// in the checker's repair hook.
void reportRepairUnavailable(const char* reason);

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
  // payload or in production calls this -- only the repository-local
  // regression, which does not travel. DELETE it and the Impl method behind
  // it (~10 lines) unless you want per-engine control that the environment
  // variable cannot give you.
  void setDebugLogging(bool enabled);

  // Binds the existing infrastructure to the explicit Design owned by
  // fillerSetting; that Design, desMgr and Grid manager must agree. Registers
  // configured filler masters. The target replacement master is registered
  // lazily by repair(), so opto does not need to predict it during
  // initialization.
  // UDM/infrastructure objects must outlive the engine. init() is one-shot.
  bool init(eUNL::PhysDesMgr* desMgr, const fillerSetting& fillerSetting);

  // [PORT-DROP] Rebuilds the private snapshot in place. It pre-dates lazy
  // init and no production path reaches it: refreshing is done by calling
  // ImplantLayerChecker::setFillerRepairContext(), which drops the engine so
  // the next failing check builds a new one. Only the repository-local
  // regression drives snapshot refresh through here, and that does not
  // travel. DELETE it (~30 lines with its Impl half).
  //
  // Contract while it exists: never changes Network Nodes; rebuild
  // Grid/Network first if rows, blockages or the instance set changed; a
  // stale or incomplete Network makes it fail closed; not concurrent with
  // repair().
  bool update(eUNL::PhysDesMgr* desMgr, const fillerSetting& fillerSetting);

  // Pre-commit implant overlay query. The only placement gate here is
  // regional: repair refuses to run on top of a gap/overlap inside the rows
  // it can edit. Whole-design placement legality stays with infrastructure.
  // This is the checker-facing entry: the candidate pose/master are consumed
  // from the exact CheckRequest built by ImplantLayerChecker::check().
  RepairOutcome repair(const ipl::CheckRequest& request);

  // [PORT-DROP] The same repair, entered with raw UDM handles instead of a
  // CheckRequest. Production always arrives through
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
