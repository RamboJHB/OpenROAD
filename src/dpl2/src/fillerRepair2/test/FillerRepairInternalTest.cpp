// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerRepair/PlacementView.h>
#include <fillerRepair/RepairPlanner.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dpl2::fillerRepair {
namespace {

eUTL::PhysOrientation wireOrientation(Orient orientation)
{
  switch (orientation) {
    case Orient::R180:
      return eUTL::PhysOrientationE::R180;
    case Orient::MX:
      return eUTL::PhysOrientationE::MX;
    case Orient::MY:
      return eUTL::PhysOrientationE::MY;
    case Orient::R0:
      return eUTL::PhysOrientationE::R0;
  }
  return eUTL::PhysOrientationE::R0;
}

class TestView final : public PlacementView
{
 public:
  TestView& setSiteWidth(DbCoord width)
  {
    siteWidth_ = width;
    return *this;
  }

  TestView& addRow(RowId row)
  {
    rows_.push_back(row);
    std::sort(rows_.begin(), rows_.end());
    rows_.erase(std::unique(rows_.begin(), rows_.end()), rows_.end());
    return *this;
  }

  TestView& addMaster(MasterId id,
                      DbCoord width,
                      DbCoord height,
                      bool filler,
                      VtId vt,
                      BandPolarity polarity = BandPolarity::N)
  {
    masters_[id] = MasterInfo{id, width, height, filler, vt, polarity};
    if (filler) {
      fillerMasters_.push_back(id);
      std::sort(fillerMasters_.begin(), fillerMasters_.end());
      fillerMasters_.erase(
          std::unique(fillerMasters_.begin(), fillerMasters_.end()),
          fillerMasters_.end());
    }
    return *this;
  }

  TestView& place(InstanceId id,
                  MasterId masterId,
                  RowId row,
                  DbCoord x,
                  Orient orientation = Orient::R0)
  {
    const MasterInfo* master = masterInfo(masterId);
    const PlacedInstance placed{id,
                                masterId,
                                row,
                                x,
                                orientation,
                                master != nullptr && master->isFiller};
    instances_[id] = placed;
    const DbCoord height = master != nullptr
                               ? std::max<DbCoord>(master->height, 1)
                               : 1;
    for (DbCoord offset = 0; offset < height; ++offset) {
      PlacedInstance rowCopy = placed;
      rowCopy.rowId = row + static_cast<RowId>(offset);
      byRow_[rowCopy.rowId].push_back(rowCopy);
      auto& bucket = byRow_[rowCopy.rowId];
      std::sort(bucket.begin(), bucket.end(), [](const auto& a, const auto& b) {
        return a.x != b.x ? a.x < b.x : a.id < b.id;
      });
    }
    return *this;
  }

  const std::vector<RowId>& rows() const override { return rows_; }
  DbCoord siteWidth() const override { return siteWidth_; }

  const std::vector<PlacedInstance>& instancesInRow(
      RowId row) const override
  {
    const auto found = byRow_.find(row);
    return found != byRow_.end() ? found->second : emptyInstances();
  }

  const PlacedInstance* instance(InstanceId id) const override
  {
    const auto found = instances_.find(id);
    return found != instances_.end() ? &found->second : nullptr;
  }

  const MasterInfo* masterInfo(MasterId id) const override
  {
    const auto found = masters_.find(id);
    return found != masters_.end() ? &found->second : nullptr;
  }

  const std::vector<MasterId>& fillerMasterIds() const override
  {
    return fillerMasters_;
  }

  CellChangeRecord cellChangeRecord(InstanceId instanceId,
                                    MasterId newMasterId) const override
  {
    const PlacedInstance* placed = instance(instanceId);
    const MasterId oldMasterId = placed != nullptr ? placed->masterId : -1;
    return {OpType::Replace,
            CellData{eUNL::LeafCellID(0, instanceId)},
            eUTL::UvDist(placed != nullptr ? placed->x : 0),
            eUTL::UvDist(placed != nullptr ? placed->rowId : 0),
            eLIB::LibCellID(0, oldMasterId),
            eLIB::LibCellID(0, newMasterId),
            wireOrientation(placed != nullptr ? placed->orientation
                                              : Orient::R0)};
  }

 private:
  DbCoord siteWidth_ = 1;
  std::vector<RowId> rows_;
  std::map<MasterId, MasterInfo> masters_;
  std::map<InstanceId, PlacedInstance> instances_;
  std::map<RowId, std::vector<PlacedInstance>> byRow_;
  std::vector<MasterId> fillerMasters_;
};

MasterId newMasterId(const CellChangeRecord& change)
{
  return static_cast<MasterId>(change.new_lib_cell_.getIndexValue());
}

Violation originalViolation()
{
  Violation violation;
  violation.ruleId = 7;
  violation.kind = ViolationKind::MinWidth;
  violation.relation = ViolationRelation::IntraRow;
  violation.primaryLayer = 1;
  violation.rowIds = {0};
  violation.xWindow = {0, 4};
  violation.measuredValue = 2;
  violation.requiredValue = 4;
  violation.participants = {
      {1, 10, 0, {0, 2}, false, true},
      {2, 20, 0, {2, 4}, true, false}};
  return violation;
}

TestView makeView()
{
  TestView view;
  view.setSiteWidth(1)
      .addRow(0)
      .addMaster(10, 2, 1, false, 1)
      .addMaster(20, 2, 1, true, 0)
      .addMaster(21, 2, 1, true, 1)
      .addMaster(22, 2, 1, true, 2)
      .addMaster(23, 4, 1, true, 1)
      .addMaster(24, 2, 2, true, 1)
      .addMaster(25, 2, 1, true, 1, BandPolarity::P)
      .addMaster(26, 2, 1, true, 0)
      .place(1, 10, 0, 0)
      .place(2, 20, 0, 2)
      .place(3, 22, 0, 4);
  return view;
}

FillerRepairRequest makeRequest()
{
  FillerRepairRequest request;
  request.targetPlace = {1, 10, 0, 0, Orient::R0};
  request.violations = {originalViolation()};
  return request;
}

class ScriptedOracle final : public RepairOracle
{
 public:
  ScriptedOracle(InstanceId requiredInstance,
                 MasterId requiredMaster,
                 bool alwaysIllegal = false,
                 bool reverseBatch = false,
                 bool wrongEcho = false)
      : requiredInstance_(requiredInstance),
        requiredMaster_(requiredMaster),
        alwaysIllegal_(alwaysIllegal),
        reverseBatch_(reverseBatch),
        wrongEcho_(wrongEcho)
  {
  }

  OracleResult checkPlaceWithOverlay(const OracleRequest& request) override
  {
    ++requestCount_;
    OracleResult result;
    result.requestId = wrongEcho_ ? request.requestId + 100 : request.requestId;
    result.status = OracleStatus::Checked;
    bool solved = false;
    for (const CellChangeRecord& change : request.fillerChanges) {
      solved = solved
               || (cellChangeRecordInstanceId(change) == requiredInstance_
                   && newMasterId(change) == requiredMaster_);
    }
    if (request.fillerChanges.empty() || alwaysIllegal_ || !solved) {
      result.violations = {originalViolation()};
    }
    result.isLegal = result.violations.empty();
    return result;
  }

  std::vector<OracleResult> checkPlaceWithOverlays(
      const std::vector<OracleRequest>& requests) override
  {
    ++batchCount_;
    std::vector<OracleResult> results;
    results.reserve(requests.size());
    for (const OracleRequest& request : requests) {
      results.push_back(checkPlaceWithOverlay(request));
    }
    if (reverseBatch_) {
      std::reverse(results.begin(), results.end());
    }
    return results;
  }

  int requestCount() const { return requestCount_; }
  int batchCount() const { return batchCount_; }

 private:
  InstanceId requiredInstance_;
  MasterId requiredMaster_;
  bool alwaysIllegal_;
  bool reverseBatch_;
  bool wrongEcho_;
  int requestCount_ = 0;
  int batchCount_ = 0;
};

bool hasDiagnostic(const FillerRepairResult& result, const std::string& code)
{
  return std::any_of(result.diagnostics.begin(),
                     result.diagnostics.end(),
                     [&code](const Diagnostic& diagnostic) {
                       return diagnostic.code == code;
                     });
}

RepairConfig quietConfig()
{
  RepairConfig config;
  config.verbose = false;
  config.maxAdaptiveLevels = 0;
  return config;
}

TEST(FillerRepairInternalTest, CandidateProviderFiltersIncompatibleMasters)
{
  const TestView view = makeView();

  const MasterCandidateResult result = view.getUsableMasterCandidates(2);

  EXPECT_EQ(result.candidates, (std::vector<MasterId>{21, 22}));
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_TRUE(view.getUsableMasterCandidates(1).candidates.empty());
  EXPECT_FALSE(view.getUsableMasterCandidates(1).diagnostics.empty());
}

TEST(FillerRepairInternalTest, MakeSwapPreservesGeometryAndRejectsInvalidMoves)
{
  const TestView view = makeView();

  const std::optional<Swap> swap = makeSwap(view, 2, 21);

  ASSERT_TRUE(swap.has_value());
  EXPECT_EQ(swap->instanceId, 2);
  EXPECT_EQ(swap->oldMasterId, 20);
  EXPECT_EQ(swap->newMasterId, 21);
  EXPECT_EQ(swap->rowId, 0);
  EXPECT_EQ(swap->span.xl, 2);
  EXPECT_EQ(swap->span.xh, 4);
  EXPECT_FALSE(makeSwap(view, 1, 21).has_value());
  EXPECT_FALSE(makeSwap(view, 2, 23).has_value());
  EXPECT_FALSE(makeSwap(view, 2, 24).has_value());
  EXPECT_FALSE(makeSwap(view, 2, 20).has_value());
}

TEST(FillerRepairInternalTest, OverlayIdentityIgnoresOrderAndDuplicates)
{
  const Region guard{{0, 8}, 0, 1};
  const Swap first{2, 20, 21, 0, {2, 4}, 0, 1};
  const Swap second{3, 22, 21, 0, {4, 6}, 2, 1};

  const OverlayKey key = overlayKey(guard, {first, second, first});

  EXPECT_EQ(key, overlayKey(guard, {second, first}));
  EXPECT_FALSE(key == overlayKey(Region{{0, 9}, 0, 1}, {first, second}));
  EXPECT_EQ(key.size(), 2U);
}

TEST(FillerRepairInternalTest, WireConversionIsSortedAndKeepsOrientation)
{
  TestView view = makeView();
  view.place(4, 20, 0, 6, Orient::MY);
  const Swap later = *makeSwap(view, 4, 21);
  const Swap earlier = *makeSwap(view, 2, 21);

  const ipl::FillerChanges changes
      = toFillerChanges({later, earlier}, view);

  ASSERT_EQ(changes.size(), 2U);
  EXPECT_EQ(cellChangeRecordInstanceId(changes[0]), 2);
  EXPECT_EQ(cellChangeRecordInstanceId(changes[1]), 4);
  EXPECT_EQ(changes[1].orientation_, eUTL::PhysOrientationE::MY);
}

TEST(FillerRepairInternalTest, EnumerationOrdersSinglesBeforePairs)
{
  const Swap a{2, 20, 21, 0, {2, 4}, 0, 1};
  const Swap b{2, 20, 22, 0, {2, 4}, 0, 2};
  const Swap c{3, 22, 21, 0, {4, 6}, 2, 1};
  const std::vector<FillerDomain> domains{{2, {a, b}}, {3, {c}}};
  RepairConfig config = quietConfig();

  const EnumerationPlan plan
      = enumerateOverlays(domains, config, 10, DebugLog(false));

  ASSERT_EQ(plan.overlays.size(), 5U);
  EXPECT_TRUE(plan.complete);
  ASSERT_EQ(plan.overlays[0].size(), 1U);
  ASSERT_EQ(plan.overlays[1].size(), 1U);
  ASSERT_EQ(plan.overlays[2].size(), 1U);
  ASSERT_EQ(plan.overlays[3].size(), 2U);
  ASSERT_EQ(plan.overlays[4].size(), 2U);
  EXPECT_EQ(plan.overlays[0][0].newMasterId, a.newMasterId);
  EXPECT_EQ(plan.overlays[1][0].newMasterId, b.newMasterId);
  EXPECT_EQ(plan.overlays[2][0].instanceId, c.instanceId);
  EXPECT_EQ(plan.overlays[3][0].newMasterId, a.newMasterId);
  EXPECT_EQ(plan.overlays[3][1].instanceId, c.instanceId);
  EXPECT_EQ(plan.overlays[4][0].newMasterId, b.newMasterId);
  EXPECT_EQ(plan.overlays[4][1].instanceId, c.instanceId);
}

TEST(FillerRepairInternalTest, EmptySnapshotSucceedsWithoutOracleCall)
{
  const TestView view = makeView();
  ScriptedOracle oracle(2, 21);
  internal::RepairPlanner planner(view, oracle, quietConfig());
  FillerRepairRequest request = makeRequest();
  request.violations.clear();

  const FillerRepairResult result = planner.repair(request);

  EXPECT_TRUE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_EQ(oracle.requestCount(), 0);
  EXPECT_TRUE(hasDiagnostic(result, "EmptySnapshot"));
}

TEST(FillerRepairInternalTest, PlannerFindsCheckerApprovedSingleSwap)
{
  const TestView view = makeView();
  const MasterId originalMaster = view.instance(2)->masterId;
  ScriptedOracle oracle(2, 21);
  internal::RepairPlanner planner(view, oracle, quietConfig());

  const FillerRepairResult result = planner.repair(makeRequest());

  ASSERT_TRUE(result.hasSolution);
  ASSERT_EQ(result.changes.size(), 1U);
  EXPECT_EQ(cellChangeRecordInstanceId(result.changes.front()), 2);
  EXPECT_EQ(newMasterId(result.changes.front()), 21);
  EXPECT_EQ(view.instance(2)->masterId, originalMaster);
  EXPECT_GT(oracle.requestCount(), 0);
}

TEST(FillerRepairInternalTest, PlannerMatchesReorderedBatchResultsByRequestId)
{
  const TestView view = makeView();
  ScriptedOracle oracle(2, 21, false, true);
  internal::RepairPlanner planner(view, oracle, quietConfig());

  const FillerRepairResult result = planner.repair(makeRequest());

  ASSERT_TRUE(result.hasSolution);
  ASSERT_EQ(result.changes.size(), 1U);
  EXPECT_EQ(newMasterId(result.changes.front()), 21);
  EXPECT_GT(oracle.batchCount(), 0);
}

TEST(FillerRepairInternalTest, NoSolutionNeverReturnsPartialChanges)
{
  const TestView view = makeView();
  ScriptedOracle oracle(2, 21, true);
  internal::RepairPlanner planner(view, oracle, quietConfig());

  const FillerRepairResult result = planner.repair(makeRequest());

  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_TRUE(hasDiagnostic(result, "NoCleanOverlay"));
}

TEST(FillerRepairInternalTest, OracleProtocolErrorFailsClosed)
{
  const TestView view = makeView();
  ScriptedOracle oracle(2, 21, false, false, true);
  internal::RepairPlanner planner(view, oracle, quietConfig());

  const FillerRepairResult result = planner.repair(makeRequest());

  EXPECT_FALSE(result.hasSolution);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_TRUE(hasDiagnostic(result, "BaselineGateFailed"));
}

}  // namespace
}  // namespace dpl2::fillerRepair
