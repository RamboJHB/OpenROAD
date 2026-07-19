#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

#include "drc/ImplantLayerChecker.h"
#include "drc/ImplantLayerCheckerHelper.h"
#include "fillerRepair/FillerRepairPlanner.h"

namespace dpl2 {
namespace ipl {
namespace {

::Rect makeRect(Dbu xl, Dbu yl, Dbu xh, Dbu yh)
{
  return ::Rect(UvDist(static_cast<int64_t>(xl)),
                UvDist(static_cast<int64_t>(yl)),
                UvDist(static_cast<int64_t>(xh)),
                UvDist(static_cast<int64_t>(yh)));
}

constexpr Dbu SITE_WIDTH = 10;
constexpr Dbu ROW_HEIGHT = 100;
constexpr RowId ROW_COUNT = 8;
constexpr ColId SITE_COUNT = 200;
constexpr Dbu MIN_RULE = 20;

constexpr LayerId F1_LAYER = 1;
constexpr LayerId F2_LAYER = 2;
constexpr LayerId F3_LAYER = 3;

// MasterIds are sequential Network indices (order matches input().masters)
constexpr MasterId C1_MASTER = 0;
constexpr MasterId C2_MASTER = 1;
constexpr MasterId C3_MASTER = 2;
constexpr MasterId F1_FILL_MASTER = 3;
constexpr MasterId F2_FILL_MASTER = 4;
constexpr MasterId F3_FILL_MASTER = 5;

constexpr int F1_WIDTH_RULE = 101;
constexpr int F1_SPACING_RULE = 201;

constexpr RowId INTRA_WIDTH_ROW = 0;
constexpr ColId INTRA_WIDTH_COL = 10;
constexpr RowId INTER_WIDTH_TARGET_ROW = 1;
constexpr RowId INTER_WIDTH_NEIGHBOR_ROW = 2;
constexpr ColId INTER_WIDTH_COL = 30;
constexpr RowId INTRA_SPACING_ROW = 3;
constexpr ColId INTRA_SPACING_COL = 50;
constexpr RowId INTER_SPACING_TARGET_ROW = 4;
constexpr RowId INTER_SPACING_NEIGHBOR_ROW = 5;
constexpr ColId INTER_SPACING_COL = 70;

constexpr RowId NEW_INTRA_WIDTH_ROW = 0;
constexpr ColId NEW_INTRA_WIDTH_COL = 100;
constexpr RowId NEW_INTER_WIDTH_TOP_ROW = 0;
constexpr RowId NEW_INTER_WIDTH_BOTTOM_ROW = 1;
constexpr ColId NEW_INTER_WIDTH_COL = 120;
constexpr RowId NEW_INTRA_SPACING_ROW = 0;
constexpr ColId NEW_INTRA_SPACING_COL = 140;
constexpr RowId NEW_INTER_SPACING_TOP_ROW = 2;
constexpr RowId NEW_INTER_SPACING_BOTTOM_ROW = 3;
constexpr ColId NEW_INTER_SPACING_COL = 160;

constexpr RowId OLD_UNRELATED_ROW = 7;
constexpr ColId OLD_UNRELATED_COL = 190;

const char* denseOverlaySchematic()
{
  return R"(Schematic: dense_overlay_8x200
  Legend: [ 1F1 ] = non-filler one-site cell on F1.
          [ aF1 ] = filler one-site cell on F1.
        1/2/3 use layers F1/F2/F3; a/b/c are F1/F2/F3 fillers.
        * marks the filler changed by the tested candidate.
        Each row is 200 sites wide and 100% occupied.
        Only repair windows are shown.

Background all rows:
Sites:    ... [ 1F1 ][ aF1 ][ 2F2 ][ bF2 ][ 3F3 ][ cF3 ] ...

Intra-row width target, row0 sites 8..13:
Sites:       08      09      10      11      12      13
before:   [ 2F2 ][ bF2 ][ 1F1 ][ bF2*][ 2F2 ][ bF2 ]
clear:    [ 2F2 ][ bF2 ][ 1F1 ][ aF1*][ 2F2 ][ bF2 ]

Inter-row width target, rows1/2 sites 30..31:
Sites:       30      31
row2 before:[ 1F1 ][ bF2*]
row2 clear: [ 1F1 ][ aF1*]
row1 target:[ 1F1 ][ aF1 ]

Intra-row spacing target, row3 sites 48..56:
Sites:       48      49      50      51      52      53      54      55      56
before:   [ bF2 ][ bF2 ][ 1F1 ][ aF1 ][ bF2*][ 1F1 ][ aF1 ][ bF2 ][ 2F2 ]
clear:    [ bF2 ][ bF2 ][ 1F1 ][ aF1 ][ aF1*][ 1F1 ][ aF1 ][ bF2 ][ 2F2 ]

Inter-row spacing target, rows4/5 sites 70..75:
Sites:       70      71      72      73      74      75
row5 before:[ ---- ][ ---- ][ bF2*][ 1F1 ][ aF1 ][ bF2 ]
row5 clear: [ ---- ][ ---- ][ aF1*][ 1F1 ][ aF1 ][ bF2 ]
row4 target:[ 1F1 ][ aF1 ][ 2F2 ][ bF2 ][ 2F2 ][ bF2 ]

New-violation windows:
Intra-row width row0 sites 98..103:
safe:     [ 2F2 ][ bF2 ][ 1F1 ][ aF1*][ 2F2 ][ bF2 ]
bad:      [ 2F2 ][ bF2 ][ 1F1 ][ bF2*][ 2F2 ][ bF2 ]

Inter-row width rows0/1 sites 120..121:
row1 safe:[ bF2*][ bF2 ]
row1 bad: [ aF1*][ bF2 ]
row0 ref: [ 1F1 ][ aF1 ]

Intra-row spacing row0 sites 140..145:
safe:     [ 1F1 ][ aF1 ][ bF2 ][ cF3*][ 1F1 ][ aF1 ]
bad:      [ 1F1 ][ aF1 ][ bF2 ][ aF1*][ 1F1 ][ aF1 ]

Inter-row spacing rows2/3 sites 160..165:
row3 safe:[ ---- ][ ---- ][ bF2 ][ cF3*][ 1F1 ][ aF1 ]
row3 bad: [ ---- ][ ---- ][ bF2 ][ aF1*][ 1F1 ][ aF1 ]
row2 ref: [ 1F1 ][ aF1 ][ 2F2 ][ bF2 ][ 2F2 ][ bF2 ]

Old unrelated baseline violation, row7 sites 189..191:
before:   [ bF2 ][ 1F1 ][ bF2 ]
)";
}

InstanceId instId(RowId rowId, ColId colId)
{
  return rowId * SITE_COUNT + colId;
}

MasterId cellMaster(int layerIndex)
{
  return std::array<MasterId, 3>{C1_MASTER, C2_MASTER, C3_MASTER}[layerIndex];
}

MasterId fillerMaster(int layerIndex)
{
  return std::array<MasterId, 3>{
      F1_FILL_MASTER, F2_FILL_MASTER, F3_FILL_MASTER}[layerIndex];
}

MasterInput master(MasterId masterId,
                   ShapeId shapeBase,
                   LayerId layer,
                   bool isFiller)
{
  MasterInput master;
  master.masterId = masterId;
  master.width = SITE_WIDTH;
  master.height = ROW_HEIGHT;
  master.isFiller = isFiller;
  master.shapes = {
      MasterShape{masterId, shapeBase, layer, makeRect(0, 50, SITE_WIDTH, 100)},
      MasterShape{masterId,
                  static_cast<ShapeId>(shapeBase + 1),
                  layer,
                  makeRect(0, 0, SITE_WIDTH, 50)}};
  return master;
}

Rule rule(int ruleId, RuleSource source, LayerId layer)
{
  Rule rule;
  rule.ruleId = ruleId;
  rule.source = source;
  rule.primaryLayer = layer;
  rule.minValue = MIN_RULE;
  return rule;
}

TrackPattern tracks()
{
  TrackPattern tracks;
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    tracks.layerBySlot[{rowId, BandSlot::Bottom}] = F1_LAYER;
    tracks.layerBySlot[{rowId, BandSlot::Top}] = F1_LAYER;
  }
  for (RowId rowId = 0; rowId + 1 < ROW_COUNT; ++rowId) {
    tracks.activeKindByBoundary[{rowId, rowId + 1}] = Polarity::N;
  }
  return tracks;
}

struct SiteSpec
{
  MasterId masterId = C1_MASTER;
  bool isFiller = false;
};

size_t siteIndex(RowId rowId, ColId colId)
{
  return static_cast<size_t>(rowId * SITE_COUNT + colId);
}

void setSite(std::vector<SiteSpec>& sites,
             RowId rowId,
             ColId colId,
             MasterId masterId,
             bool isFiller)
{
  sites[siteIndex(rowId, colId)] = SiteSpec{masterId, isFiller};
}

void setCell(std::vector<SiteSpec>& sites,
             RowId rowId,
             ColId colId,
             MasterId masterId)
{
  setSite(sites, rowId, colId, masterId, false);
}

void setFiller(std::vector<SiteSpec>& sites,
               RowId rowId,
               ColId colId,
               MasterId masterId)
{
  setSite(sites, rowId, colId, masterId, true);
}

std::vector<PlacedInst> densePlaced()
{
  std::vector<SiteSpec> sites(static_cast<size_t>(ROW_COUNT * SITE_COUNT));
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    for (ColId colId = 0; colId < SITE_COUNT; ++colId) {
      const int layerIndex = (colId / 2) % 3;
      const bool isFiller = colId % 2 == 1;
      sites[siteIndex(rowId, colId)] = SiteSpec{
          isFiller ? fillerMaster(layerIndex) : cellMaster(layerIndex),
          isFiller};
    }
  }

  setCell(sites, INTRA_WIDTH_ROW, 8, C2_MASTER);
  setFiller(sites, INTRA_WIDTH_ROW, 9, F2_FILL_MASTER);
  setCell(sites, INTRA_WIDTH_ROW, 10, C1_MASTER);
  setFiller(sites, INTRA_WIDTH_ROW, 11, F2_FILL_MASTER);
  setCell(sites, INTRA_WIDTH_ROW, 12, C2_MASTER);
  setFiller(sites, INTRA_WIDTH_ROW, 13, F2_FILL_MASTER);

  setCell(sites, INTER_WIDTH_TARGET_ROW, 30, C1_MASTER);
  setFiller(sites, INTER_WIDTH_TARGET_ROW, 31, F1_FILL_MASTER);
  setCell(sites, INTER_WIDTH_NEIGHBOR_ROW, 30, C1_MASTER);
  setFiller(sites, INTER_WIDTH_NEIGHBOR_ROW, 31, F2_FILL_MASTER);

  for (RowId rowId : {INTRA_SPACING_ROW - 1, INTRA_SPACING_ROW + 1}) {
    for (ColId colId = 48; colId <= 56; ++colId) {
      if (colId % 2 == 0) {
        setCell(sites, rowId, colId, C2_MASTER);
      } else {
        setFiller(sites, rowId, colId, F2_FILL_MASTER);
      }
    }
  }
  setFiller(sites, INTRA_SPACING_ROW, 48, F2_FILL_MASTER);
  setFiller(sites, INTRA_SPACING_ROW, 49, F2_FILL_MASTER);
  setCell(sites, INTRA_SPACING_ROW, 50, C1_MASTER);
  setFiller(sites, INTRA_SPACING_ROW, 51, F1_FILL_MASTER);
  setFiller(sites, INTRA_SPACING_ROW, 52, F2_FILL_MASTER);
  setCell(sites, INTRA_SPACING_ROW, 53, C1_MASTER);
  setFiller(sites, INTRA_SPACING_ROW, 54, F1_FILL_MASTER);
  setFiller(sites, INTRA_SPACING_ROW, 55, F2_FILL_MASTER);
  setCell(sites, INTRA_SPACING_ROW, 56, C2_MASTER);

  setCell(sites, INTER_SPACING_TARGET_ROW, 70, C1_MASTER);
  setFiller(sites, INTER_SPACING_TARGET_ROW, 71, F1_FILL_MASTER);
  setCell(sites, INTER_SPACING_TARGET_ROW, 72, C2_MASTER);
  setFiller(sites, INTER_SPACING_TARGET_ROW, 73, F2_FILL_MASTER);
  setCell(sites, INTER_SPACING_TARGET_ROW, 74, C2_MASTER);
  setFiller(sites, INTER_SPACING_TARGET_ROW, 75, F2_FILL_MASTER);
  setFiller(sites, INTER_SPACING_NEIGHBOR_ROW, 72, F2_FILL_MASTER);
  setCell(sites, INTER_SPACING_NEIGHBOR_ROW, 73, C1_MASTER);
  setFiller(sites, INTER_SPACING_NEIGHBOR_ROW, 74, F1_FILL_MASTER);
  setFiller(sites, INTER_SPACING_NEIGHBOR_ROW, 75, F2_FILL_MASTER);

  setCell(sites, NEW_INTRA_WIDTH_ROW, 98, C2_MASTER);
  setFiller(sites, NEW_INTRA_WIDTH_ROW, 99, F2_FILL_MASTER);
  setCell(sites, NEW_INTRA_WIDTH_ROW, 100, C1_MASTER);
  setFiller(sites, NEW_INTRA_WIDTH_ROW, 101, F1_FILL_MASTER);
  setCell(sites, NEW_INTRA_WIDTH_ROW, 102, C2_MASTER);
  setFiller(sites, NEW_INTRA_WIDTH_ROW, 103, F2_FILL_MASTER);

  setCell(sites, NEW_INTER_WIDTH_TOP_ROW, 120, C1_MASTER);
  setFiller(sites, NEW_INTER_WIDTH_TOP_ROW, 121, F1_FILL_MASTER);
  setFiller(sites, NEW_INTER_WIDTH_BOTTOM_ROW, 120, F2_FILL_MASTER);
  setFiller(sites, NEW_INTER_WIDTH_BOTTOM_ROW, 121, F2_FILL_MASTER);

  setCell(sites, NEW_INTRA_SPACING_ROW, 140, C1_MASTER);
  setFiller(sites, NEW_INTRA_SPACING_ROW, 141, F1_FILL_MASTER);
  setFiller(sites, NEW_INTRA_SPACING_ROW, 142, F2_FILL_MASTER);
  setFiller(sites, NEW_INTRA_SPACING_ROW, 143, F3_FILL_MASTER);
  setCell(sites, NEW_INTRA_SPACING_ROW, 144, C1_MASTER);
  setFiller(sites, NEW_INTRA_SPACING_ROW, 145, F1_FILL_MASTER);

  setCell(sites, NEW_INTER_SPACING_TOP_ROW, 160, C1_MASTER);
  setFiller(sites, NEW_INTER_SPACING_TOP_ROW, 161, F1_FILL_MASTER);
  setCell(sites, NEW_INTER_SPACING_TOP_ROW, 162, C2_MASTER);
  setFiller(sites, NEW_INTER_SPACING_TOP_ROW, 163, F2_FILL_MASTER);
  setCell(sites, NEW_INTER_SPACING_TOP_ROW, 164, C2_MASTER);
  setFiller(sites, NEW_INTER_SPACING_TOP_ROW, 165, F2_FILL_MASTER);
  setFiller(sites, NEW_INTER_SPACING_BOTTOM_ROW, 162, F2_FILL_MASTER);
  setFiller(sites, NEW_INTER_SPACING_BOTTOM_ROW, 163, F3_FILL_MASTER);
  setCell(sites, NEW_INTER_SPACING_BOTTOM_ROW, 164, C1_MASTER);
  setFiller(sites, NEW_INTER_SPACING_BOTTOM_ROW, 165, F1_FILL_MASTER);

  // Old unrelated violation inside the full guard. It must be present in the
  // baseline but filtered from successful overlay results.
  setFiller(sites, OLD_UNRELATED_ROW, 189, F2_FILL_MASTER);
  setCell(sites, OLD_UNRELATED_ROW, 190, C1_MASTER);
  setFiller(sites, OLD_UNRELATED_ROW, 191, F2_FILL_MASTER);

  std::vector<PlacedInst> placed;
  placed.reserve(static_cast<size_t>(ROW_COUNT * SITE_COUNT));
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    for (ColId colId = 0; colId < SITE_COUNT; ++colId) {
      const SiteSpec spec = sites[siteIndex(rowId, colId)];
      placed.push_back(PlacedInst{instId(rowId, colId),
                                  spec.masterId,
                                  rowId,
                                  colId,
                                  PhysOrientationE::R0,
                                  spec.isFiller});
    }
  }
  return placed;
}

ImplantInput input()
{
  ImplantInput input;
  input.layers = {ImplantLayer{F1_LAYER, "F1", Family::VTL, Polarity::N},
                  ImplantLayer{F2_LAYER, "F2", Family::VTH, Polarity::N},
                  ImplantLayer{F3_LAYER, "F3", Family::VTUL, Polarity::N}};
  input.rules = {rule(F1_WIDTH_RULE, RuleSource::Width, F1_LAYER),
                 rule(102, RuleSource::Width, F2_LAYER),
                 rule(103, RuleSource::Width, F3_LAYER),
                 rule(F1_SPACING_RULE, RuleSource::Spacing, F1_LAYER),
                 rule(202, RuleSource::Spacing, F2_LAYER),
                 rule(203, RuleSource::Spacing, F3_LAYER)};
  input.masters = {master(C1_MASTER, 1, F1_LAYER, false),
                   master(C2_MASTER, 3, F2_LAYER, false),
                   master(C3_MASTER, 5, F3_LAYER, false),
                   master(F1_FILL_MASTER, 7, F1_LAYER, true),
                   master(F2_FILL_MASTER, 9, F2_LAYER, true),
                   master(F3_FILL_MASTER, 11, F3_LAYER, true)};
  input.placedInsts = densePlaced();
  for (RowId rowId = 0; rowId < ROW_COUNT; ++rowId) {
    input.rows.push_back(rowId);
  }
  input.tracks = tracks();
  input.rowHeight = ROW_HEIGHT;
  input.siteWidth = SITE_WIDTH;
  return input;
}

Rect guard()
{
  return makeRect(0, 0, SITE_COUNT * SITE_WIDTH, ROW_COUNT * ROW_HEIGHT);
}

CheckRequest request(RowId rowId, ColId colId)
{
  return CheckRequest{
      instId(rowId, colId), C1_MASTER, rowId, colId, PhysOrientationE::R0};
}

bool hasViolation(const CheckResult& result,
                  int ruleId,
                  Relationship relationship,
                  std::initializer_list<InstanceId> instanceIds)
{
  return std::any_of(
      result.violations.begin(),
      result.violations.end(),
      [ruleId, relationship, instanceIds](const Violation& violation) {
        if (violation.ruleId != ruleId
            || violation.relationship != relationship) {
          return false;
        }
        for (InstanceId instanceId : instanceIds) {
          if (std::find(violation.instances.begin(),
                        violation.instances.end(),
                        instanceId)
              == violation.instances.end()) {
            return false;
          }
        }
        return true;
      });
}

void expectOldUnrelatedFiltered(const CheckResult& result)
{
  EXPECT_FALSE(hasViolation(result,
                            F1_WIDTH_RULE,
                            Relationship::IntraRow,
                            {instId(OLD_UNRELATED_ROW, OLD_UNRELATED_COL)}));
}

LeafCellID leafCellId(RowId rowId, ColId colId)
{
  return LeafCellID(0, instId(rowId, colId));
}

LibCellID libCellId(MasterId masterId)
{
  return LibCellID(0, masterId);
}

std::vector<CheckResult> check(const CheckRequest& request,
                               std::vector<FillerChanges> changes)
{
  SCOPED_TRACE(denseOverlaySchematic());
  const ImplantInput in = input();
  ImplantLayerCheckerHelper helper;
  helper.initialize(in);
  ImplantLayerChecker checker(helper.getGrid(), helper.getNetwork());
  helper.initChecker(checker);
  EXPECT_TRUE(checker.getDiags().empty());
  return checker.checkPlaceWithOverlays(request, guard(), changes);
}

namespace fr = ::dpl2::fillerRepair;

fr::Orient toPlannerOrient(PhysOrientation orientation)
{
  if (orientation == PhysOrientationE::R180)
    return fr::Orient::R180;
  if (orientation == PhysOrientationE::MX)
    return fr::Orient::MX;
  if (orientation == PhysOrientationE::MY)
    return fr::Orient::MY;
  return fr::Orient::R0;
}

PhysOrientation toCheckerOrient(fr::Orient orientation)
{
  switch (orientation) {
    case fr::Orient::R180:
      return PhysOrientationE::R180;
    case fr::Orient::MX:
      return PhysOrientationE::MX;
    case fr::Orient::MY:
      return PhysOrientationE::MY;
    case fr::Orient::R0:
      break;
  }
  return PhysOrientationE::R0;
}

// Portable test view built from the same ImplantInput consumed by the final
// checker's official test helper.  It deliberately implements only the pure
// planner boundary: no UDM session, DEF/LEF reader, or database mutation is
// required to exercise planner -> overlay checker end to end.
class PortablePlacementView final : public fr::PlacementView
{
 public:
  explicit PortablePlacementView(const ImplantInput& input)
      : site_width_(input.siteWidth)
  {
    std::map<LayerId, ImplantLayer> layers;
    for (const ImplantLayer& layer : input.layers) {
      layers[layer.id] = layer;
    }

    for (size_t index = 0; index < input.masters.size(); ++index) {
      const MasterItem& master = input.masters[index];
      fr::MasterInfo info;
      info.id = master.masterId;
      info.width = master.width;
      info.height = std::max<Dbu>(
          1, input.rowHeight > 0 ? master.height / input.rowHeight : 1);
      info.isFiller = master.isFiller;
      if (!master.shapes.empty()) {
        const auto layer = layers.find(master.shapes.front().layer);
        if (layer != layers.end()) {
          info.vt = static_cast<fr::VtId>(layer->second.family);
          info.bottomBandPolarity = layer->second.polarity == Polarity::P
                                        ? fr::BandPolarity::P
                                        : fr::BandPolarity::N;
        }
      }
      masters_[info.id] = info;
      if (info.isFiller)
        filler_master_ids_.push_back(info.id);
    }

    for (RowId rowId : input.rows) {
      rows_.push_back(static_cast<fr::RowId>(rowId));
      row_spans_[static_cast<fr::RowId>(rowId)]
          = fr::XInterval{0, SITE_COUNT * input.siteWidth};
    }
    std::sort(rows_.begin(), rows_.end());
    std::sort(filler_master_ids_.begin(), filler_master_ids_.end());

    for (const PlacedInst& placed : input.placedInsts) {
      fr::PlacedInstance instance;
      instance.id = placed.instanceId;
      instance.masterId = placed.masterId;
      instance.rowId = placed.rowId;
      instance.x = placed.colId * input.siteWidth;
      instance.orientation = toPlannerOrient(placed.orientation);
      instance.isFiller = placed.isFiller;
      instances_[instance.id] = instance;
      by_row_[instance.rowId].push_back(instance);
    }
    for (auto& [rowId, instances] : by_row_) {
      (void) rowId;
      std::sort(
          instances.begin(),
          instances.end(),
          [](const fr::PlacedInstance& left, const fr::PlacedInstance& right) {
            return left.x != right.x ? left.x < right.x : left.id < right.id;
          });
    }
  }

  const std::vector<fr::RowId>& rows() const override { return rows_; }

  fr::XInterval rowLegalSpan(fr::RowId rowId) const override
  {
    const auto found = row_spans_.find(rowId);
    return found == row_spans_.end() ? fr::XInterval{} : found->second;
  }

  fr::DbCoord siteWidth() const override { return site_width_; }

  const std::vector<fr::PlacedInstance>& instancesInRow(
      fr::RowId rowId) const override
  {
    const auto found = by_row_.find(rowId);
    return found == by_row_.end() ? emptyInstances() : found->second;
  }

  const fr::PlacedInstance* instance(fr::InstanceId id) const override
  {
    const auto found = instances_.find(id);
    return found == instances_.end() ? nullptr : &found->second;
  }

  const fr::MasterInfo* masterInfo(fr::MasterId id) const override
  {
    const auto found = masters_.find(id);
    return found == masters_.end() ? nullptr : &found->second;
  }

  const std::vector<fr::MasterId>& fillerMasterIds() const override
  {
    return filler_master_ids_;
  }

 private:
  fr::DbCoord site_width_ = 0;
  std::vector<fr::RowId> rows_;
  std::map<fr::RowId, fr::XInterval> row_spans_;
  std::map<fr::MasterId, fr::MasterInfo> masters_;
  std::map<fr::InstanceId, fr::PlacedInstance> instances_;
  std::map<fr::RowId, std::vector<fr::PlacedInstance>> by_row_;
  std::vector<fr::MasterId> filler_master_ids_;
};

class PortableCheckerOracle final : public fr::ImplantOverlayChecker
{
 public:
  PortableCheckerOracle(const PortablePlacementView& view,
                        const ImplantLayerChecker& checker)
      : view_(view), checker_(checker)
  {
  }

  fr::CheckResult checkPlaceWithOverlay(
      const fr::OverlayCheckRequest& request) override
  {
    std::vector<fr::CheckResult> results = checkPlaceWithOverlays({request});
    return results.empty() ? fr::CheckResult{} : std::move(results.front());
  }

  std::vector<fr::CheckResult> checkPlaceWithOverlays(
      const std::vector<fr::OverlayCheckRequest>& requests) override
  {
    std::vector<fr::CheckResult> results(requests.size());
    if (requests.empty())
      return results;

    const fr::OverlayCheckRequest& first = requests.front();
    std::vector<FillerChanges> changes;
    changes.reserve(requests.size());
    for (const fr::OverlayCheckRequest& request : requests) {
      FillerChanges candidate;
      candidate.reserve(request.fillerChanges.size());
      for (const fr::FillerChange& change : request.fillerChanges) {
        const fr::PlacedInstance* instance = view_.instance(change.instanceId);
        if (instance == nullptr) {
          continue;
        }
        candidate.push_back(FillerCellRecord{
            OpType::Replace,
            leafCellId(instance->rowId,
                       static_cast<ColId>(instance->x / view_.siteWidth())),
            UvDist(static_cast<int32_t>(instance->x)),
            UvDist(static_cast<int32_t>(instance->rowId * ROW_HEIGHT)),
            libCellId(instance->masterId),
            libCellId(change.newMasterId)});
      }
      changes.push_back(std::move(candidate));
    }

    const CheckRequest target{
        first.targetPlace.instanceId,
        first.targetPlace.masterId,
        first.targetPlace.rowId,
        static_cast<ColId>(first.targetPlace.x / view_.siteWidth()),
        toCheckerOrient(first.targetPlace.orientation)};
    const ::Rect guardRect
        = makeRect(first.guardRegion.x.xl,
                   first.guardRegion.rowLo * ROW_HEIGHT,
                   first.guardRegion.x.xh,
                   (first.guardRegion.rowHi + 1) * ROW_HEIGHT - 1);
    const std::vector<CheckResult> raw
        = checker_.checkPlaceWithOverlays(target, guardRect, changes);

    for (size_t index = 0; index < requests.size(); ++index) {
      fr::CheckResult& result = results[index];
      result.requestId = requests[index].requestId;
      if (index >= raw.size()) {
        result.status = fr::CheckStatus::CheckerError;
        result.diagnostics.push_back(
            fr::makeDiag(fr::Severity::Fatal,
                         "CheckerProtocolError",
                         "checker result count does not match request count"));
        continue;
      }
      const CheckResult& checkerResult = raw[index];
      for (const Diagnostic& diagnostic : checkerResult.diagnostics) {
        result.diagnostics.push_back(fr::makeDiag(
            fr::Severity::Warning, diagnostic.status, diagnostic.message));
      }
      for (const Violation& violation : checkerResult.violations) {
        result.violations.push_back(
            toPlannerViolation(violation, first.targetPlace.instanceId));
      }
      if (result.violations.empty() && !checkerResult.isLegal
          && !result.diagnostics.empty()) {
        result.status = fr::CheckStatus::InvalidOverlay;
        result.isLegal = false;
      } else {
        result.status = fr::CheckStatus::Checked;
        result.isLegal
            = result.violations.empty() && result.diagnostics.empty();
      }
    }
    return results;
  }

 private:
  fr::Violation toPlannerViolation(const Violation& violation,
                                   fr::InstanceId targetId) const
  {
    fr::Violation result;
    result.ruleId = violation.ruleId;
    result.kind = violation.ruleSource == RuleSource::Width
                          || violation.ruleSource == RuleSource::Lef58Width
                      ? fr::ViolationKind::MinWidth
                      : fr::ViolationKind::MinSpacing;
    result.relation = violation.relationship == Relationship::InterRow
                          ? fr::ViolationRelation::InterRow
                          : fr::ViolationRelation::IntraRow;
    result.primaryLayer = violation.primaryLayer;
    result.secondaryLayer = violation.secondaryLayer;
    result.xWindow = {violation.xWindow.xl, violation.xWindow.xh};
    result.measuredValue = violation.measuredValue;
    result.requiredValue = violation.requiredValue;
    result.rowIds.assign(violation.rowIds.begin(), violation.rowIds.end());
    for (InstanceId id : violation.instances) {
      fr::ViolationParticipant participant;
      participant.instanceId = id;
      participant.isTarget = id == targetId;
      if (const fr::PlacedInstance* instance = view_.instance(id)) {
        participant.masterId = instance->masterId;
        participant.rowId = instance->rowId;
        participant.isFiller = instance->isFiller;
        const fr::MasterInfo* master = view_.masterInfo(instance->masterId);
        const fr::DbCoord width = master == nullptr ? 0 : master->width;
        participant.xRange = {instance->x, instance->x + width};
      }
      result.participants.push_back(participant);
    }
    return result;
  }

  const PortablePlacementView& view_;
  const ImplantLayerChecker& checker_;
};

fr::TargetPlace plannerTarget(RowId rowId, ColId colId)
{
  return fr::TargetPlace{instId(rowId, colId),
                         C1_MASTER,
                         rowId,
                         colId * SITE_WIDTH,
                         fr::Orient::R0};
}

fr::Region snapshotRegion(RowId rowId, ColId colId)
{
  return fr::Region{
      fr::XInterval{std::max<Dbu>(0, colId * SITE_WIDTH - 4 * MIN_RULE),
                    std::min<Dbu>(SITE_COUNT * SITE_WIDTH,
                                  (colId + 1) * SITE_WIDTH + 4 * MIN_RULE)},
      std::max<RowId>(0, rowId - 1),
      std::min<RowId>(ROW_COUNT - 1, rowId + 1)};
}

void expectPlannerRepairsWithFinalChecker(RowId rowId, ColId colId)
{
  SCOPED_TRACE(denseOverlaySchematic());
  const ImplantInput immutableInput = input();
  const std::vector<PlacedInst> before = immutableInput.placedInsts;
  ImplantLayerCheckerHelper helper;
  helper.initialize(immutableInput);
  ImplantLayerChecker checker(helper.getGrid(), helper.getNetwork());
  helper.initChecker(checker);
  ASSERT_TRUE(checker.getDiags().empty());

  PortablePlacementView view(immutableInput);
  PortableCheckerOracle oracle(view, checker);
  const fr::TargetPlace target = plannerTarget(rowId, colId);
  const fr::Region snapshot = snapshotRegion(rowId, colId);
  fr::OverlayCheckRequest baselineRequest{0, target, snapshot, {}};
  const fr::CheckResult baseline
      = oracle.checkPlaceWithOverlay(baselineRequest);
  ASSERT_EQ(baseline.status, fr::CheckStatus::Checked);
  ASSERT_FALSE(baseline.isLegal);
  ASSERT_FALSE(baseline.violations.empty());

  fr::internal::FillerRepairPlanner planner(view, oracle);
  const fr::FillerRepairResult repaired
      = planner.repair(fr::FillerRepairRequest{target, baseline.violations});
  ASSERT_TRUE(repaired.hasSolution);
  ASSERT_FALSE(repaired.changes.empty());

  fr::OverlayCheckRequest verifyRequest{1, target, snapshot, repaired.changes};
  const fr::CheckResult verified = oracle.checkPlaceWithOverlay(verifyRequest);
  EXPECT_EQ(verified.status, fr::CheckStatus::Checked);
  EXPECT_TRUE(verified.isLegal);
  EXPECT_TRUE(verified.violations.empty());

  ASSERT_EQ(immutableInput.placedInsts.size(), before.size());
  for (size_t index = 0; index < before.size(); ++index) {
    EXPECT_EQ(immutableInput.placedInsts[index].instanceId,
              before[index].instanceId);
    EXPECT_EQ(immutableInput.placedInsts[index].masterId,
              before[index].masterId);
    EXPECT_EQ(immutableInput.placedInsts[index].rowId, before[index].rowId);
    EXPECT_EQ(immutableInput.placedInsts[index].colId, before[index].colId);
  }
}

TEST(ImplantCheckerOverlayTest, DenseCaseIntraRowWidth)
{
  const CheckRequest target = request(INTRA_WIDTH_ROW, INTRA_WIDTH_COL);
  const std::vector<CheckResult> results
      = check(target,
              {{FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_WIDTH_ROW, 11),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_WIDTH_ROW, 11),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F2_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_WIDTH_ROW, 11),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)},
                FillerCellRecord{OpType::Replace,
                                 leafCellId(NEW_INTRA_WIDTH_ROW, 101),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F2_FILL_MASTER)}}});

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].violations.empty());

  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasViolation(
      results[1], F1_WIDTH_RULE, Relationship::IntraRow, {target.instanceId}));

  EXPECT_FALSE(results[2].isLegal);
  EXPECT_FALSE(hasViolation(
      results[2], F1_WIDTH_RULE, Relationship::IntraRow, {target.instanceId}));
  EXPECT_TRUE(hasViolation(results[2],
                           F1_WIDTH_RULE,
                           Relationship::IntraRow,
                           {instId(NEW_INTRA_WIDTH_ROW, NEW_INTRA_WIDTH_COL)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST(ImplantCheckerOverlayTest, DenseCaseInterRowWidth)
{
  const CheckRequest target = request(INTER_WIDTH_TARGET_ROW, INTER_WIDTH_COL);
  const InstanceId neighbor = instId(INTER_WIDTH_NEIGHBOR_ROW, INTER_WIDTH_COL);
  const std::vector<CheckResult> results
      = check(target,
              {{FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_WIDTH_NEIGHBOR_ROW, 31),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_WIDTH_NEIGHBOR_ROW, 31),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F2_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_WIDTH_NEIGHBOR_ROW, 31),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)},
                FillerCellRecord{
                    OpType::Replace,
                    leafCellId(NEW_INTER_WIDTH_BOTTOM_ROW, NEW_INTER_WIDTH_COL),
                    UvDist(0),
                    UvDist(0),
                    LibCellID(),
                    libCellId(F1_FILL_MASTER)}}});

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].violations.empty());

  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasViolation(results[1],
                           F1_WIDTH_RULE,
                           Relationship::InterRow,
                           {target.instanceId, neighbor}));

  EXPECT_FALSE(results[2].isLegal);
  EXPECT_FALSE(hasViolation(results[2],
                            F1_WIDTH_RULE,
                            Relationship::InterRow,
                            {target.instanceId, neighbor}));
  EXPECT_TRUE(
      hasViolation(results[2],
                   F1_WIDTH_RULE,
                   Relationship::InterRow,
                   {instId(NEW_INTER_WIDTH_TOP_ROW, NEW_INTER_WIDTH_COL),
                    instId(NEW_INTER_WIDTH_BOTTOM_ROW, NEW_INTER_WIDTH_COL)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST(ImplantCheckerOverlayTest, DenseCaseIntraRowSpacing)
{
  const CheckRequest target = request(INTRA_SPACING_ROW, INTRA_SPACING_COL);
  const InstanceId neighbor = instId(INTRA_SPACING_ROW, INTRA_SPACING_COL + 3);
  const std::vector<CheckResult> results
      = check(target,
              {{FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_SPACING_ROW, 52),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_SPACING_ROW, 52),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F2_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTRA_SPACING_ROW, 52),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)},
                FillerCellRecord{OpType::Replace,
                                 leafCellId(NEW_INTRA_SPACING_ROW, 143),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}}});

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].violations.empty());

  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasViolation(results[1],
                           F1_SPACING_RULE,
                           Relationship::IntraRow,
                           {target.instanceId, neighbor}));

  EXPECT_FALSE(results[2].isLegal);
  EXPECT_FALSE(hasViolation(results[2],
                            F1_SPACING_RULE,
                            Relationship::IntraRow,
                            {target.instanceId, neighbor}));
  EXPECT_TRUE(
      hasViolation(results[2],
                   F1_SPACING_RULE,
                   Relationship::IntraRow,
                   {instId(NEW_INTRA_SPACING_ROW, NEW_INTRA_SPACING_COL),
                    instId(NEW_INTRA_SPACING_ROW, NEW_INTRA_SPACING_COL + 4)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST(ImplantCheckerOverlayTest, DenseCaseInterRowSpacing)
{
  const CheckRequest target
      = request(INTER_SPACING_TARGET_ROW, INTER_SPACING_COL);
  const InstanceId neighbor
      = instId(INTER_SPACING_NEIGHBOR_ROW, INTER_SPACING_COL + 3);
  const std::vector<CheckResult> results
      = check(target,
              {{FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_SPACING_NEIGHBOR_ROW, 72),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_SPACING_NEIGHBOR_ROW, 72),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F2_FILL_MASTER)}},
               {FillerCellRecord{OpType::Replace,
                                 leafCellId(INTER_SPACING_NEIGHBOR_ROW, 72),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)},
                FillerCellRecord{OpType::Replace,
                                 leafCellId(NEW_INTER_SPACING_BOTTOM_ROW, 163),
                                 UvDist(0),
                                 UvDist(0),
                                 LibCellID(),
                                 libCellId(F1_FILL_MASTER)}}});

  ASSERT_EQ(results.size(), 3u);
  EXPECT_TRUE(results[0].isLegal);
  EXPECT_TRUE(results[0].violations.empty());

  EXPECT_FALSE(results[1].isLegal);
  EXPECT_TRUE(hasViolation(results[1],
                           F1_SPACING_RULE,
                           Relationship::InterRow,
                           {target.instanceId, neighbor}));

  EXPECT_FALSE(results[2].isLegal);
  EXPECT_FALSE(hasViolation(results[2],
                            F1_SPACING_RULE,
                            Relationship::InterRow,
                            {target.instanceId, neighbor}));
  EXPECT_TRUE(hasViolation(
      results[2],
      F1_SPACING_RULE,
      Relationship::InterRow,
      {instId(NEW_INTER_SPACING_TOP_ROW, NEW_INTER_SPACING_COL),
       instId(NEW_INTER_SPACING_BOTTOM_ROW, NEW_INTER_SPACING_COL + 4)}));
  expectOldUnrelatedFiltered(results[0]);
}

TEST(FillerRepairCheckerE2ETest, RepairsIntraRowWidth)
{
  const RowId rowId = INTRA_WIDTH_ROW;
  const ColId colId = INTRA_WIDTH_COL;
  ASSERT_EQ(instId(rowId, colId), instId(INTRA_WIDTH_ROW, INTRA_WIDTH_COL));
  expectPlannerRepairsWithFinalChecker(rowId, colId);
  SUCCEED();
}

TEST(FillerRepairCheckerE2ETest, RepairsInterRowWidth)
{
  const RowId rowId = INTER_WIDTH_TARGET_ROW;
  const ColId colId = INTER_WIDTH_COL;
  ASSERT_EQ(instId(rowId, colId),
            instId(INTER_WIDTH_TARGET_ROW, INTER_WIDTH_COL));
  expectPlannerRepairsWithFinalChecker(rowId, colId);
  SUCCEED();
}

TEST(FillerRepairCheckerE2ETest, RepairsIntraRowSpacing)
{
  const RowId rowId = INTRA_SPACING_ROW;
  const ColId colId = INTRA_SPACING_COL;
  ASSERT_EQ(instId(rowId, colId), instId(INTRA_SPACING_ROW, INTRA_SPACING_COL));
  expectPlannerRepairsWithFinalChecker(rowId, colId);
  SUCCEED();
}

TEST(FillerRepairCheckerE2ETest, RepairsInterRowSpacing)
{
  const RowId rowId = INTER_SPACING_TARGET_ROW;
  const ColId colId = INTER_SPACING_COL;
  ASSERT_EQ(instId(rowId, colId),
            instId(INTER_SPACING_TARGET_ROW, INTER_SPACING_COL));
  expectPlannerRepairsWithFinalChecker(rowId, colId);
  SUCCEED();
}

}  // namespace
}  // namespace ipl
}  // namespace dpl2
