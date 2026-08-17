// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The runtime half of filler repair: everything that touches the database.
//
// It borrows the caller-owned checker, freezes one placement revision, and
// feeds the pure search (RepairPlanner) through PlacementView and a
// request-local RepairOracle.
//
// repair() accepts one temporary standard-cell Node plus exactly one overlay
// record naming the same-footprint Network Node it replaces. It answers only
// with surrounding filler Replace records and changes nothing.
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
  // The checker is the engine's only infrastructure seam. It exposes the
  // Grid, Design and Network initialized by DePlace and must outlive the
  // engine. Construction eagerly builds the immutable repair snapshot before
  // the object can be published to checker worker threads.
  explicit FillerRepairEngine(const ipl::ImplantLayerChecker& checker);
  ~FillerRepairEngine();

  FillerRepairEngine(const FillerRepairEngine&) = delete;
  FillerRepairEngine& operator=(const FillerRepairEngine&) = delete;

  // Diagnostics are retained for setup/debug reporting. A not-ready engine
  // always fails closed and never returns partial changes.
  bool isReady() const;
  std::vector<ipl::Diagnostic> getInitDiagnostics() const;

  // The overlay record is caller-owned and represents either the old std cell
  // (isLegal) or the one filler replaced by a new std cell (findLegal). Both
  // footprints must be identical and the target may not move. The result is
  // atomic, Replace-only, and never contains that overlay target itself.
  RepairOutcome repair(const ipl::CheckRequestOverlay& request) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fillerRepair
}  // namespace dpl2
