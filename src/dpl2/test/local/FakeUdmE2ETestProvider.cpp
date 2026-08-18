// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Repository-local data provider for the portable runtime E2E cases.
// This is the only source that knows fake_udm. It is intentionally outside
// the fillerRepair delivery directory.

#include "E2ETestProvider.h"

#include <map>
#include <memory>

#include "fake_udm.h"
#include <infrastructure/Grid.h>
#include <infrastructure/Padding.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

namespace dpl2::fillerRepair::test {
namespace {

struct MasterSpec
{
  const char* name;
  int libIndex;
  int width;
  bool isFiller;
  int nLayerRel;
  int pLayerRel;
};

constexpr MasterSpec kMasters[] = {
    {"SL6", 0, 6, false, 0, 1}, {"TL4", 1, 4, false, 0, 1},
    {"TH4", 2, 4, false, 2, 3}, {"FL2", 3, 2, true, 0, 1},
    {"FH2", 4, 2, true, 2, 3},  {"FS2", 5, 2, true, 4, 5},
    {"BL2", 9, 2, false, 0, 1},
};

struct Placement
{
  int cellIndex;
  int libIndex;
  int row;
  int x;
};

constexpr Placement kPlacements[] = {
    {100, 0, 0, 0},  {101, 0, 0, 6},  {102, 0, 0, 12},
    {103, 3, 0, 18}, {110, 0, 1, 0},  {111, 0, 1, 6},
    {112, 0, 1, 12}, {113, 3, 1, 18}, {120, 0, 2, 0},
    {121, 3, 2, 6},  {122, 1, 2, 8},  {123, 3, 2, 12},
    {124, 0, 2, 14}, {130, 0, 3, 0},  {131, 0, 3, 6},
    {132, 0, 3, 12}, {133, 3, 3, 18}, {140, 0, 4, 0},
    {141, 0, 4, 6},  {142, 0, 4, 12}, {143, 3, 4, 18},
};

int cellIndex(CellRole role)
{
  switch (role) {
    case CellRole::Row0ThirdCell: return 102;
    case CellRole::Row0TailFiller: return 103;
    case CellRole::Row1TailFiller: return 113;
    case CellRole::Target: return 122;
    case CellRole::TargetLeftFiller: return 121;
    case CellRole::TargetRightFiller: return 123;
  }
  return -1;
}

int masterIndex(MasterRole role)
{
  switch (role) {
    case MasterRole::TargetOld: return 1;
    case MasterRole::TargetNew: return 2;
    case MasterRole::RepairFiller: return 4;
    case MasterRole::ExtraUninstantiatedFiller: return 6;
    case MasterRole::MismatchedTarget: return 8;
    case MasterRole::BufferLow: return 9;
  }
  return -1;
}

void buildDesign(fake_udm::DesignDb& db, const DesignSetup& setup)
{
  db.coreSite.width_ = eUTL::UvDist(kSiteWidth);
  db.coreSite.height_ = eUTL::UvDist(kRowHeight);
  db.tech().addLayer(
      "VTL_N",
      true,
      0,
      setup.implantRuleWidth,
      setup.usedLayerMissingRule ? 0 : 2);
  db.tech().addLayer("VTL_P", true, 1, setup.implantRuleWidth, 2);
  db.tech().addLayer("VTH_N", true, 2, setup.implantRuleWidth, 2);
  db.tech().addLayer("VTH_P", true, 3, setup.implantRuleWidth, 2);
  db.tech().addLayer("VTS_N", true, 4, setup.implantRuleWidth, 2);
  db.tech().addLayer("VTS_P", true, 5, setup.implantRuleWidth, 2);
  db.tech().addLayer("M1", false, 6);
  if (setup.unusedRuleLayers) {
    db.tech().addLayer("VTUL_N", true, 7, 6, 0);
    db.tech().addLayer("VTUL_P", true, 8, 6, 0);
  }

  for (const MasterSpec& spec : kMasters) {
    eLIB::PhysLibCell& cell = db.addMaster(
        spec.name,
        spec.libIndex,
        spec.width,
        kRowHeight,
        spec.isFiller && !setup.misclassifiedFillerMasters);
    fake_udm::DesignDb::addShape(cell, spec.nLayerRel, 0, kRowHeight / 2);
    fake_udm::DesignDb::addShape(
        cell, spec.pLayerRel, kRowHeight / 2, kRowHeight);
  }
  eLIB::PhysLibCell& extra = db.addMaster("FX4", 6, 4, kRowHeight, true);
  fake_udm::DesignDb::addShape(extra, 2, 0, kRowHeight / 2);
  fake_udm::DesignDb::addShape(extra, 3, kRowHeight / 2, kRowHeight);

  eLIB::PhysLibCell& hardMacro
      = db.addMaster("HM6", 7, 6, 2 * kRowHeight, false);
  hardMacro.type_ = eLIB::PhysMacroType(eLIB::PhysMacroType::TypeE::BLOCK);
  // This macro is placed MX in row 0. Its canonical two-row polarity must be
  // P/N then N/P so mirroring produces the row track pattern P/N then N/P.
  fake_udm::DesignDb::addShape(hardMacro, 1, 0, kRowHeight / 2);
  fake_udm::DesignDb::addShape(hardMacro, 0, kRowHeight / 2, kRowHeight);
  fake_udm::DesignDb::addShape(
      hardMacro, 0, kRowHeight, 3 * kRowHeight / 2);
  fake_udm::DesignDb::addShape(
      hardMacro, 1, 3 * kRowHeight / 2, 2 * kRowHeight);

  eLIB::PhysLibCell& mismatched
      = db.addMaster("TX5", 8, 5, kRowHeight, false);
  fake_udm::DesignDb::addShape(mismatched, 2, 0, kRowHeight / 2);
  fake_udm::DesignDb::addShape(mismatched, 3, kRowHeight / 2, kRowHeight);

  int rowIndexOffset = 0;
  if (setup.padRowFirst) {
    db.desMgr().addRow(setup.padRowOriginX,
                       -kRowHeight,
                       kSiteWidth,
                       kRowHeight,
                       kRowSites,
                       true);
    rowIndexOffset = 1;
  }
  for (int row = 0; row < kStandardRows; ++row) {
    db.desMgr().addRow(setup.rowOriginX[static_cast<size_t>(row)],
                       row * kRowHeight,
                       setup.rowSiteWidth[static_cast<size_t>(row)],
                       kRowHeight,
                       kRowSites);
  }
  if (setup.overlappingDoubleHeightRow) {
    // A second site class may cover two base placement rows. Append it after
    // the ordinary rows so PhysRow iteration order is deliberately not a Grid
    // row-id mapping.
    db.desMgr().addRow(0,
                       kRowHeight,
                       kSiteWidth,
                       2 * kRowHeight,
                       kRowSites);
  }
  if (setup.padRowLast) {
    db.desMgr().addRow(setup.padRowOriginX,
                       kStandardRows * kRowHeight,
                       kSiteWidth,
                       kRowHeight,
                       kRowSites,
                       true);
  }
  if (setup.row0TailHardBlockage) {
    const int64_t rowX = setup.rowOriginX[0];
    db.desMgr().addBlockage(rowX + 18, 0, rowX + 20, kRowHeight);
  }
  if (setup.row0TailSoftBlockage) {
    const int64_t rowX = setup.rowOriginX[0];
    db.desMgr().addBlockage(
        rowX + 18, 0, rowX + 20, kRowHeight, true);
  }

  for (const Placement& placement : kPlacements) {
    if (setup.row0ThirdHardMacro && placement.cellIndex == 112) {
      continue;  // the two-row hard macro supplies this upper-row coverage
    }
    const int rowIndex = placement.row + rowIndexOffset;
    const eUTL::PhysOrientation orient = rowIndex % 2 == 0
                                             ? eUTL::PhysOrientationE::MX
                                             : eUTL::PhysOrientationE::R0;
    const int libIndex = setup.row0ThirdHardMacro && placement.cellIndex == 102
                             ? 7
                             : placement.libIndex;
    db.desMgr().addCell(
        eUNL::LeafCellID(0, placement.cellIndex),
        &db.design.lib_acc_.getPhysLibCell(libIndex),
        setup.rowOriginX[static_cast<size_t>(placement.row)] + placement.x,
        placement.row * kRowHeight,
        orient);
  }
  db.activate();
}

class FakeDesignFixture final : public E2ETestDesign
{
 public:
  explicit FakeDesignFixture(const DesignSetup& setup) : setup_(setup)
  {
    buildDesign(db_, setup_);
  }

  eUNL::Design* design() override { return &db_.design; }
  eUNL::PhysDesMgr* desMgr() override { return db_.design.getPhysDesMgr(); }
  const eLIB::PhysLibCell& master(MasterRole role) const override
  {
    return db_.design.lib_acc_.getPhysLibCell(masterIndex(role));
  }
  eUNL::LeafCellID cell(CellRole role) const override
  {
    return eUNL::LeafCellID(0, cellIndex(role));
  }
  int64_t rowOriginX(int standardRow) const override
  {
    return setup_.rowOriginX.at(static_cast<size_t>(standardRow));
  }
  size_t standardRowCount() const override { return kStandardRows; }

  void moveCell(CellRole role, int64_t x, int64_t y) override
  {
    eUNL::PhysCellData& data = db_.desMgr().cells_[cell(role)];
    data.origin
        = eUTL::Point2D(eUTL::UvDist(x), eUTL::UvDist(y));
  }

  void replaceCellMaster(CellRole role, MasterRole master) override
  {
    db_.desMgr().cells_[cell(role)].master
        = &db_.design.lib_acc_.getPhysLibCell(masterIndex(master));
  }

  PhysicalSnapshot snapshot() const override
  {
    PhysicalSnapshot result;
    for (const Placement& placement : kPlacements) {
      const eUNL::PhysCell placed = db_.design.des_mgr_.getPhysCell(
          eUNL::LeafCellID(0, placement.cellIndex));
      if (!placed.isValid()) {
        continue;
      }
      const eUTL::Point2D origin = placed.getOrigin();
      result.emplace_back(
          placement.cellIndex,
          origin.getX().getStorage(),
          origin.getY().getStorage(),
          placed.getPhysMaster().getLibCellId().getIndexValue(),
          static_cast<int>(placed.getStatus()),
          static_cast<int>(placed.getOrient().getValue()));
    }
    return result;
  }

  void activate() override { db_.activate(); }

 private:
  DesignSetup setup_;
  fake_udm::DesignDb db_;
};

class FakeInfrastructureFixture final : public E2ETestInfrastructure
{
 public:
  bool build(FakeDesignFixture& fixture, const DesignSetup& setup)
  {
    eUNL::PhysDesMgr* desMgr = fixture.desMgr();
    bool haveCore = false;
    eUTL::Rect core;
    for (const eUNL::PhysRow& row : desMgr->getPhysRowIter()) {
      if (row.getSite().getIsPad()) {
        continue;
      }
      core = haveCore ? core.expand(row.getBbox()) : row.getBbox();
      haveCore = true;
    }
    if (!haveCore) {
      return false;
    }

    padding_->setDesginManager(desMgr);
    if (setup.row0TailHaloWidth > 0) {
      padding_->setPadding(fixture.cell(CellRole::Row0ThirdCell),
                           dpl2::GridX{0},
                           dpl2::GridX{setup.row0TailHaloWidth});
    }
    grid_.setCore(core);
    grid_.examineRows(desMgr);
    grid_.initGrid(desMgr, padding_, 100, 100);
    network_.setCore(core);
    dpl2::fillerSetting filler_setting(fixture.design());
    filler_setting.addFillerCell("FL2 FH2 FS2");

    std::map<eLIB::LibCellID, const eLIB::PhysLibCell*> placedMasters;
    for (const Placement& placement : kPlacements) {
      const eUNL::LeafCellID id(0, placement.cellIndex);
      const eUNL::PhysCell cell = desMgr->getPhysCell(id);
      if (!cell.isValid()) {
        if (setup.row0ThirdHardMacro && placement.cellIndex == 112) {
          continue;
        }
        return false;
      }
      placedMasters[cell.getPhysMaster().getLibCellId()]
          = &cell.getPhysMaster();
    }
    // Empty edge-type table: this harness exercises implant DRC, which reads
    // master geometry only. addMaster dereferences the table unconditionally.
    static const dpl2::EdgeTypeTable kNoEdgeTypes;
    for (const auto& [id, master] : placedMasters) {
      (void) id;
      network_.addMaster(*master, filler_setting, &grid_, &kNoEdgeTypes);
    }
    for (const Placement& placement : kPlacements) {
      const eUNL::LeafCellID id(0, placement.cellIndex);
      if (!desMgr->getPhysCell(id).isValid()) {
        if (setup.row0ThirdHardMacro && placement.cellIndex == 112) {
          continue;
        }
        return false;
      }
      network_.addNode(id, desMgr);
    }
    for (const auto& node : network_.getNodes()) {
      grid_.paintPixel(node.get());
    }
    return true;
  }

  dpl2::Grid* grid() override { return &grid_; }
  dpl2::Network* network() override { return &network_; }

 private:
  std::shared_ptr<dpl2::Padding> padding_
      = std::make_shared<dpl2::Padding>();
  dpl2::Grid grid_;
  dpl2::Network network_;
};

class FakeUdmProvider final : public E2ETestProvider
{
 public:
  std::unique_ptr<E2ETestDesign> createDesign(
      const DesignSetup& setup) override
  {
    return std::make_unique<FakeDesignFixture>(setup);
  }

  std::unique_ptr<E2ETestInfrastructure> createInfrastructure(
      E2ETestDesign& design,
      const DesignSetup& setup) override
  {
    auto* fakeDesign = dynamic_cast<FakeDesignFixture*>(&design);
    if (fakeDesign == nullptr) {
      return nullptr;
    }
    auto infrastructure = std::make_unique<FakeInfrastructureFixture>();
    if (!infrastructure->build(*fakeDesign, setup)) {
      return nullptr;
    }
    return infrastructure;
  }
};

}  // namespace

std::unique_ptr<E2ETestProvider> makeE2ETestProvider()
{
  return std::make_unique<FakeUdmProvider>();
}

}  // namespace dpl2::fillerRepair::test
