// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Top-level UDM-facing entry point of the filler VT overlay repair.
//
// Wires the whole stack together for one design state:
//   UdmIdBridge (id replay + validate)
//     -> CheckerPlacementView (snapshot incl. coverage extras)
//     -> UdmPrecheck (cached 100%-utility gate)
//     -> CheckerOracleAdapter (list-only overlay API)
//     -> UdmMasterCandidateProvider (checker fillers ∩ fillerSetting)
//     -> FillerRepairEngine (pure planner)
//
// Usage (per opto target, BEFORE committing the target's master change):
//   FillerVtRepair repair(desMgr, checker, config);
//   if (!repair.isReady()) { report repair.setupDiagnostics(); }
//   auto result = repair.repair(targetCellId, newMaster);
//   if (result.hasSolution) { commit target + result.changes atomically; }
//
// Lifetime: this object snapshots the design at construction. Rebuild it
// after ANY commit (the checker must also have been updated/rebuilt from the
// same state). The precheck cache survives rebuilds when shared via
// FillerVtRepairConfig::sharedPrecheck -- same-size swap commits do not
// change coverage, so keep coverageRevision stable across them and bump it
// only on geometry-changing edits (spec #12).
//
// Snapshot semantics: the target's NEW master is passed as an overlay to
// checkPlaceWithOverlays (empty filler-change candidate) -- the committed
// design may still hold the OLD master. The engine's baseline/candidate
// checks then go through the SAME adapter, so the one-to-one multiset delta
// never mixes snapshot sources (duplicate contract, AGENTS D21).

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// UDM (mirrors the checker header's include set).
#include <phys/physDesMgr.hh>

#include "../FillerRepairEngine.h"
#include "CheckerOracleAdapter.h"
#include "CheckerPlacementView.h"
#include "UdmIdBridge.h"
#include "UdmMasterCandidateProvider.h"
#include "UdmPrecheck.h"
#include "infrastructure/fillerSetting.h"

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

// One accepted filler swap in UDM terms (plus the checker-frame ids for
// logging / cross-checks). Commit stays with the infrastructure.
struct UdmFillerChange
{
  eUNL::LeafCellID cellId;
  const eLIB::PhysLibCell* newMaster = nullptr;
  InstanceId instanceId = 0;
  MasterId newMasterId = 0;
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

  // Optional ECO filler allow list; nullptr -> every checker filler master.
  dpl2::fillerSetting* fillerSetting = nullptr;

  // Optional shared precheck cache (see UdmPrecheck.h). nullptr -> a private
  // instance is used (precheck reruns per FillerVtRepair construction).
  UdmPrecheck* sharedPrecheck = nullptr;
  std::uint64_t coverageRevision = 0;  // bump on geometry-changing edits ONLY

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
                 const ipl::ImplantLayerChecker& checker,
                 const FillerVtRepairConfig& config = {});

  // False when bridge validation failed (id replay out of lockstep with
  // initFromUDM) -- repair() then refuses with the setup diagnostics.
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
  UdmPrecheck own_precheck_;
  std::unique_ptr<UdmIdBridge> bridge_;
  std::unique_ptr<CheckerPlacementView> view_;
  std::unique_ptr<CheckerOracleAdapter> oracle_;
  std::unique_ptr<UdmMasterCandidateProvider> candidates_;
  DbCoord default_halo_x_ = 0;
  bool ready_ = false;
  std::vector<Diagnostic> setup_diagnostics_;
};

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
