/////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2020, The Regents of the University of California
// All rights reserved.
//
// BSD 3-Clause License
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
///////////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "FillerPlacementInternal.h"
#include "dpl/Opendp.h"
#include "utl/Logger.h"

#ifdef DPL2_FILLER_SETTING_AVAILABLE
#include <dpl2/DePlace.h>
#include <infrastructure/fillerSetting.h>
#endif

namespace dpl {

using std::to_string;
using std::vector;

using utl::DPL;

using odb::dbMaster;
using odb::dbPlacementStatus;

namespace {

struct RuntimeMaster
{
  dbMaster* master = nullptr;
  int width_sites = 0;
  int height_rows = 0;
};

string fillerName(const string& prefix,
                  const filler_internal::TiledFiller& filler)
{
  return prefix + to_string(filler.row) + "_" + to_string(filler.column);
}

#ifdef DPL2_FILLER_SETTING_AVAILABLE
string insertionPrefix(const string& configured)
{
  string prefix = configured.empty() ? "FILLER" : configured;
  if (prefix.back() != '_') {
    prefix.push_back('_');
  }
  return prefix + "INSERT_";
}

dbMaster* findUniqueMaster(dbDatabase* db, const string& name)
{
  dbMaster* match = nullptr;
  for (dbLib* library : db->getLibs()) {
    if (dbMaster* master = library->findMaster(name.c_str())) {
      if (match != nullptr && match != master) {
        return nullptr;
      }
      match = master;
    }
  }
  return match;
}
#endif

}  // namespace

struct Opendp::FillerPlacementRequest
{
  dbMasterSeq masters;
  string prefix;
  bool follow_order = true;
  bool fit_space = true;
  std::set<std::pair<dbMaster*, dbMaster*>> forbidden_master_abutments;
};

void Opendp::fillerPlacement(dbMasterSeq* filler_masters, const char* prefix)
{
  FillerPlacementRequest request;
  if (filler_masters != nullptr) {
    request.masters = *filler_masters;
  }
  request.prefix = prefix == nullptr ? "FILLER_" : prefix;
  // Preserve the historical command behavior: geometry, not caller order,
  // determines the preferred legacy filler sequence.
  request.follow_order = false;
  request.fit_space = true;
  fillerPlacement(request);
}

void Opendp::fillerPlacement()
{
#ifdef DPL2_FILLER_SETTING_AVAILABLE
  dpl2::DePlace* deplace = dpl2::DePlace::get();
  if (deplace == nullptr || deplace->getFillerSetting() == nullptr) {
    logger_->error(DPL, 42, "dpl2 filler_setting is not initialized.");
  }
  fillerPlacement(*deplace->getFillerSetting());
#else
  logger_->error(DPL,
                 41,
                 "this build has no dpl2 filler_setting integration; pass "
                 "filler masters to filler_placement explicitly.");
#endif
}

void Opendp::fillerPlacement(const dpl2::fillerSetting& setting)
{
#ifdef DPL2_FILLER_SETTING_AVAILABLE
  FillerPlacementRequest request;
  request.prefix = insertionPrefix(setting.getPrefix());
  request.follow_order = setting.getFollowOrder();
  request.fit_space = setting.getFitSpace();

  std::map<int, dbMaster*> masters_by_dpl2_id;
  for (const eLIB::PhysLibCell* physical : setting.getFillerPhysCells()) {
    if (physical == nullptr) {
      continue;
    }
    const string name = physical->getLibCell().getName();
    dbMaster* master = findUniqueMaster(db_, name);
    if (master == nullptr) {
      logger_->error(DPL,
                     43,
                     "dpl2 filler_setting master {} does not uniquely resolve "
                     "to an OpenDB master.",
                     name);
    }
    request.masters.push_back(master);
    masters_by_dpl2_id[physical->getLibCellId().getIndexValue()] = master;
  }
  for (const auto& [master_ids, avoid] : setting.getAvoidPattern()) {
    const auto left = masters_by_dpl2_id.find(master_ids.first);
    const auto right = masters_by_dpl2_id.find(master_ids.second);
    if (avoid && left != masters_by_dpl2_id.end()
        && right != masters_by_dpl2_id.end()) {
      request.forbidden_master_abutments.insert({left->second, right->second});
    }
  }
  fillerPlacement(request);
#else
  (void) setting;
  logger_->error(DPL,
                 51,
                 "this build has no dpl2 filler_setting integration; pass "
                 "filler masters to filler_placement explicitly.");
#endif
}

void Opendp::fillerPlacement(const FillerPlacementRequest& request)
{
  // Refresh occupancy before every insertion.  This keeps repeated calls and
  // database edits made by other commands from planning over stale grid data.
  importDb();

  initGrid();
  setGridCells();

  vector<RuntimeMaster> masters;
  std::set<dbMaster*> seen_masters;
  masters.reserve(request.masters.size());
  for (dbMaster* master : request.masters) {
    if (master == nullptr || !seen_masters.insert(master).second) {
      continue;
    }
    const odb::dbMasterType type = master->getType();
    if (type != odb::dbMasterType::CORE
        && type != odb::dbMasterType::CORE_SPACER) {
      logger_->error(DPL,
                     39,
                     "filler master {} must have CLASS CORE or CORE SPACER.",
                     master->getConstName());
    }
    const int width = master->getWidth();
    const int height = master->getHeight();
    if (width <= 0 || height <= 0 || width % site_width_ != 0
        || height % row_height_ != 0) {
      logger_->error(DPL,
                     40,
                     "filler master {} must have positive site-aligned width "
                     "and row-aligned height.",
                     master->getConstName());
    }
    masters.push_back({master, width / site_width_, height / row_height_});
  }
  if (masters.empty()) {
    logger_->error(DPL, 44, "no filler masters are configured for insertion.");
  }

  if (!request.follow_order) {
    std::stable_sort(
        masters.begin(),
        masters.end(),
        [](const auto& left, const auto& right) {
          const int left_area = left.width_sites * left.height_rows;
          const int right_area = right.width_sites * right.height_rows;
          if (left_area != right_area) {
            return left_area > right_area;
          }
          if (left.height_rows != right.height_rows) {
            return left.height_rows > right.height_rows;
          }
          if (left.width_sites != right.width_sites) {
            return left.width_sites > right.width_sites;
          }
          return string(left.master->getConstName())
                 < right.master->getConstName();
        });
  }

  filler_internal::SiteGrid site_grid;
  site_grid.row_count = row_count_;
  site_grid.column_count = row_site_count_;
  site_grid.fillable.resize(static_cast<std::size_t>(row_count_)
                            * row_site_count_);
  for (int row = 0; row < row_count_; ++row) {
    for (int column = 0; column < row_site_count_; ++column) {
      const Pixel* pixel = gridPixel(column, row);
      site_grid
          .fillable[static_cast<std::size_t>(row) * row_site_count_ + column]
          = pixel != nullptr && pixel->is_valid && pixel->cell == nullptr;
    }
  }

  vector<filler_internal::FillerFootprint> footprints;
  footprints.reserve(masters.size());
  for (int index = 0; index < static_cast<int>(masters.size()); ++index) {
    footprints.push_back(
        {index, masters[index].width_sites, masters[index].height_rows, {}});
  }

  filler_internal::PlannerConfig config;
  config.fit_space = request.fit_space;
  std::map<dbMaster*, int> master_indices;
  for (int index = 0; index < static_cast<int>(masters.size()); ++index) {
    master_indices[masters[index].master] = index;
  }
  for (const auto& [left, right] : request.forbidden_master_abutments) {
    const auto left_index = master_indices.find(left);
    const auto right_index = master_indices.find(right);
    if (left_index != master_indices.end()
        && right_index != master_indices.end()) {
      config.forbidden_master_abutments.insert(
          {left_index->second, right_index->second});
    }
  }
  const filler_internal::PlannerResult plan
      = filler_internal::planFillers(site_grid, footprints, config);
  if (plan.status == filler_internal::PlannerStatus::invalid_input) {
    logger_->error(DPL, 45, "invalid filler insertion planner input.");
  }
  if (plan.status == filler_internal::PlannerStatus::budget_exceeded) {
    logger_->error(DPL,
                   46,
                   "filler insertion search exceeded its deterministic "
                   "budget; no fillers were placed.");
  }
  if (plan.status == filler_internal::PlannerStatus::impossible
      && request.fit_space) {
    for (int row = 0; row < row_count_; ++row) {
      int column = 0;
      while (column < row_site_count_) {
        const std::size_t site
            = static_cast<std::size_t>(row) * row_site_count_ + column;
        if (site_grid.fillable[site] == 0) {
          ++column;
          continue;
        }
        const int begin = column;
        while (column < row_site_count_
               && site_grid.fillable[static_cast<std::size_t>(row)
                                         * row_site_count_
                                     + column]
                      != 0) {
          ++column;
        }
        const int gap = column - begin;
        const int x = core_.xMin() + begin * site_width_;
        const int y = core_.yMin() + row * row_height_;
        logger_->error(
            DPL,
            2,
            "could not fill gap of size {} at {},{} dbu between {} and {}",
            gap,
            x,
            y,
            gridInstName(row, begin - 1),
            gridInstName(row, column + 1));
      }
    }
  }

  std::set<string> names;
  for (const filler_internal::TiledFiller& filler : plan.fillers) {
    const string name = fillerName(request.prefix, filler);
    if (!names.insert(name).second
        || block_->findInst(name.c_str()) != nullptr) {
      logger_->error(DPL,
                     47,
                     "filler instance name {} already exists; no fillers were "
                     "placed.",
                     name);
    }
  }

  vector<dbInst*> created;
  created.reserve(plan.fillers.size());
  for (const filler_internal::TiledFiller& filler : plan.fillers) {
    const RuntimeMaster& master = masters[filler.master_index];
    const string name = fillerName(request.prefix, filler);
    dbInst* inst = dbInst::create(block_,
                                  master.master,
                                  name.c_str(),
                                  /* physical_only */ true);
    if (inst == nullptr) {
      for (dbInst* created_inst : created) {
        dbInst::destroy(created_inst);
      }
      logger_->error(DPL,
                     48,
                     "failed to create filler instance {}; rolled back {} "
                     "instances.",
                     name,
                     created.size());
    }
    const int x = core_.xMin() + filler.column * site_width_;
    const int y = core_.yMin() + filler.row * row_height_;
    inst->setOrient(gridPixel(filler.column, filler.row)->orient_);
    inst->setLocation(x, y);
    inst->setPlacementStatus(dbPlacementStatus::PLACED);
    inst->setSourceType(odb::dbSourceType::DIST);
    created.push_back(inst);
  }

  filler_count_ = static_cast<int>(created.size());
  have_fillers_ = have_fillers_ || !created.empty();
  if (plan.status == filler_internal::PlannerStatus::partial) {
    logger_->warn(DPL,
                  49,
                  "Left {} legal sites unfilled because fit_space is false.",
                  plan.fillable_sites - plan.covered_sites);
  }
  logger_->info(DPL, 1, "Placed {} filler instances.", filler_count_);
}

void Opendp::setGridCells()
{
  for (Cell& cell : cells_) {
    visitCellPixels(
        cell, false, [&](Pixel* pixel) { setGridCell(cell, pixel); });
  }
}

const char* Opendp::gridInstName(int row, int col)
{
  if (col < 0) {
    return "core_left";
  }
  if (col > row_site_count_) {
    return "core_right";
  }

  const Cell* cell = gridPixel(col, row)->cell;
  if (cell) {
    return cell->db_inst_->getConstName();
  }
  return "?";
}

void Opendp::removeFillers()
{
  block_ = db_->getChip()->getBlock();
  for (odb::dbInst* db_inst : block_->getInsts()) {
    if (isFiller(db_inst)) {
      odb::dbInst::destroy(db_inst);
    }
  }
}

bool Opendp::isFiller(odb::dbInst* db_inst)
{
  dbMaster* db_master = db_inst->getMaster();
  return db_master->getType() == odb::dbMasterType::CORE_SPACER
         // Filter spacer cells used as tapcells.
         && db_inst->getPlacementStatus() != odb::dbPlacementStatus::LOCKED;
}

// Return true if cell is a single site Core Spacer.
bool Opendp::isOneSiteCell(odb::dbMaster* db_master) const
{
  return db_master->getType() == odb::dbMasterType::CORE_SPACER
         && db_master->getWidth() == site_width_;
}

}  // namespace dpl
