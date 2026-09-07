// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The runtime half of filler repair: everything that touches the database.
//
// It borrows the caller-owned checker and caches master/geometry metadata.
// Each repair reads current Grid/Network placement through a request-local
// PlacementView and feeds the pure search through a request-local RepairOracle.
//
// repair() accepts one temporary standard-cell Node plus Delete overlays naming
// either one same-footprint std cell or selected fillers intersecting a new
// buffer. Exact-cover requests retain the swap-only path. Non-exact requests
// may return request-local filler Add records for released sites plus
// surrounding filler Replaces. It never returns Deletes and changes nothing.
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

  // The checker already validates this request. The engine trusts its temporary
  // Node and target overlay, then returns only checker-approved filler Adds
  // and/or Replaces.
  // Placement commits must update Grid and Network before the next repair;
  // neither may be mutated during repair (concurrent read-only repairs are OK).
  RepairOutcome repair(const ipl::CheckRequest& request) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fillerRepair
}  // namespace dpl2
