// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// In-memory PlacementView for unit tests (spec section 11 TODO 2).
//
// A FakeDesign is built fluently:
//   design.setSiteWidth(1)
//         .addMaster(41, /*w=*/4, /*h=*/1, /*filler=*/true, /*vt=*/1)
//         .addRow(0, 0, 16)
//         .place(100, 41, /*row=*/0, /*x=*/0);
// It is deliberately dumb: no legality checks on construction, so tests can
// build broken layouts (gaps, overlaps) for the pre-check cases.

#pragma once

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "../PlacementView.h"

namespace dpl2::fillerRepair {

class FakeDesign : public PlacementView
{
 public:
  FakeDesign& setSiteWidth(DbCoord w)
  {
    site_width_ = w;
    return *this;
  }

  FakeDesign& addMaster(MasterId id, DbCoord width, DbCoord height, bool isFiller, VtId vt)
  {
    masters_[id] = MasterInfo{id, width, height, isFiller, vt};
    return *this;
  }

  FakeDesign& addRow(RowId id, DbCoord xl, DbCoord xh)
  {
    row_spans_[id] = XInterval{xl, xh};
    return *this;
  }

  FakeDesign& place(InstanceId id, MasterId masterId, RowId rowId, DbCoord x,
                    Orient orient = Orient::R0)
  {
    const auto it = masters_.find(masterId);
    const bool isFiller = it != masters_.end() && it->second.isFiller;
    instances_[id] = PlacedInstance{id, masterId, rowId, x, orient, isFiller};
    return *this;
  }

  FakeDesign& remove(InstanceId id)
  {
    instances_.erase(id);
    return *this;
  }

  FakeDesign& setFillerMasterIds(std::vector<MasterId> ids)
  {
    configured_fillers_ = std::move(ids);
    have_configured_fillers_ = true;
    return *this;
  }

  // Master ids in deterministic order, useful for test inspection.
  std::vector<MasterId> allMasters() const
  {
    std::vector<MasterId> ids;
    ids.reserve(masters_.size());
    for (const auto& [id, info] : masters_) {
      ids.push_back(id);
    }
    return ids;
  }

  // PlacementView -----------------------------------------------------------

  std::vector<RowId> rows() const override
  {
    std::vector<RowId> ids;
    ids.reserve(row_spans_.size());
    for (const auto& [id, span] : row_spans_) {
      ids.push_back(id);
    }
    return ids;
  }

  XInterval rowLegalSpan(RowId rowId) const override
  {
    const auto it = row_spans_.find(rowId);
    return it != row_spans_.end() ? it->second : XInterval{};
  }

  DbCoord siteWidth() const override { return site_width_; }

  std::vector<PlacedInstance> instancesInRow(RowId rowId) const override
  {
    std::vector<PlacedInstance> result;
    for (const auto& [id, inst] : instances_) {
      const MasterInfo* master = masterInfo(inst.masterId);
      const DbCoord height
          = master != nullptr ? std::max<DbCoord>(master->height, 1) : 1;
      if (rowId >= inst.rowId && rowId < inst.rowId + height) {
        PlacedInstance copy = inst;
        copy.rowId = rowId;
        result.push_back(copy);
      }
    }
    std::sort(result.begin(),
              result.end(),
              [](const PlacedInstance& a, const PlacedInstance& b) {
                return a.x != b.x ? a.x < b.x : a.id < b.id;
              });
    return result;
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

  std::vector<MasterId> fillerMasterIds() const override
  {
    if (have_configured_fillers_) return configured_fillers_;
    std::vector<MasterId> ids;
    for (const auto& [id, info] : masters_) {
      if (info.isFiller) ids.push_back(id);
    }
    return ids;
  }

 private:
  DbCoord site_width_ = 1;
  std::map<MasterId, MasterInfo> masters_;      // ordered => deterministic
  std::map<RowId, XInterval> row_spans_;        // ordered => deterministic
  std::map<InstanceId, PlacedInstance> instances_;
  std::vector<MasterId> configured_fillers_;
  bool have_configured_fillers_ = false;
};

}  // namespace dpl2::fillerRepair
