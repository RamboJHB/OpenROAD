// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <phys/physDesMgr.hh>

#include "../Log.h"
#include "../PlacementView.h"
#include "drc/ImplantLayerChecker.h"
#include "infrastructure/fillerSetting.h"
#include "infrastructure/network.h"

namespace dpl2::fillerRepair::adapter {

Orient toPlannerOrient(eUTL::PhysOrientation orientation);
eUTL::PhysOrientation toUdmOrient(Orient orient);

// Single immutable boundary shared by precheck, candidate generation and
// checker/planner id conversion. Placement geometry comes from infrastructure
// Objects; checker tables contribute only implant metadata and validation.
class InfrastructurePlacementView : public PlacementView
{
 public:
  InfrastructurePlacementView(const eUNL::PhysDesMgr& desMgr,
                              const dpl2::Network& network,
                              const ipl::ImplantLayerChecker& checker,
                              const dpl2::fillerSetting& fillerSetting,
                              const DebugLog& log);

  bool isValid() const;
  const std::vector<Diagnostic>& setupDiagnostics() const
  {
    return setup_diagnostics_;
  }

  std::vector<RowId> rows() const override;
  XInterval rowLegalSpan(RowId rowId) const override;
  DbCoord siteWidth() const override { return site_width_; }
  std::vector<PlacedInstance> instancesInRow(RowId rowId) const override;
  const PlacedInstance* instance(InstanceId id) const override;
  const MasterInfo* masterInfo(MasterId id) const override;
  std::vector<MasterId> fillerMasterIds() const override
  {
    return filler_master_ids_;
  }
  SiteCoverageResult checkSiteCoverage(const DebugLog& log) const override;

  DbCoord rowHeight() const { return row_height_; }
  InstanceId instanceIdOf(eUNL::LeafCellID cellId) const;
  MasterId masterIdOf(const eLIB::PhysLibCell& master) const;
  eUNL::LeafCellID leafCellOf(InstanceId instanceId) const;
  const eLIB::PhysLibCell* physLibCellOf(MasterId masterId) const;
  // True when the checker carries an implant model for this master -- only
  // such masters can appear in overlay requests the checker can validate.
  bool checkerModelsMaster(MasterId masterId) const
  {
    return checker_master_ids_.count(masterId) != 0;
  }

 private:
  void addProblem(Severity severity,
                  const std::string& code,
                  const std::string& message);

  DbCoord site_width_ = 0;
  DbCoord row_height_ = 0;
  std::map<RowId, XInterval> row_spans_;
  std::map<MasterId, MasterInfo> masters_;
  std::map<MasterId, const eLIB::PhysLibCell*> master_cells_;
  std::map<InstanceId, PlacedInstance> instances_;
  std::map<InstanceId, eUNL::LeafCellID> leaf_cells_;
  std::set<InstanceId> checker_instance_ids_;
  std::set<MasterId> checker_master_ids_;
  std::map<RowId, std::vector<PlacedInstance>> by_row_;
  std::vector<MasterId> filler_master_ids_;
  std::vector<Diagnostic> setup_diagnostics_;
  mutable std::optional<SiteCoverageResult> coverage_cache_;
};

}  // namespace dpl2::fillerRepair::adapter
