// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Fake implant overlay checker for planner unit tests.
//
// Purpose: lock the OverlayCheckRequest/CheckResult protocol and give the
// planner a rule-parameterized oracle for unit tests. The rule model is a
// deliberate simplification (single implant band per row, VT id == layer id):
//
//   runs        maximal x-adjacent same-VT stretches per row
//   intra MW    run length < mwIntra                     (ruleId 1)
//   intra MS    gap between same-VT runs < msIntra       (ruleId 2)
//   inter MW    x-overlap of same-VT runs in adjacent rows
//               0 < overlap < mwInter                    (ruleId 3)
//   inter MS    x-distance of disjoint same-VT runs in adjacent
//               rows < msInter (corner touch counts as 0) (ruleId 4)
//
// The real checker owns the true semantics (P/N bands, PRL, LEF58 etc.);
// nothing in the planner may depend on the details above -- that is exactly
// the checker-as-oracle boundary the fake exists to enforce.
//
// Protocol guarantees implemented here and asserted by tests:
//  - every CheckResult echoes the request's requestId;
//  - batch results are returned in input order (planner must not rely on it);
//  - one invalid request affects only its own result;
//  - status != Checked always carries diagnostics;
//  - violations are collected only inside guardRegion.

#pragma once

#include <map>
#include <vector>

#include "../OracleGate.h"
#include "FakeDesign.h"

namespace dpl2::fillerRepair {

struct FakeImplantRules
{
  DbCoord mwIntra = 0;  // 0 disables the rule
  DbCoord msIntra = 0;
  DbCoord mwInter = 0;
  DbCoord msInter = 0;
};

class FakeImplantChecker : public ImplantOverlayChecker
{
 public:
  FakeImplantChecker(const FakeDesign& design, FakeImplantRules rules)
      : design_(design), rules_(rules)
  {
  }

  CheckResult checkPlaceWithOverlay(const OverlayCheckRequest& request) override;
  std::vector<CheckResult> checkPlaceWithOverlays(
      const std::vector<OverlayCheckRequest>& requests) override;

  // Telemetry for tests: total requests evaluated / batch calls made.
  int requestCount() const { return request_count_; }
  int batchCount() const { return batch_count_; }

 private:
  struct Run
  {
    VtId vt = kUnknownVt;
    XInterval span;
    std::vector<const PlacedInstance*> insts;
  };

  CheckResult evaluate(const OverlayCheckRequest& request) const;

  // Effective master of an instance under the overlay: fillerChanges first,
  // then the target-place master override, else the placed master.
  MasterId effectiveMaster(const PlacedInstance& inst,
                           const OverlayCheckRequest& request,
                           const std::map<InstanceId, MasterId>& overlay) const;

  std::vector<Run> buildRuns(RowId rowId,
                             const OverlayCheckRequest& request,
                             const std::map<InstanceId, MasterId>& overlay) const;

  const FakeDesign& design_;
  FakeImplantRules rules_;
  int request_count_ = 0;
  int batch_count_ = 0;
};

}  // namespace dpl2::fillerRepair
