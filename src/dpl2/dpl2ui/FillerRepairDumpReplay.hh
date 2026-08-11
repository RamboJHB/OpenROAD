#pragma once
// [FRPORT] Optional dump-replay API used by test_filler_repair -load.

#include <iosfwd>
#include <string>

namespace dpl2 {

// [fillerRepair-fix] Data-only control/result types for dump replay. Runtime
// callers continue to use FillerRepairEngine directly.
struct FillerRepairDumpReplayOptions
{
  // Negative values select sweep mode. Otherwise both ids must be present.
  int instanceId = -1;
  int masterId = -1;
  int maxProposals = 2000;
  int maxReportedLines = 50;
};

struct FillerRepairDumpReplayResult
{
  bool completed = false;
  bool passed = false;
  int baselineChecked = 0;
  int baselineIllegal = 0;
  int proposals = 0;
  int cleanRightAway = 0;
  int repaired = 0;
  int unrepairable = 0;
  int totalFillerSwaps = 0;
  std::string error;
};

// Replays checker + pure filler planner directly from a gzip checker-helper
// dump. It needs no loaded design and never mutates the reconstructed model.
FillerRepairDumpReplayResult replayFillerRepairDump(
    const std::string& filePath,
    const FillerRepairDumpReplayOptions& options,
    std::ostream& out);

}  // namespace dpl2
