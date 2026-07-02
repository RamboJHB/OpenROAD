// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Fake infrastructure candidate provider (spec sections 5.3 / 11 TODO 2).
//
// Mirrors the agreed semantics exactly: input must be a filler instance;
// candidates are filler masters with the same width and height, excluding
// the current master, in deterministic (ascending master id) order. A
// non-filler input or a filler without replacements yields empty candidates
// plus a diagnostic -- never an error.

#pragma once

#include "../CandidateApi.h"
#include "../Log.h"
#include "FakeDesign.h"

namespace dpl2::fillerRepair {

class FakeCandidateProvider : public FillerMasterCandidateProvider
{
 public:
  explicit FakeCandidateProvider(const FakeDesign& design) : design_(design) {}

  MasterCandidateResult getUsableMasterCandidates(
      const MasterCandidateRequest& request) const override
  {
    MasterCandidateResult result;

    const PlacedInstance* inst = design_.instance(request.fillerInstanceId);
    if (inst == nullptr) {
      result.diagnostics.push_back(
          makeDiag(Severity::Error, "UnknownInstance",
                   cat("instance ", request.fillerInstanceId, " not found")));
      return result;
    }
    if (!inst->isFiller) {
      result.diagnostics.push_back(
          makeDiag(Severity::Warning, "NotAFiller",
                   cat("instance ", request.fillerInstanceId, " is not a filler")));
      return result;
    }

    const MasterInfo* current = design_.masterInfo(inst->masterId);
    for (const MasterId id : design_.allMasters()) {
      const MasterInfo* master = design_.masterInfo(id);
      if (master->isFiller && id != inst->masterId
          && master->width == current->width
          && master->height == current->height) {
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

 private:
  const FakeDesign& design_;
};

}  // namespace dpl2::fillerRepair
