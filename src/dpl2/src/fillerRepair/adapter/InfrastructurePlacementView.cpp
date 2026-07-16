// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "InfrastructurePlacementView.h"

#include <algorithm>
#include <set>

namespace dpl2::fillerRepair::adapter {
namespace {

VtId vtOfMaster(const ipl::ImplantLayerChecker& checker,
                const ipl::MasterInput& master)
{
  for (const ipl::MasterShape& shape : master.shapes) {
    for (const ipl::ImplantLayer& layer : checker.layers()) {
      if (layer.id == shape.layer) {
        return layer.family == ipl::Family::Unknown
                   ? kUnknownVt
                   : static_cast<VtId>(layer.family);
      }
    }
  }
  return kUnknownVt;
}

// Bottom-band polarity in the master's R0 frame, mirroring the checker's
// rebuildMasterShapes anchor: the bottommost band shape's layer carries it
// (master.shapes are the rebuilt canonical band shapes).
BandPolarity bottomPolarityOfMaster(const ipl::ImplantLayerChecker& checker,
                                    const ipl::MasterInput& master)
{
  const ipl::MasterShape* bottom = nullptr;
  for (const ipl::MasterShape& shape : master.shapes) {
    if (bottom == nullptr || shape.rect.getYL().getStorage()
                                 < bottom->rect.getYL().getStorage()) {
      bottom = &shape;
    }
  }
  if (bottom != nullptr) {
    for (const ipl::ImplantLayer& layer : checker.layers()) {
      if (layer.id == bottom->layer) {
        return layer.polarity == ipl::Polarity::P ? BandPolarity::P
                                                  : BandPolarity::N;
      }
    }
  }
  return BandPolarity::N;  // parseLayerName's default polarity
}

bool supportedOrientation(eUTL::PhysOrientation orientation)
{
  return orientation == eUTL::PhysOrientationE::R0
         || orientation == eUTL::PhysOrientationE::R180
         || orientation == eUTL::PhysOrientationE::MX
         || orientation == eUTL::PhysOrientationE::MY;
}

// MUST match the checker's MasterInput.isFiller predicate
// (ImplantLayerChecker::initFromUDM: isCoreFiller() || isPadFiller()),
// otherwise the master cross-check below reports a false mismatch.
bool isFillerMaster(const eLIB::PhysLibCell& cell)
{
  return cell.getType().isCoreFiller() || cell.getType().isPadFiller();
}

}  // namespace

Orient toPlannerOrient(eUTL::PhysOrientation orientation)
{
  if (orientation == eUTL::PhysOrientationE::R180) return Orient::R180;
  if (orientation == eUTL::PhysOrientationE::MX) return Orient::MX;
  if (orientation == eUTL::PhysOrientationE::MY) return Orient::MY;
  return Orient::R0;
}

eUTL::PhysOrientation toUdmOrient(Orient orient)
{
  switch (orient) {
    case Orient::R180: return eUTL::PhysOrientationE::R180;
    case Orient::MX: return eUTL::PhysOrientationE::MX;
    case Orient::MY: return eUTL::PhysOrientationE::MY;
    case Orient::R0: break;
  }
  return eUTL::PhysOrientationE::R0;
}

InfrastructurePlacementView::InfrastructurePlacementView(
    const eUNL::PhysDesMgr& desMgr,
    const dpl2::Network& network,
    const ipl::ImplantLayerChecker& checker,
    const dpl2::fillerSetting& fillerSetting,
    const DebugLog& log)
{
  struct RowFrame
  {
    DbCoord originX = 0;
    DbCoord yLo = 0;
    DbCoord yHi = 0;
  };
  std::map<RowId, RowFrame> frames;

  RowId rowId = 0;
  for (const eUNL::PhysRow& row : desMgr.getPhysRowIter()) {
    if (!row.getSite().getIsPad()) {
      const DbCoord width = row.getSite().getWidth().getStorage();
      const DbCoord height = row.getSite().getHeight().getStorage();
      if (site_width_ == 0) site_width_ = width;
      else if (site_width_ != width) {
        addProblem(Severity::Fatal, "NonUniformSiteWidth",
                   "non-pad rows do not share one site width");
      }
      if (row_height_ == 0) row_height_ = height;
      else if (row_height_ != height) {
        addProblem(Severity::Fatal, "NonUniformRowHeight",
                   "non-pad rows do not share one row height");
      }
      const eUTL::Rect bbox = row.getBbox();
      const DbCoord spanWidth = (bbox.getXH() - bbox.getXL()).getStorage();
      if (spanWidth <= 0 || width <= 0 || spanWidth % width != 0) {
        addProblem(Severity::Fatal, "InvalidRowSpan",
                   cat("row ", rowId, " has an invalid legal span"));
      }
      row_spans_[rowId] = XInterval{0, spanWidth};
      frames[rowId] = RowFrame{row.getOrigin().getX().getStorage(),
                               row.getOrigin().getY().getStorage(),
                               (row.getOrigin().getY()
                                + row.getSite().getHeight()).getStorage()};
    }
    ++rowId;
  }
  if (site_width_ <= 0 || row_height_ <= 0 || row_spans_.empty()) {
    addProblem(Severity::Fatal, "MissingRowGeometry",
               "no usable standard-cell row geometry");
  }
  if (checker.siteWidth() != site_width_) {
    addProblem(Severity::Fatal, "CheckerSiteWidthMismatch",
               cat("checker site width ", checker.siteWidth(),
                   " differs from infrastructure ", site_width_));
  }
  // Planner windows/guards and the checker's inter-row comparisons both use
  // ONE x frame across rows (x relative to the row origin). Rows with
  // different origin X (offset core corners, multi-segment rows) silently
  // break that assumption for both sides -> refuse instead of miscomparing.
  for (const auto& [id, frame] : frames) {
    if (frame.originX != frames.begin()->second.originX) {
      addProblem(Severity::Fatal, "RowOriginMisaligned",
                 cat("row ", id, " origin X ", frame.originX,
                     " differs from row ", frames.begin()->first, " origin X ",
                     frames.begin()->second.originX,
                     " -> per-row x frames are not comparable"));
      break;
    }
  }

  // y -> row lookup index. Uniform row height is validated above, so at most
  // one yLo group can contain a given y; among same-y segments the checker's
  // initFromUDM picks the FIRST row in row order -- mirror that tie-break.
  struct RowRange
  {
    DbCoord yLo = 0;
    DbCoord yHi = 0;
    RowId rowId = 0;
  };
  std::vector<RowRange> rowRanges;
  rowRanges.reserve(frames.size());
  for (const auto& [id, frame] : frames) {
    rowRanges.push_back(RowRange{frame.yLo, frame.yHi, id});
  }
  std::sort(rowRanges.begin(), rowRanges.end(),
            [](const RowRange& a, const RowRange& b) {
              return a.yLo != b.yLo ? a.yLo < b.yLo : a.rowId < b.rowId;
            });
  const auto rowContaining = [&rowRanges](DbCoord y) -> RowId {
    auto it = std::upper_bound(
        rowRanges.begin(), rowRanges.end(), y,
        [](DbCoord value, const RowRange& range) { return value < range.yLo; });
    if (it == rowRanges.begin()) {
      return -1;
    }
    --it;
    while (it != rowRanges.begin() && std::prev(it)->yLo == it->yLo) {
      --it;
    }
    return (y >= it->yLo && y < it->yHi) ? it->rowId : -1;
  };

  row_list_.reserve(row_spans_.size());
  for (const auto& [id, span] : row_spans_) {
    row_list_.push_back(id);
  }

  const auto heightInRows = [this](DbCoord height) {
    return row_height_ > 0
               ? std::max<DbCoord>((height + row_height_ - 1) / row_height_, 1)
               : 1;
  };

  for (const auto& masterPtr : network.getMasters()) {
    if (masterPtr == nullptr || masterPtr->getPhysLibCell() == nullptr) continue;
    const eLIB::PhysLibCell* cell = masterPtr->getPhysLibCell();
    const MasterId id = static_cast<MasterId>(
        masterPtr->getDbMaster().getIndexValue());
    master_cells_[id] = cell;
    masters_[id] = MasterInfo{id,
                              cell->getWidth().getStorage(),
                              heightInRows(cell->getHeight().getStorage()),
                              isFillerMaster(*cell),
                              kUnknownVt};
  }

  for (const eLIB::PhysLibCell* cell : fillerSetting.getFillerMasters()) {
    if (cell == nullptr) continue;
    const MasterId id = static_cast<MasterId>(
        cell->getLibCellId().getIndexValue());
    const MasterInfo info{id,
                          cell->getWidth().getStorage(),
                          heightInRows(cell->getHeight().getStorage()),
                          isFillerMaster(*cell),
                          kUnknownVt};
    filler_master_ids_.push_back(id);
    master_cells_[id] = cell;
    auto [it, inserted] = masters_.emplace(id, info);
    if (!inserted && (it->second.width != info.width
                      || it->second.height != info.height
                      || it->second.isFiller != info.isFiller)) {
      addProblem(Severity::Fatal, "FillerMasterMismatch",
                 cat("fillerSetting and Network disagree for master ", id));
    }
    if (!info.isFiller) {
      addProblem(Severity::Fatal, "ConfiguredMasterNotFiller",
                 cat("configured master ", id, " is not a filler master"));
    }
  }

  for (const ipl::MasterInput& input : checker.masters()) {
    const MasterId id = static_cast<MasterId>(input.masterId);
    const MasterInfo checkerInfo{id,
                                 input.width,
                                 heightInRows(input.height),
                                 input.isFiller,
                                 vtOfMaster(checker, input),
                                 bottomPolarityOfMaster(checker, input)};
    checker_master_ids_.insert(id);
    auto [it, inserted] = masters_.emplace(id, checkerInfo);
    if (!inserted) {
      if (it->second.width != checkerInfo.width
          || it->second.height != checkerInfo.height
          || it->second.isFiller != checkerInfo.isFiller) {
        addProblem(Severity::Fatal, "CheckerMasterMismatch",
                   cat("checker and infrastructure disagree for master ", id));
      }
      it->second.vt = checkerInfo.vt;
      it->second.bottomBandPolarity = checkerInfo.bottomBandPolarity;
    }
  }
  std::sort(filler_master_ids_.begin(), filler_master_ids_.end());
  filler_master_ids_.erase(
      std::unique(filler_master_ids_.begin(), filler_master_ids_.end()),
      filler_master_ids_.end());
  // A configured candidate the checker cannot model cannot be validated by
  // the oracle -- every overlay using it would come back invalid. Exclude it
  // (Warning, not Fatal): the repair stays usable with the modeled subset,
  // and the diagnostic tells RD which masters the checker must learn.
  filler_master_ids_.erase(
      std::remove_if(filler_master_ids_.begin(), filler_master_ids_.end(),
                     [this](MasterId id) {
                       if (checker_master_ids_.count(id) != 0) {
                         return false;
                       }
                       addProblem(
                           Severity::Warning, "CheckerMissingConfiguredMaster",
                           cat("checker has no model for configured filler "
                               "master ", id, " -> excluded from candidates"));
                       return true;
                     }),
      filler_master_ids_.end());

  const eUTL::Rect& core = network.getCore();
  for (const auto& nodePtr : network.getNodes()) {
    const dpl2::Node* node = nodePtr.get();
    if (node == nullptr || (!node->isPlaced() && !node->isFixed())) continue;
    const dpl2::Master* infraMaster = node->getMaster();
    if (infraMaster == nullptr || infraMaster->getPhysLibCell() == nullptr) {
      addProblem(Severity::Fatal, "NodeMissingMaster",
                 cat("node ", node->getId(), " has no physical master"));
      continue;
    }
    const eLIB::PhysLibCell* cell = infraMaster->getPhysLibCell();
    if (!cell->getType().isCore()) continue;
    if (!supportedOrientation(node->getOrient())) {
      addProblem(Severity::Fatal, "UnsupportedOrientation",
                 cat("node ", node->getId(), " has unsupported orientation"));
      continue;
    }

    const DbCoord absoluteY = node->getBottom().v + core.getYL().getStorage();
    const RowId containingRow = rowContaining(absoluteY);
    if (containingRow < 0) {
      addProblem(Severity::Fatal, "NodeOutsideRows",
                 cat("node ", node->getId(), " is not in a legal row"));
      continue;
    }

    const MasterId masterId = static_cast<MasterId>(
        infraMaster->getDbMaster().getIndexValue());
    const MasterInfo* info = masterInfo(masterId);
    if (info == nullptr) {
      addProblem(Severity::Fatal, "UnknownInfrastructureMaster",
                 cat("node ", node->getId(), " references master ", masterId));
      continue;
    }
    const bool isFiller = isFillerMaster(*cell);
    if (node->isFiller() != isFiller) {
      addProblem(Severity::Fatal, "FillerClassificationMismatch",
                 cat("node ", node->getId(), " filler flag disagrees with master"));
      continue;
    }

    const InstanceId id = static_cast<InstanceId>(
        node->getDbInst().getIndexValue());
    const DbCoord absoluteX = node->getLeft().v + core.getXL().getStorage();
    PlacedInstance placed{id,
                          masterId,
                          containingRow,
                          absoluteX - frames.at(containingRow).originX,
                          toPlannerOrient(node->getOrient()),
                          isFiller};
    // A placed filler whose master has no checker VT (no implant shapes, or
    // unparseable family) simply cannot be swapped: candidate filtering
    // already excludes it, and the checker sees the same committed geometry
    // in baseline and candidates, so legality stays sound. Warning, not
    // Fatal -- one odd filler must not disable repair for the whole design.
    if (placed.isFiller && info->vt == kUnknownVt) {
      addProblem(Severity::Warning, "CheckerMissingPlacedFillerMaster",
                 cat("placed filler ", id, " uses master ", masterId,
                     " without checker VT metadata -> not swappable"));
    }
    instances_[id] = placed;
    leaf_cells_[id] = node->getDbInst();
    for (DbCoord offset = 0; offset < std::max<DbCoord>(info->height, 1); ++offset) {
      PlacedInstance rowCopy = placed;
      rowCopy.rowId = containingRow + static_cast<RowId>(offset);
      const auto frame = frames.find(rowCopy.rowId);
      if (frame == frames.end()) {
        addProblem(Severity::Fatal, "MultiRowOutsideRows",
                   cat("node ", node->getId(), " extends outside legal rows"));
        break;
      }
      rowCopy.x = absoluteX - frame->second.originX;
      by_row_[rowCopy.rowId].push_back(rowCopy);
    }
  }

  for (auto& [id, list] : by_row_) {
    std::sort(list.begin(), list.end(),
              [](const PlacedInstance& a, const PlacedInstance& b) {
                return a.x != b.x ? a.x < b.x : a.id < b.id;
              });
  }

  for (const auto& [checkerId, checkerInst] : checker.placedInsts()) {
    const InstanceId id = static_cast<InstanceId>(checkerId);
    checker_instance_ids_.insert(id);
    const PlacedInstance* infra = instance(id);
    if (infra == nullptr) {
      addProblem(Severity::Fatal, "CheckerInstanceMissing",
                 cat("checker instance ", id, " has no Network node"));
      continue;
    }
    if (infra->masterId != static_cast<MasterId>(checkerInst.masterId)
        || infra->rowId != static_cast<RowId>(checkerInst.rowId)
        || infra->x != static_cast<DbCoord>(checkerInst.colId) * site_width_
        || infra->orientation != toPlannerOrient(checkerInst.orientation)
        || infra->isFiller != checkerInst.isFiller) {
      addProblem(Severity::Fatal, "CheckerInfrastructureMismatch",
                 cat("checker and infrastructure disagree for instance ", id));
    }
  }

  for (const auto& [id, infra] : instances_) {
    if (checker_master_ids_.count(infra.masterId) != 0
        && checker_instance_ids_.count(id) == 0) {
      addProblem(Severity::Fatal, "InfrastructureInstanceMissingFromChecker",
                 cat("infrastructure instance ", id,
                     " uses a checker master but is absent from checker placement"));
    }
  }

  log.msg("adapter",
          cat("infrastructure view: ", instances_.size(),
              " placed node(s), ", masters_.size(), " master(s), ",
              filler_master_ids_.size(), " configured filler master(s)"));
}

bool InfrastructurePlacementView::isValid() const
{
  return std::none_of(setup_diagnostics_.begin(), setup_diagnostics_.end(),
                      [](const Diagnostic& d) { return d.severity == Severity::Fatal; });
}

SiteCoverageResult InfrastructurePlacementView::checkSiteCoverage(
    const DebugLog& log) const
{
  // Same-size swaps never change coverage and the view is an immutable
  // snapshot, so one computation serves every repair on this view;
  // call_once keeps concurrent repair threads safe.
  std::call_once(coverage_once_, [&] {
    coverage_cache_ = PlacementView::checkSiteCoverage(log);
  });
  return coverage_cache_;
}

void InfrastructurePlacementView::addProblem(Severity severity,
                                             const std::string& code,
                                             const std::string& message)
{
  setup_diagnostics_.push_back(makeDiag(severity, code, message));
}

XInterval InfrastructurePlacementView::rowLegalSpan(RowId id) const
{
  const auto it = row_spans_.find(id);
  return it == row_spans_.end() ? XInterval{} : it->second;
}

const std::vector<PlacedInstance>& InfrastructurePlacementView::instancesInRow(
    RowId id) const
{
  const auto it = by_row_.find(id);
  return it == by_row_.end() ? emptyInstances() : it->second;
}

const PlacedInstance* InfrastructurePlacementView::instance(InstanceId id) const
{
  const auto it = instances_.find(id);
  return it == instances_.end() ? nullptr : &it->second;
}

const MasterInfo* InfrastructurePlacementView::masterInfo(MasterId id) const
{
  const auto it = masters_.find(id);
  return it == masters_.end() ? nullptr : &it->second;
}

InstanceId InfrastructurePlacementView::instanceIdOf(
    eUNL::LeafCellID cellId) const
{
  const InstanceId id = static_cast<InstanceId>(cellId.getIndexValue());
  return instances_.count(id) == 0 || checker_instance_ids_.count(id) == 0 ? -1 : id;
}

MasterId InfrastructurePlacementView::masterIdOf(
    const eLIB::PhysLibCell& master) const
{
  const MasterId id = static_cast<MasterId>(master.getLibCellId().getIndexValue());
  return masters_.count(id) == 0 ? -1 : id;
}

eUNL::LeafCellID InfrastructurePlacementView::leafCellOf(
    InstanceId id) const
{
  const auto it = leaf_cells_.find(id);
  return it == leaf_cells_.end() ? eUNL::LeafCellID() : it->second;
}

const eLIB::PhysLibCell* InfrastructurePlacementView::physLibCellOf(
    MasterId id) const
{
  const auto it = master_cells_.find(id);
  return it == master_cells_.end() ? nullptr : it->second;
}

}  // namespace dpl2::fillerRepair::adapter
