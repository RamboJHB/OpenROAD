// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Top-level UDM-facing entry point of the filler VT overlay repair.
//
// Wires one immutable infrastructure snapshot to the checker oracle and
// planner:
//   InfrastructurePlacementView (Network/Node/Master + fillerSetting)
//     -> CheckerOracleAdapter (list-only overlay API)
//     -> FillerRepairEngine (pure planner)
//
// Usage (before committing the target master change):
//   FillerVtRepair repair(desMgr, network, checker, fillerSetting, config);
//   if (!repair.isReady()) { report repair.setupDiagnostics(); }
//   auto result = repair.repair(targetCellId, newMaster);
//
// Rebuild after any design commit so infrastructure, checker and planner all
// describe the same state. Coverage is cached only inside this immutable view.
//
// Snapshot semantics: the target's NEW master is passed as an overlay to
// checkPlaceWithOverlays (empty filler-change candidate) -- the committed
// design may still hold the OLD master. The engine's baseline/candidate
// checks then go through the SAME adapter, so the one-to-one multiset delta
// never mixes snapshot sources (duplicate contract, AGENTS D21).

#pragma once

#include <memory>
#include <vector>

// UDM (mirrors the checker header's include set).
#include <phys/physDesMgr.hh>

#include "../FillerRepairEngine.h"
#include "CheckerOracleAdapter.h"
#include "InfrastructurePlacementView.h"
#include "infrastructure/fillerSetting.h"
#include "infrastructure/network.h"

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

// One accepted filler swap in UDM terms. Commit stays with the
// infrastructure.
struct UdmFillerChange
{
  eUNL::LeafCellID cellId;
  const eLIB::PhysLibCell* newMaster = nullptr;
};

struct FillerVtRepairResult
{
  bool hasSolution = false;
  std::vector<UdmFillerChange> changes;
  std::vector<Diagnostic> diagnostics;
};

struct FillerVtRepairConfig
{
  RepairConfig repair;

  // Initial-snapshot guard halo around the target, in DBU on each side.
  // <= 0 -> derived as 2x the max rule query radius (minValue/prl/length)
  // from checker.rules(). Precision is not correctness-critical: the engine
  // re-derives its own windows/guards from the snapshot violations and
  // expands adaptively. [VERIFY-UDM] validate against real rule decks.
  DbCoord snapshotHaloX = 0;
  int snapshotHaloRows = 1;  // rows above/below the target's covered rows

  bool verbose = false;  // enables the [fr] transcript across all stages
};

class FillerVtRepair
{
 public:
  // `checker` must be initialized (initFromUDM) from the SAME desMgr state,
  // with no commits in between.
  FillerVtRepair(const eUNL::PhysDesMgr& desMgr,
                 const dpl2::Network& network,
                 const ipl::ImplantLayerChecker& checker,
                 const dpl2::fillerSetting& fillerSetting,
                 const FillerVtRepairConfig& config = {});

  // False when infrastructure/checker ids or geometry disagree; repair() then
  // refuses with setup diagnostics.
  bool isReady() const { return ready_; }
  const std::vector<Diagnostic>& setupDiagnostics() const
  {
    return setup_diagnostics_;
  }

  // `targetCell` = the opto-changed std cell (must be in the checker's
  // placed set); `newMaster` = its proposed new master (must carry implant
  // shapes). Pure: no design mutation; the result lists the filler swaps to
  // commit together with the target change.
  FillerVtRepairResult repair(eUNL::LeafCellID targetCell,
                              const eLIB::PhysLibCell& newMaster);

 private:
  Region snapshotGuard(const TargetPlace& target) const;

  FillerVtRepairConfig config_;
  DebugLog log_;
  std::unique_ptr<InfrastructurePlacementView> view_;
  std::unique_ptr<CheckerOracleAdapter> oracle_;
  DbCoord default_halo_x_ = 0;
  bool ready_ = false;
  std::vector<Diagnostic> setup_diagnostics_;
};

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
