// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Database-free exact-cover tiling for the placement cells released when a
// target changes footprint. Implant legality and VT choice stay with the
// checker-guided planner; this layer only guarantees gap/overlap-free geometry.

#pragma once

#include <cstddef>
#include <vector>

#include <fillerRepair/RepairTypes.h>

namespace dpl2::fillerRepair::internal {

struct SiteCell
{
  RowId rowId = 0;
  int colId = 0;

  bool operator==(const SiteCell& other) const
  {
    return rowId == other.rowId && colId == other.colId;
  }
  bool operator<(const SiteCell& other) const
  {
    return rowId != other.rowId ? rowId < other.rowId : colId < other.colId;
  }
};

struct FillerFootprint
{
  MasterId masterId = 0;
  int widthSites = 0;
  int heightRows = 0;
};

struct TiledFiller
{
  MasterId masterId = 0;
  RowId rowId = 0;
  int colId = 0;
};

struct RetileConfig
{
  std::size_t maxSolutions = 16;
  std::size_t maxSearchStates = 100000;
};

struct RetileResult
{
  std::vector<std::vector<TiledFiller>> solutions;
  bool truncated = false;
  std::size_t searchStates = 0;
};

// Enumerates deterministic exact covers of `emptySites`. Input filler masters
// are collapsed by footprint: master/orientation alternatives are handled
// later by the normal checker-guided search, so geometry enumeration never
// repeats the same tiling merely because a footprint has several masters.
RetileResult enumerateRetilings(std::vector<SiteCell> emptySites,
                                std::vector<FillerFootprint> footprints,
                                RetileConfig config = {});

}  // namespace dpl2::fillerRepair::internal
