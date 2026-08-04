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

  // Optional per-engine override for the [fr][stage] transcript.
  void setDebugLogging(bool enabled);

  // Binds the existing infrastructure; fillerSetting's manager, desMgr and
  // Grid manager must agree. Configured filler masters and target replacement
  // masters must already exist in Network. init() only refreshes the
  // configured masters' filler flag; it never constructs a Master.
  // UDM/infrastructure objects must outlive the engine. init() is one-shot.
  bool init(eUNL::PhysDesMgr* desMgr, const fillerSetting& fillerSetting);

  // Pre-commit implant overlay query. The only placement gate here is
  // regional: repair refuses to run on top of a gap/overlap inside the rows
  // it can edit. Whole-design placement legality stays with infrastructure.
  // This is the checker-facing entry: the candidate pose/master are consumed
  // from the exact CheckRequest built by ImplantLayerChecker::check().
  RepairOutcome repair(const ipl::CheckRequest& request);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fillerRepair
}  // namespace dpl2
