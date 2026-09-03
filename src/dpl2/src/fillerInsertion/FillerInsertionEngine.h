// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#pragma once

#include <infrastructure/Objects.h>

#include <cstddef>
#include <string>
#include <vector>

namespace dpl2 {
class Grid;
class Network;
class PlacementDRC;
class fillerSetting;

namespace fillerInsertion {

struct InsertionDiagnostic
{
  std::string code;
  std::string message;
};

struct InsertionLimits
{
  std::size_t maxSolutions = 64;
  std::size_t maxSearchStates = 250000;
};

struct InsertionOutcome
{
  bool hasSolution = false;
  bool complete = false;
  std::vector<CellChangeRecord> changes;
  std::vector<InsertionDiagnostic> diagnostics;
  std::size_t fillableSites = 0;
  std::size_t coveredSites = 0;
  std::size_t searchStates = 0;
};

// Immutable request adapter. All candidates and policy come from setting;
// Grid and Network are read-only snapshots. Returned Add records are not
// committed or painted.
class FillerInsertionEngine
{
 public:
  FillerInsertionEngine(const Grid& grid,
                        const Network& network,
                        const fillerSetting& setting,
                        const PlacementDRC* placementDrc = nullptr);

  InsertionOutcome plan(const InsertionLimits& limits = {}) const;

 private:
  const Grid& grid_;
  const Network& network_;
  const fillerSetting& setting_;
  const PlacementDRC* placementDrc_;
};

}  // namespace fillerInsertion
}  // namespace dpl2
