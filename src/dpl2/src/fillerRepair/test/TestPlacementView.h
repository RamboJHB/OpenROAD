// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// In-memory PlacementView for unit tests.
//
// A TestPlacementView is built fluently:
//   design.setSiteWidth(1)
//         .addMaster(41, /*w=*/4, /*h=*/1, /*filler=*/true, /*vt=*/1)
//         .addRow(0, 0, 16)
//         .place(100, 41, /*row=*/0, /*x=*/0);
// It is deliberately dumb: no legality checks on construction, so tests can
// build broken layouts (gaps, overlaps) for the pre-check cases.
//
// PlacementView's reference-returning queries are served from caches that
// every mutator invalidates and the next query rebuilds. Unlike runtime
// views this object stays mutable, so it is SINGLE-THREADED by design
// (tests only) -- the thread-safety contract lives with the runtime engine.

#pragma once

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include <fillerRepair/PlacementView.h>

namespace dpl2::fillerRepair {

class TestPlacementView : public PlacementView
{
 public:
  TestPlacementView& setSiteWidth(DbCoord w)
  {
    site_width_ = w;
    return *this;
  }

  TestPlacementView& addMaster(MasterId id, DbCoord width, DbCoord height, bool isFiller, VtId vt,
                        BandPolarity bottomBandPolarity = BandPolarity::N)
  {
    masters_[id] = MasterInfo{id, width, height, isFiller, vt, bottomBandPolarity};
    caches_dirty_ = true;  // master height feeds multi-row bucketing
    return *this;
  }

  TestPlacementView& addRow(RowId id, DbCoord xl, DbCoord xh)
  {
    row_spans_[id] = XInterval{xl, xh};
    caches_dirty_ = true;
    return *this;
  }

  TestPlacementView& place(InstanceId id, MasterId masterId, RowId rowId, DbCoord x,
                    Orient orient = Orient::R0)
  {
    const auto it = masters_.find(masterId);
    const bool isFiller = it != masters_.end() && it->second.isFiller;
    instances_[id] = PlacedInstance{id, masterId, rowId, x, orient, isFiller};
    caches_dirty_ = true;
    return *this;
  }

  TestPlacementView& remove(InstanceId id)
  {
    instances_.erase(id);
    caches_dirty_ = true;
    return *this;
  }

  TestPlacementView& setFillerMasterIds(std::vector<MasterId> ids)
  {
    configured_fillers_ = std::move(ids);
    have_configured_fillers_ = true;
    caches_dirty_ = true;
    return *this;
  }

  // PlacementView -----------------------------------------------------------

  const std::vector<RowId>& rows() const override
  {
    refreshCaches();
    return row_list_;
  }

  DbCoord siteWidth() const override { return site_width_; }

  const std::vector<PlacedInstance>& instancesInRow(RowId rowId) const override
  {
    refreshCaches();
    const auto it = by_row_.find(rowId);
    return it != by_row_.end() ? it->second : emptyInstances();
  }

  const PlacedInstance* instance(InstanceId id) const override
  {
    const auto it = instances_.find(id);
    return it != instances_.end() ? &it->second : nullptr;
  }

  const MasterInfo* masterInfo(MasterId id) const override
  {
    const auto it = masters_.find(id);
    return it != masters_.end() ? &it->second : nullptr;
  }

  const std::vector<MasterId>& fillerMasterIds() const override
  {
    refreshCaches();
    return filler_master_list_;
  }

  FillerCellRecord fillerCellRecord(InstanceId instanceId,
                                    MasterId newMasterId) const override
  {
    const PlacedInstance* placed = instance(instanceId);
    const MasterId originalMaster
        = placed != nullptr ? placed->masterId : MasterId{};
    return FillerCellRecord{OpType::Replace,
                            eUNL::LeafCellID(0, instanceId),
                            eUTL::UvDist(placed != nullptr ? placed->x : 0),
                            eUTL::UvDist(placed != nullptr ? placed->rowId : 0),
                            eLIB::LibCellID(0, originalMaster),
                            eLIB::LibCellID(0, newMasterId)};
  }

 private:
  void refreshCaches() const
  {
    if (!caches_dirty_) {
      return;
    }
    caches_dirty_ = false;

    row_list_.clear();
    row_list_.reserve(row_spans_.size());
    for (const auto& [id, span] : row_spans_) {
      row_list_.push_back(id);
    }

    by_row_.clear();
    for (const auto& [id, inst] : instances_) {
      const MasterInfo* master = masterInfo(inst.masterId);
      const DbCoord height
          = master != nullptr ? std::max<DbCoord>(master->height, 1) : 1;
      // Multi-height contract: reported by every covered row. Rows are
      // bucketed even when not declared via addRow (tests place instances
      // in undeclared rows for boundary cases).
      for (DbCoord offset = 0; offset < height; ++offset) {
        PlacedInstance copy = inst;
        copy.rowId = inst.rowId + static_cast<RowId>(offset);
        by_row_[copy.rowId].push_back(copy);
      }
    }
    for (auto& [rowId, list] : by_row_) {
      std::sort(list.begin(), list.end(),
                [](const PlacedInstance& a, const PlacedInstance& b) {
                  return a.x != b.x ? a.x < b.x : a.id < b.id;
                });
    }

    if (have_configured_fillers_) {
      filler_master_list_ = configured_fillers_;
    } else {
      filler_master_list_.clear();
      for (const auto& [id, info] : masters_) {
        if (info.isFiller) {
          filler_master_list_.push_back(id);
        }
      }
    }
    // Contract: sorted ascending, unique.
    std::sort(filler_master_list_.begin(), filler_master_list_.end());
    filler_master_list_.erase(
        std::unique(filler_master_list_.begin(), filler_master_list_.end()),
        filler_master_list_.end());
  }

  DbCoord site_width_ = 1;
  std::map<MasterId, MasterInfo> masters_;      // ordered => deterministic
  std::map<RowId, XInterval> row_spans_;        // ordered => deterministic
  std::map<InstanceId, PlacedInstance> instances_;
  std::vector<MasterId> configured_fillers_;
  bool have_configured_fillers_ = false;
  // Query caches (single-threaded test object; mutators set the dirty flag).
  mutable bool caches_dirty_ = true;
  mutable std::vector<RowId> row_list_;
  mutable std::map<RowId, std::vector<PlacedInstance>> by_row_;
  mutable std::vector<MasterId> filler_master_list_;
};

}  // namespace dpl2::fillerRepair
