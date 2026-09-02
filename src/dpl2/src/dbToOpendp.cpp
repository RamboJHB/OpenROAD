// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors
#include <dpl2/DePlace.h>
#include <infrastructure/Grid.h>
#include <infrastructure/network.h>
#include <infrastructure/Objects.h>
#include <infrastructure/Padding.h>
#include <infrastructure/fillerSetting.h>
// These checkers are not part of the destination snapshot yet.
// #include <drc/PaddingChecker.h>
// #include <drc/EdgeSpacingChecker.h>
// #include <drc/BlockedLayersChecker.h>
// #include <drc/OneSiteGapChecker.h>
#include <drc/ImplantLayerChecker.h>
// #include <drc/FixedMaskCheck.h>
#include <PlacementDRC.h>

#include <unlObjTypes.hh>

namespace dpl2 {
void DePlace::importDb()
{
  Rect rect = getCoreArea();
  this->core_ = rect;

  grid_->setCore(rect);
  network_->setCore(rect);
  disallow_one_site_gaps_ = !hasOneSiteMaster(desMgr_);

  importClear();
  grid_->examineRows(desMgr_);
  initEdgeTypeTable();
  createNetwork();
  initPlacementDRC();
  setUpPlacementGroups();
  data_loaded_ = true;
}

void DePlace::initEdgeTypeTable()
{
  edge_type_table_ = std::make_unique<EdgeTypeTable>();
  const eLIB::TechLib& tech = desMgr_->getTopTech();
  for (const auto& rule : tech.getRuleIter()) {
    if (rule.getCheckType() == eLIB::RuleCheckType::LIB_CELL_EDGE_SPACING_TABLE_RULE) {
      auto* cest = static_cast<const eLIB::TechCellEdgeSpacingTable*>(rule.getCheckPtr());
      const auto& spacing_rules = cest->getEntries();
      if (!spacing_rules.empty()) {
        std::vector<std::string> names;
        names.reserve(spacing_rules.size() * 2);
        for (const auto& sr : spacing_rules) {
          names.push_back(sr.getEdgeType1());
          names.push_back(sr.getEdgeType2());
        }
        edge_type_table_->addNames(names.begin(), names.end());
      }
      break;
    }
  }
}

void DePlace::initPlacementDRC()
{
  // [FRPORT] Finalize the shared Master/Node classification before the
  // checker snapshots it. This is a setup-only mutation before worker checks.
  network_->updateFillerClassification(*filler_setting_);
  drc_engine_ = std::make_unique<PlacementDRC>(grid_.get());

// Register all DRC checkers into the extensible framework.
// Adding a new rule = write a new Checker subclass + add one line here.
  // Re-enable these registrations when their checker sources are available.
  // drc_engine_->addChecker(DRCCheckerType::EdgeSpacing,
  //     std::make_unique<EdgeSpacingChecker>(grid_.get(), *edge_type_table_, design_, desMgr_->getTopTech(), desMgr_));
  // drc_engine_->addChecker(DRCCheckerType::BlockedLayers,
  //     std::make_unique<BlockedLayersChecker>(grid_.get(), design_));
  // drc_engine_->addChecker(DRCCheckerType::Padding,
  //     std::make_unique<PaddingChecker>(grid_.get(), design_, padding_.get(), desMgr_));
  // drc_engine_->addChecker(DRCCheckerType::OneSiteGap,
  //     std::make_unique<OneSiteGapChecker>(grid_.get(), design_, disallow_one_site_gaps_));
  // [FRPORT] The final fillerSetting was applied while importing Network.
  // This checker is the single immutable revision used by placement workers.
  drc_engine_->addChecker(
      DRCCheckerType::ImplantLayer,
      std::make_unique<ipl::ImplantLayerChecker>(grid_.get(), design_, network_.get()));
  // drc_engine_->addChecker(DRCCheckerType::FixedMask,
  //     std::make_unique<FixedMaskChecker>(grid_.get(), design_, desMgr_));
}

void DePlace::importClear()
{
  deleteGrid();
}

bool DePlace::hasOneSiteMaster(PhysDesMgr* desMgr)
{
  const eUNL::LibObjAccessor& libAcc = design_->getLibAcc();
  for (const auto& libCell : libAcc.getLibCellIter(true, false)) {
    eLIB::PhysLibCell master = libAcc.getPhysLibCell(libCell.getId());

    auto master_type = master.getType();
    if (master_type.isBlock() || master_type.isPad() || master_type.isCover()) {
      continue;
    }

    // Ignore IO corner cells
    if (master_type.getType() == PhysMacroType::TypeE::ENDCAP_TOPLEFT
        || master_type.getType() == PhysMacroType::TypeE::ENDCAP_TOPRIGHT
        || master_type.getType() == PhysMacroType::TypeE::ENDCAP_BOTTOMLEFT
        || master_type.getType() == PhysMacroType::TypeE::ENDCAP_BOTTOMRIGHT) {
      continue;
    }

    const TechSite* site = master.getTechSite();
    if (site == nullptr) {
      continue;
    }

    if (site->getIsPad()) {
      continue;
    }

    if (site->getWidth() == master.getWidth()) {
      return true;
    }
  }
  return false;
}

void DePlace::createNetwork()
{
  eUNL::Session& sess = eUNL::Session::getSession();
  eUNL::Design* design = sess.getCurrentDesign();
  eUNL::HierManager* hierMgr = design->getHierMgr();

  const eUNL::LibObjAccessor& libAcc = design->getLibAcc();
  for (const auto& libCell : libAcc.getLibCellIter(true, false)) {
    network_->addMaster(libAcc.getPhysLibCell(libCell.getId()), *(filler_setting_.get()), grid_.get(), edge_type_table_.get());
  }

  struct PhysCellVisitor : public eUNL::UnlBaseVisitor<eUNL::PhysCell, eUNL::LeafCellID> {
    const PhysDesMgr* desMgr;
    const Grid* grid;
    const EdgeTypeTable* edge_types;
    const fillerSetting* filler_setting;
    Network* network;

    PhysCellVisitor(const PhysDesMgr* d, const Grid* g, const EdgeTypeTable* et, Network* net, const fillerSetting* fs)
        : desMgr(d), grid(g), edge_types(et), network(net), filler_setting(fs){}

    bool filter(const eUNL::PhysCell& pcell, const eUNL::LeafCellID& lcId) override {
      if (pcell.getPhysMaster().getLibCell().getName() != "GND") {
        return true;
      }
      return false;
    }

    eUNL::UnlIterStatus visit(const eUNL::PhysCell& pcell, const eUNL::LeafCellID& lcId) override {
      network->addMaster(pcell.getPhysMaster(), *filler_setting, grid, edge_types);
      network->addNode(lcId, desMgr);
      return eUNL::UnlIterStatus::CONTINUE;
    }
  };

  tbb::task_arena arena(1);
  PhysCellVisitor swVisitor{desMgr_, grid_.get(), edge_type_table_.get(), network_.get(), filler_setting_.get()};
  eLIB::PhysMacroUsageSet usages;
  desMgr_->iterateAllPhysCells(arena, swVisitor, usages);

}

void DePlace::setUpPlacementGroups()
{
  regions_rtree_.clear();
  int count = 0;
  auto db_groups = desMgr_->getPhysGroupIter();
  for (eUNL::PhysGroup db_group : db_groups) {
    if (!db_group.hasRegion()) {
      continue;
    }
    const eUNL::PhysRegion& region = db_group.getRegion();

    Group* rptr = arch_->createAndAddRegion();
    rptr->setId(count++);
    DbuRect bbox(Rect(UvDist(INT_MAX), UvDist(INT_MAX), UvDist(INT_MIN), UvDist(INT_MIN)));
    for (Rect box : region.getRects()) {
      box = box.overlap(core_, true);
      box.move(-core_.getXL(), -core_.getYL());

      bgBox bgbox(
      bgPoint(box.getXL().getStorage(), box.getYL().getStorage()),
      bgPoint(
          box.getXH().getStorage() - 1,
          box.getYH().getStorage() - 1));  /// the -1 is to prevent imaginary overlaps
                                          /// where a region ends and another starts
      regions_rtree_.insert(bgbox);
      rptr->addRect(box);
      bbox.expand(DbuRect(box));
    }
    rptr->setBoundary(bbox.getRect());
    // The instances within this region.
    for (LeafCellID db_inst : db_group.getLeafCells()) {
      Node* nd = network_->getNode(db_inst);
      if (nd != nullptr) {
        nd->setGroupId(rptr->getId());
        nd->setGroup(rptr);
        rptr->addCell(nd);
      }
    }
  }
}

}  //namespace dpl2
