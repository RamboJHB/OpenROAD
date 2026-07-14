// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// 100%-utility pre-check, upstreamed per spec section 6.1 / revision #12.
//
// The planner's runUtilityPreCheck is authoritative but scans every row, so
// it must NOT run once per repair target. Key insight: the repair only ever
// commits SAME-SIZE master swaps, which never change site coverage -- so one
// precheck result stays valid across an entire repair campaign until a
// geometry-changing edit (cell add/remove/move/resize) happens.
//
// This wrapper caches the result keyed by a caller-supplied coverage
// revision stamp. Bump the stamp ONLY on geometry-changing edits, not on
// filler VT swap commits. [VERIFY-UDM] If PhysDesMgr exposes an edit/commit
// counter, use it (over-invalidation is safe, just slower); otherwise the
// integration keeps its own counter.
//
// The view passed in must be coverage-complete: built with the UdmIdBridge
// coverage extras so implant-less placed cells are visible (otherwise every
// such cell reports a false Gap).

#pragma once

#include <cstdint>
#include <optional>

#include "../Log.h"
#include "../PreCheck.h"

namespace dpl2 {
namespace fillerRepair {
namespace adapter {

class UdmPrecheck
{
 public:
  explicit UdmPrecheck(const DebugLog& log) : log_(log) {}

  // Returns the cached result when `coverageRevision` matches the last run;
  // otherwise reruns runUtilityPreCheck over `view` and caches it. The
  // reference stays valid until the next check()/invalidate().
  const SiteCoverageResult& check(const PlacementView& view,
                                  std::uint64_t coverageRevision);

  void invalidate() { revision_.reset(); }

 private:
  std::optional<std::uint64_t> revision_;
  SiteCoverageResult cached_;
  DebugLog log_;
};

}  // namespace adapter
}  // namespace fillerRepair
}  // namespace dpl2
