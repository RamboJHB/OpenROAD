// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// fillerRepair::FillerMasterCandidateProvider over the checker's UDM-derived
// master tables, optionally intersected with the infra fillerSetting allow
// list (the ECO filler library configured by name).
//
// Semantics (spec 5.3): input is a filler INSTANCE; candidates are filler
// masters with the same width and height, a KNOWN and DIFFERENT implant
// family (VT), excluding the current master, ascending master id. When a
// fillerSetting is provided, only masters whose PhysLibCell is in
// getFillerCells() qualify (the design may contain filler masters the ECO
// flow is not allowed to instantiate). No allow list -> every checker filler
// master qualifies.
//
// Data path mirrors D16's fake-UDM rehearsal, now on real tables:
// width/height/isFiller from checker.masters() (built by initFromUDM from
// PhysLibCell), VT from the implant layer family under the master's shapes.
// The avoid-abut patterns in fillerSetting are a commit/abutment concern and
// deliberately NOT applied here -- the checker judges legality. [VERIFY-UDM]

#pragma once

#include <set>
#include <vector>

#include "../CandidateApi.h"
#include "../Log.h"
#include "CheckerPlacementView.h"
#include "UdmIdBridge.h"
#include "drc/ImplantLayerChecker.h"
#include "infrastructure/fillerSetting.h"

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

class UdmMasterCandidateProvider : public FillerMasterCandidateProvider
{
 public:
  // `setting` may be nullptr (no allow list). The view resolves instances;
  // the bridge maps the allow list's LibCellIDs into checker master ids.
  UdmMasterCandidateProvider(const ipl::ImplantLayerChecker& checker,
                             const CheckerPlacementView& view,
                             const UdmIdBridge& bridge,
                             dpl2::fillerSetting* setting,
                             const DebugLog& log);

  MasterCandidateResult getUsableMasterCandidates(
      const MasterCandidateRequest& request) const override;

 private:
  const CheckerPlacementView& view_;
  const DebugLog& log_;
  // Filler masters eligible as replacements, precomputed: id -> (w, h, vt).
  struct Entry
  {
    DbCoord width = 0;
    DbCoord height = 0;
    VtId vt = kUnknownVt;
  };
  std::map<MasterId, Entry> eligible_;  // ordered -> ascending candidates
};

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
