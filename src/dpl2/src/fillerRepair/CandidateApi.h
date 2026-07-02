// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Abstract infrastructure-facing candidate API (spec section 5.3).
//
// Answers one question: for a known filler instance, which masters can
// replace it directly. Candidates exclude the current master and are
// guaranteed same-width / same-height / site- and orientation-compatible.
// Empty candidates is a normal answer (with diagnostics explaining why),
// not an error.

#pragma once

#include <vector>

#include "Types.h"

namespace dpl2::fillerRepair {

struct MasterCandidateRequest
{
  InstanceId fillerInstanceId = 0;
};

struct MasterCandidate
{
  MasterId masterId = 0;
};

struct MasterCandidateResult
{
  std::vector<MasterCandidate> candidates;
  std::vector<Diagnostic> diagnostics;
};

class FillerMasterCandidateProvider
{
 public:
  virtual ~FillerMasterCandidateProvider() = default;

  virtual MasterCandidateResult getUsableMasterCandidates(
      const MasterCandidateRequest& request) const = 0;
};

}  // namespace dpl2::fillerRepair
