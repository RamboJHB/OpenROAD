// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The runtime half of filler repair: everything that touches the database.

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

// Called by the checker hook when lazy initialization is unavailable.
void reportRepairUnavailable(const char* reason);

class FillerRepairEngine
{
 public:
  FillerRepairEngine(Grid* grid, Network* network);
  ~FillerRepairEngine();

  FillerRepairEngine(const FillerRepairEngine&) = delete;
  FillerRepairEngine& operator=(const FillerRepairEngine&) = delete;

  // Gets PhysDesMgr from Grid and the active fillerSetting from Network.
  // Configured masters must already exist in Network with real edge data.
  bool init();

  // Non-mutating, pre-commit overlay repair.
  RepairOutcome repair(const ipl::CheckRequest& request);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fillerRepair
}  // namespace dpl2
