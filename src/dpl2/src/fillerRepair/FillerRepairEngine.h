// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Production filler-repair facade. One init() builds and owns the private
// Grid/Network/checker snapshot for one physical-design revision. Callers do
// not construct repair infrastructure. Neither precheck() nor repair() mutates
// UDM.

#pragma once

#include <memory>
#include <vector>

#include "drc/ImplantLayerChecker.h"

namespace dpl2 {

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
  FillerRepairEngine();
  ~FillerRepairEngine();

  FillerRepairEngine(const FillerRepairEngine&) = delete;
  FillerRepairEngine& operator=(const FillerRepairEngine&) = delete;

  // Enables the [fr][stage] decision transcript. Disabled by default; the
  // switch changes diagnostics output only and never changes search order or
  // acceptance. Configure it outside concurrent precheck()/repair() calls.
  void setDebugLogging(bool enabled);

  // Builds the complete immutable repair snapshot and the final implant
  // checker in one call. leafCells is the hierarchy traversal result owned by
  // opto; targetNewMaster is registered even when it is not instantiated.
  // UDM design/library objects must outlive the engine. An engine is
  // one-design/one-init: construct a new engine after a commit.
  bool init(eUNL::PhysDesMgr* desMgr,
            const std::vector<eUNL::LeafCellID>& leafCells,
            const fillerSetting& fillerSetting,
            const eLIB::PhysLibCell& targetNewMaster);

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
