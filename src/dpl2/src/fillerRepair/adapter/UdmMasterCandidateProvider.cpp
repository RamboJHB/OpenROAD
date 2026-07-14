// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "UdmMasterCandidateProvider.h"

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

UdmMasterCandidateProvider::UdmMasterCandidateProvider(
    const ipl::ImplantLayerChecker& checker,
    const CheckerPlacementView& view,
    const UdmIdBridge& bridge,
    dpl2::fillerSetting* setting,
    const DebugLog& log)
    : view_(view), log_(log)
{
  // Optional allow list from the infra fillerSetting: LibCellID -> checker
  // MasterId via the bridge. Unknown entries (no implant shapes -> not in the
  // checker's master set) are reported and skipped.
  std::set<ipl::MasterId> allowed;
  bool haveAllowList = false;
  if (setting != nullptr) {
    haveAllowList = true;
    for (const eLIB::LibCellID libCellId : setting->getFillerCells()) {
      const ipl::MasterId id = bridge.masterIdOf(libCellId);
      if (id >= 0) {
        allowed.insert(id);
      } else {
        log.msg("adapter",
                cat("fillerSetting master libCell ",
                    static_cast<int>(libCellId.getIndexValue()),
                    " has no implant shapes -> not a checker master, skipped"));
      }
    }
  }

  for (const ipl::MasterInput& mi : checker.masters()) {
    if (!mi.isFiller) {
      continue;
    }
    const MasterId id = static_cast<MasterId>(mi.masterId);
    if (haveAllowList && allowed.count(mi.masterId) == 0) {
      continue;
    }
    const MasterInfo* info = view_.masterInfo(id);
    if (info == nullptr || info->vt == kUnknownVt) {
      // A replacement must have a derivable implant family; otherwise the
      // swap's target VT is meaningless to the ranker.
      continue;
    }
    eligible_[id] = Entry{info->width, info->height, info->vt};
  }
  log.msg("adapter",
          cat("candidate provider: ", eligible_.size(),
              " eligible filler master(s)",
              haveAllowList ? " (fillerSetting allow list applied)" : ""));
}

MasterCandidateResult UdmMasterCandidateProvider::getUsableMasterCandidates(
    const MasterCandidateRequest& request) const
{
  MasterCandidateResult result;

  const PlacedInstance* inst = view_.instance(request.fillerInstanceId);
  if (inst == nullptr) {
    result.diagnostics.push_back(
        makeDiag(Severity::Error, "UnknownInstance",
                 cat("instance ", request.fillerInstanceId, " not found")));
    return result;
  }
  if (!inst->isFiller) {
    result.diagnostics.push_back(
        makeDiag(Severity::Warning, "NotAFiller",
                 cat("instance ", request.fillerInstanceId,
                     " is not a filler")));
    return result;
  }
  const MasterInfo* current = view_.masterInfo(inst->masterId);
  if (current == nullptr) {
    result.diagnostics.push_back(
        makeDiag(Severity::Error, "UnknownMaster",
                 cat("master ", inst->masterId, " of instance ",
                     request.fillerInstanceId, " not in the checker set")));
    return result;
  }

  // Same width + height, different (known) VT, ascending id. The current
  // master's own VT may be unknown -- size-matched candidates still apply.
  for (const auto& [id, entry] : eligible_) {
    if (id != inst->masterId && entry.width == current->width
        && entry.height == current->height && entry.vt != current->vt) {
      result.candidates.push_back(MasterCandidate{id});
    }
  }
  if (result.candidates.empty()) {
    result.diagnostics.push_back(
        makeDiag(Severity::Info, "NoUsableMaster",
                 cat("no same-size replacement for instance ",
                     request.fillerInstanceId)));
  }
  return result;
}

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
