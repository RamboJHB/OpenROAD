// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The runtime half of filler repair: everything that touches the database.
// repair() is non-mutating and returns one checker-verified atomic transaction:
// same-footprint Replace, or Delete/Add retiling for a changed target footprint.

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
  explicit FillerRepairEngine(const ipl::ImplantLayerChecker& checker);
  ~FillerRepairEngine();

  FillerRepairEngine(const FillerRepairEngine&) = delete;
  FillerRepairEngine& operator=(const FillerRepairEngine&) = delete;

  // Construction eagerly builds the immutable snapshot from the checker.
  // Bind this engine only when it is ready.
  bool isReady() const;
  const std::vector<ipl::Diagnostic>& getInitDiagnostics() const;

  // Non-mutating, pre-commit overlay repair.
  RepairOutcome repair(const ipl::CheckRequest& request);
  RepairOutcome repair(const CellChangeRecord& targetChange);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fillerRepair
}  // namespace dpl2
