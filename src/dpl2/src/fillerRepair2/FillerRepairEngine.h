// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The runtime half of filler repair: everything that touches the database.
//
// It borrows the caller-owned checker, freezes one placement revision, and
// feeds the pure search (RepairPlanner) through PlacementView and a
// request-local RepairOracle.
//
// repair() accepts one temporary standard-cell Node plus Delete overlays naming
// either one same-footprint std cell or every filler exactly covered by a new
// buffer. It answers only with surrounding filler Replace records and changes
// nothing.
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
  // outlive the engine. Construction builds the immutable repair snapshot.
  explicit FillerRepairEngine(const ipl::ImplantLayerChecker& checker);
  ~FillerRepairEngine();

  FillerRepairEngine(const FillerRepairEngine&) = delete;
  FillerRepairEngine& operator=(const FillerRepairEngine&) = delete;

  bool isReady() const;

  // The checker already validates this request. The engine trusts its temporary
  // Node and target overlay, then returns only checker-approved filler swaps.
  RepairOutcome repair(const ipl::CheckRequest& request) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fillerRepair
}  // namespace dpl2
