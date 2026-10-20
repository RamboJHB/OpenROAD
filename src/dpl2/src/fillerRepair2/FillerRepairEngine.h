// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The runtime half of filler repair: everything that touches the database.
//
// It borrows the caller-owned checker and caches master/geometry metadata.
// Each repair reads current Grid/Network placement through a request-local
// PlacementView and feeds the pure search through a request-local RepairOracle.
//
// repair() accepts an optional temporary standard cell, one old standard-cell
// Delete and/or filler Deletes. A null target prepares standard-cell removal.
// Pending filler Adds are rebuilt around the final target; pending Replaces
// seed the existing swap planner. The result is the complete filler transaction
// (Delete/Add/Replace), excluding the old standard-cell Delete.
// UDM, Grid and Network come out exactly as they went in; committing is the
// caller's decision.

#pragma once

#include <drc/ImplantLayerChecker.h>

#include <memory>

namespace dpl2 {
namespace fillerRepair {

struct RepairOutcome
{
  bool hasSolution = false;
  ipl::FillerChanges changes;
};

class FillerRepairEngine
{
 public:
  // The checker exposes the Grid and Network initialized by DePlace and must
  // outlive the engine. Construction caches master/VT/compatibility metadata,
  // not placed instances. Masters, rules and grid geometry must stay unchanged.
  explicit FillerRepairEngine(const ipl::ImplantLayerChecker& checker);
  ~FillerRepairEngine();

  FillerRepairEngine(const FillerRepairEngine&) = delete;
  FillerRepairEngine& operator=(const FillerRepairEngine&) = delete;

  bool isReady() const;

  // Temporary Node identity/master and request placement describe the target;
  // its stored position is ignored. All filler records are normalized and
  // checker-approved before success; failure returns no partial transaction.
  // Placement commits must update Grid and Network before the next repair;
  // neither may be mutated during repair (concurrent read-only repairs are OK).
  RepairOutcome repair(const ipl::CheckRequest& request) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fillerRepair
}  // namespace dpl2
