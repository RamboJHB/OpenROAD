// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <PlacementDRC.h>
#include <drc/ImplantLayerChecker.h>
#include <fillerInsertion/FillerInsertionEngine.h>
#include <fillerInsertion/FillerInsertionPlanner.h>
#include <infrastructure/Grid.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dpl2::fillerInsertion {
namespace {

namespace planner = internal;

struct RuntimeMaster
{
  const eLIB::PhysLibCell* physical = nullptr;
  const Master* network = nullptr;
  int widthSites = 0;
  int heightRows = 0;
};

InsertionDiagnostic diagnostic(std::string code, std::string message)
{
  return {std::move(code), std::move(message)};
}

std::string insertionPrefix(const std::string& configured)
{
  std::string prefix = configured.empty() ? "FILLER" : configured;
  if (prefix.back() != '_') {
    prefix.push_back('_');
  }
  prefix += "INSERT_";
  return prefix;
}

std::string insertionName(const std::string& prefix,
                          const planner::TiledFiller& tile,
                          const RuntimeMaster& master,
                          std::size_t index)
{
  std::ostringstream stream;
  stream << prefix << 'R' << tile.rowId << "_C" << tile.columnId << "_W"
         << master.widthSites << "_H" << master.heightRows << '_' << index;
  return stream.str();
}

}  // namespace

FillerInsertionEngine::FillerInsertionEngine(const Grid& grid,
                                             const Network& network,
                                             const fillerSetting& setting,
                                             const PlacementDRC* placementDrc)
    : grid_(grid),
      network_(network),
      setting_(setting),
      placementDrc_(placementDrc)
{
}

InsertionOutcome FillerInsertionEngine::plan(
    const InsertionLimits& limits) const
{
  InsertionOutcome outcome;
  const int rowCount = grid_.getRowCount().v;
  const int columnCount = grid_.getRowSiteCount().v;
  const int siteWidth = grid_.getSiteWidth().v;
  if (rowCount <= 0 || columnCount <= 0 || siteWidth <= 0) {
    outcome.diagnostics.push_back(diagnostic(
        "invalid_grid", "filler insertion requires a non-empty site grid"));
    return outcome;
  }

  planner::SiteGrid siteGrid;
  siteGrid.rowCount = rowCount;
  siteGrid.columnCount = columnCount;
  siteGrid.fillable.resize(static_cast<std::size_t>(rowCount) * columnCount, 0);
  for (int row = 0; row < rowCount; ++row) {
    for (int column = 0; column < columnCount; ++column) {
      const Pixel* pixel = grid_.gridPixel(GridX{column}, GridY{row});
      const bool fillable = pixel != nullptr && pixel->is_valid
                            && pixel->cell == nullptr
                            && pixel->padding_reserved_by == nullptr;
      siteGrid.fillable[static_cast<std::size_t>(row) * columnCount + column]
          = fillable ? 1 : 0;
    }
  }

  std::vector<RuntimeMaster> masters;
  const std::vector<const eLIB::PhysLibCell*> configured
      = setting_.getFillerPhysCells();
  masters.reserve(configured.size());
  for (const eLIB::PhysLibCell* physical : configured) {
    if (physical == nullptr || physical->getTechSite() == nullptr) {
      outcome.diagnostics.push_back(diagnostic(
          "filler_missing_site", "configured filler has no physical site"));
      return outcome;
    }
    const int width = physical->getWidth().getStorage();
    const int height = physical->getHeight().getStorage();
    if (width <= 0 || height <= 0 || width % siteWidth != 0) {
      outcome.diagnostics.push_back(diagnostic(
          "filler_not_site_aligned",
          "configured filler width/height must be positive and site aligned"));
      return outcome;
    }
    const Master* networkMaster
        = network_.getMaster(network_.getMasterId(physical->getLibCellId()));
    if (networkMaster == nullptr || networkMaster->getPhysLibCell() != physical
        || !networkMaster->isFiller()) {
      outcome.diagnostics.push_back(diagnostic(
          "stale_filler_catalog",
          "configured filler is absent or not published as a Network filler"));
      return outcome;
    }
    const int heightRows = grid_.gridHeight(*physical).v;
    if (heightRows <= 0) {
      outcome.diagnostics.push_back(diagnostic(
          "invalid_filler_height", "configured filler has no row footprint"));
      return outcome;
    }
    masters.push_back({physical, networkMaster, width / siteWidth, heightRows});
  }
  if (masters.empty()) {
    outcome.diagnostics.push_back(diagnostic(
        "no_filler_masters", "set_filler_option supplied no filler masters"));
    return outcome;
  }

  if (!setting_.getFollowOrder()) {
    std::stable_sort(masters.begin(),
                     masters.end(),
                     [](const auto& left, const auto& right) {
                       const int leftArea = left.widthSites * left.heightRows;
                       const int rightArea
                           = right.widthSites * right.heightRows;
                       if (leftArea != rightArea) {
                         return leftArea > rightArea;
                       }
                       if (left.heightRows != right.heightRows) {
                         return left.heightRows > right.heightRows;
                       }
                       if (left.widthSites != right.widthSites) {
                         return left.widthSites > right.widthSites;
                       }
                       return left.network->getId() < right.network->getId();
                     });
  }

  std::vector<planner::FillerFootprint> footprints;
  std::map<int, RuntimeMaster> masterById;
  footprints.reserve(masters.size());
  for (const RuntimeMaster& master : masters) {
    planner::FillerFootprint footprint;
    footprint.masterId = master.network->getId();
    footprint.widthSites = master.widthSites;
    footprint.heightRows = master.heightRows;
    footprint.allowedOrigins.resize(siteGrid.fillable.size(), 0);
    const std::string& siteName = master.physical->getTechSite()->getName();
    const int physicalHeight = master.physical->getHeight().getStorage();
    for (int row = 0; row + master.heightRows <= rowCount; ++row) {
      const int y = grid_.gridYToDbu(GridY{row}).v;
      const int yEnd = grid_.gridYToDbu(GridY{row + master.heightRows}).v;
      if (yEnd - y != physicalHeight) {
        continue;
      }
      for (int column = 0; column + master.widthSites <= columnCount;
           ++column) {
        if (grid_.getSiteOrientation(GridX{column}, GridY{row}, siteName)
                .has_value()) {
          footprint.allowedOrigins[static_cast<std::size_t>(row) * columnCount
                                   + column]
              = 1;
        }
      }
    }
    footprints.push_back(std::move(footprint));
    masterById.emplace(master.network->getId(), master);
  }

  planner::PlannerConfig plannerConfig;
  plannerConfig.fitSpace = setting_.getFitSpace();
  plannerConfig.maxSolutions = limits.maxSolutions;
  plannerConfig.maxSearchStates = limits.maxSearchStates;
  for (const auto& [pair, avoid] : setting_.getAvoidPattern()) {
    if (avoid) {
      plannerConfig.forbiddenWidthAbutments.insert(pair);
    }
  }
  const planner::PlannerResult planned
      = planner::planFillers(siteGrid, std::move(footprints), plannerConfig);
  outcome.fillableSites = planned.fillableSites;
  outcome.coveredSites = planned.coveredSites;
  outcome.searchStates = planned.searchStates;
  if (planned.status == planner::PlannerStatus::BudgetExceeded) {
    outcome.diagnostics.push_back(diagnostic(
        "search_budget_exceeded", "filler insertion search budget exhausted"));
    return outcome;
  }
  if (planned.status == planner::PlannerStatus::InvalidInput) {
    outcome.diagnostics.push_back(diagnostic(
        "invalid_planner_input", "filler insertion planner input is invalid"));
    return outcome;
  }
  if (planned.solutions.empty()) {
    outcome.diagnostics.push_back(
        diagnostic("space_not_tileable",
                   "configured fillers cannot tile the empty sites"));
    return outcome;
  }

  const auto* implantChecker
      = placementDrc_ != nullptr
            ? dynamic_cast<const ipl::ImplantLayerChecker*>(
                  placementDrc_->getChecker(DRCCheckerType::ImplantLayer))
            : nullptr;
  if (setting_.getCheckDRC() && implantChecker == nullptr) {
    outcome.diagnostics.push_back(diagnostic(
        "missing_insertion_checker",
        "CheckDRC requires a published ImplantLayerChecker revision"));
    return outcome;
  }

  const std::string prefix = insertionPrefix(setting_.getPrefix());
  bool rejectedByCommittedAvoidPattern = false;
  bool rejectedByDrc = false;
  for (const std::vector<planner::TiledFiller>& solution : planned.solutions) {
    std::vector<CellChangeRecord> changes;
    changes.reserve(solution.size());
    bool valid = true;
    for (std::size_t index = 0; index < solution.size(); ++index) {
      const planner::TiledFiller& tile = solution[index];
      const auto found = masterById.find(tile.masterId);
      if (found == masterById.end()) {
        valid = false;
        break;
      }
      const RuntimeMaster& master = found->second;
      const std::optional<PhysOrientation> orientation
          = grid_.getSiteOrientation(GridX{tile.columnId},
                                     GridY{tile.rowId},
                                     master.physical->getTechSite()->getName());
      if (!orientation.has_value()) {
        valid = false;
        break;
      }
      changes.push_back(
          {OpType::Add,
           CellData{insertionName(prefix, tile, master, index)},
           eUTL::UvDist(static_cast<int64_t>(tile.columnId) * siteWidth),
           eUTL::UvDist(
               static_cast<int64_t>(grid_.gridYToDbu(GridY{tile.rowId}).v)),
           eLIB::LibCellID{},
           master.physical->getLibCellId(),
           *orientation});
    }
    if (!valid) {
      continue;
    }
    bool avoidsCommittedFiller = true;
    for (const planner::TiledFiller& tile : solution) {
      const RuntimeMaster& master = masterById.at(tile.masterId);
      for (int rowOffset = 0; rowOffset < master.heightRows; ++rowOffset) {
        const int row = tile.rowId + rowOffset;
        if (tile.columnId > 0) {
          const Pixel* left
              = grid_.gridPixel(GridX{tile.columnId - 1}, GridY{row});
          if (left != nullptr && left->cell != nullptr && left->cell->isFiller()
              && setting_.needAvoidAbut(
                  {grid_.gridWidth(left->cell).v, master.widthSites})) {
            avoidsCommittedFiller = false;
          }
        }
        const int rightColumn = tile.columnId + master.widthSites;
        if (rightColumn < columnCount) {
          const Pixel* right = grid_.gridPixel(GridX{rightColumn}, GridY{row});
          if (right != nullptr && right->cell != nullptr
              && right->cell->isFiller()
              && setting_.needAvoidAbut(
                  {master.widthSites, grid_.gridWidth(right->cell).v})) {
            avoidsCommittedFiller = false;
          }
        }
      }
    }
    if (!avoidsCommittedFiller) {
      rejectedByCommittedAvoidPattern = true;
      continue;
    }
    if (setting_.getCheckDRC() && implantChecker != nullptr) {
      const ipl::CheckResult checked
          = implantChecker->checkFillerInsertion(changes);
      if (!checked.isLegal) {
        rejectedByDrc = true;
        continue;
      }
    }
    outcome.hasSolution = true;
    outcome.complete = planned.status == planner::PlannerStatus::Complete;
    outcome.changes = std::move(changes);
    return outcome;
  }

  if (rejectedByDrc) {
    outcome.diagnostics.push_back(diagnostic(
        "drc_rejected", "no deterministic filler tiling passed final DRC"));
  } else if (rejectedByCommittedAvoidPattern) {
    outcome.diagnostics.push_back(diagnostic(
        "avoid_pattern_rejected",
        "no tiling satisfies avoid patterns at committed filler boundaries"));
  } else {
    outcome.diagnostics.push_back(
        diagnostic("invalid_generated_tiling",
                   "planner output could not be materialized"));
  }
  return outcome;
}

}  // namespace dpl2::fillerInsertion
