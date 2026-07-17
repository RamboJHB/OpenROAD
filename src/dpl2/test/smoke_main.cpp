// Tier-1 end-to-end smoke over the REAL repair chain:
//   fake-UDM DesignDb -> Session -> ipl::ImplantLayerChecker (real
//   init(desMgr) UDM extraction) -> adapter::PlacementView::repair().
//
// Scenario (siteWidth=1, rowHeight=8, WIDTH rule=6, SPACING rule=2):
//   row1: SL[0,6) FL[6,8) T=TL[8,12) FL2[12,14) SL[14,20)   (all VTL)
//   rows 0/2: SL SL SL FL (all VTL, fully covered)
// repair(T -> TH(VTH)) breaks the VTL run: VTH island [8,12) width 4 < 6.
// Swapping ONE adjacent VTL filler to the VTH filler master heals it
// (either filler works; the engine picks deterministically).

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#include "drc/ImplantLayerChecker.h"
#include "fillerRepair/adapter/PlacementView.h"
#include "infrastructure/RepairInfrastructure.h"
#include "infrastructure/fillerSetting.h"

namespace {

constexpr int kSiteWidth = 1;
constexpr int kRowHeight = 8;
constexpr int kRowSites = 20;

struct MasterSpec
{
  const char* name;
  int libIndex;
  int width;
  bool isFiller;
  int nLayerRel;  // bottom band layer (N)
  int pLayerRel;  // top band layer (P)
};

const MasterSpec kMasters[] = {
    {"SL6", 0, 6, false, 0, 1},  // VTL std, context cells
    {"TL4", 1, 4, false, 0, 1},  // VTL std, the target's old master
    {"TH4", 2, 4, false, 2, 3},  // VTH std, the target's new master
    {"FL2", 3, 2, true, 0, 1},   // VTL filler
    {"FH2", 4, 2, true, 2, 3},   // VTH filler
    {"FS2", 5, 2, true, 4, 5},   // VTS filler (third VT, demoted in domains)
};

struct Placement
{
  int cellIndex;  // LeafCellID index
  int libIndex;
  int row;
  int x;
};

const Placement kPlacements[] = {
    // row 0: SL6 SL6 SL6 FL2  -> 6+6+6+2 = 20
    {100, 0, 0, 0},
    {101, 0, 0, 6},
    {102, 0, 0, 12},
    {103, 3, 0, 18},
    // row 1: SL6 FL2 TL4 FL2 SL6 -> 6+2+4+2+6 = 20
    {110, 0, 1, 0},
    {111, 3, 1, 6},
    {112, 1, 1, 8},  // the repair target
    {113, 3, 1, 12},
    {114, 0, 1, 14},
    // row 2: SL6 SL6 SL6 FL2
    {120, 0, 2, 0},
    {121, 0, 2, 6},
    {122, 0, 2, 12},
    {123, 3, 2, 18},
};

int failures = 0;

void expect(bool ok, const std::string& what)
{
  if (!ok) {
    ++failures;
    std::printf("[smoke][FAIL] %s\n", what.c_str());
  } else {
    std::printf("[smoke][ok] %s\n", what.c_str());
  }
}

}  // namespace

int main()
{
  using dpl2::fillerRepair::adapter::PlacementView;
  using dpl2::fillerRepair::adapter::RepairOutcome;

  // --- fake UDM design ------------------------------------------------------
  fake_udm::DesignDb db;
  db.coreSite.width_ = eUTL::UvDist(kSiteWidth);
  db.coreSite.height_ = eUTL::UvDist(kRowHeight);
  db.tech().addLayer("VTL_N", true, 0, /*width=*/6, /*minSpacing=*/2);
  db.tech().addLayer("VTL_P", true, 1, 6, 2);
  db.tech().addLayer("VTH_N", true, 2, 6, 2);
  db.tech().addLayer("VTH_P", true, 3, 6, 2);
  db.tech().addLayer("VTS_N", true, 4, 6, 2);
  db.tech().addLayer("VTS_P", true, 5, 6, 2);
  db.tech().addLayer("M1", false, 6);  // non-implant noise

  for (const MasterSpec& spec : kMasters) {
    eLIB::PhysLibCell& cell = db.addMaster(spec.name, spec.libIndex,
                                           spec.width, kRowHeight,
                                           spec.isFiller);
    fake_udm::DesignDb::addShape(cell, spec.nLayerRel, 0, kRowHeight / 2);
    fake_udm::DesignDb::addShape(cell, spec.pLayerRel, kRowHeight / 2,
                                 kRowHeight);
  }
  for (int row = 0; row < 3; ++row) {
    db.desMgr().addRow(0, row * kRowHeight, kSiteWidth, kRowHeight, kRowSites);
  }
  // Row alternation convention: the track pattern expects P at the bottom
  // band on EVEN rows and N on odd rows (buildTrackPattern); R0 masters are
  // N-bottom, so even-row cells are placed MX-flipped.
  for (const Placement& p : kPlacements) {
    const eUTL::PhysOrientation orient = p.row % 2 == 0
                                             ? eUTL::PhysOrientationE::MX
                                             : eUTL::PhysOrientationE::R0;
    db.desMgr().addCell(eUNL::LeafCellID(0, p.cellIndex),
                        &db.design.lib_acc_.getPhysLibCell(p.libIndex), p.x,
                        p.row * kRowHeight, orient);
  }
  db.activate();

  // --- ECO filler allow list (the candidate universe, AGENTS D23) -----------
  dpl2::fillerSetting fillerSetting(&db.design);
  fillerSetting.addFillerCell("FL2 FH2 FS2");

  // --- production infrastructure import ------------------------------------
  // The test provides only UDM data + cell handles.  Network masters/nodes,
  // Grid geometry and occupancy are all built by production infrastructure.
  std::vector<eUNL::LeafCellID> leafCells;
  leafCells.reserve(std::size(kPlacements));
  for (const Placement& p : kPlacements) {
    leafCells.emplace_back(0, p.cellIndex);
  }
  dpl2::RepairInfrastructure infrastructure;
  const bool infraReady = infrastructure.build(
      db.design.getPhysDesMgr(), leafCells, fillerSetting,
      db.design.lib_acc_.getPhysLibCell(2));  // target new master TH4
  for (const std::string& diag : infrastructure.diagnostics()) {
    std::printf("[smoke][infra] %s\n", diag.c_str());
  }
  expect(infraReady, "production infrastructure snapshot is ready");
  expect(infrastructure.network()->getNodes().size() == std::size(kPlacements),
         "Network nodes were imported from PhysDesMgr");
  expect(infrastructure.network()->getMasters().size() == std::size(kMasters),
         "placed, target and getFillerMasters masters were registered");

  // --- the real checker: constructor pulls the Session design and runs the
  // full init(desMgr) UDM extraction over the fake data ----------------------
  dpl2::ipl::ImplantLayerChecker checker(infrastructure.grid(),
                                         infrastructure.network());

  // --- unified boundary + repair --------------------------------------------
  PlacementView::Config config;
  config.verbose = std::getenv("FR_VERBOSE") != nullptr;
  PlacementView view(db.design.getPhysDesMgr(), infrastructure.grid(),
                     infrastructure.network(), &checker, &fillerSetting,
                     config);
  for (const auto& diag : view.setupDiagnostics()) {
    std::printf("[smoke][setup] %s: %s\n", diag.code.c_str(),
                diag.message.c_str());
  }
  expect(view.isReady(), "unified PlacementView is ready");
  const int targetNodeId =
      infrastructure.network()->getNodeId(eUNL::LeafCellID(0, 112));
  const auto* targetInstance = view.instance(targetNodeId);
  expect(targetInstance != nullptr && targetInstance->rowId == 1
             && targetInstance->x == 8,
         "PlacementView precheck geometry comes from PhysDesMgr");
  const std::vector<dpl2::fillerRepair::MasterId>& candidates =
      view.fillerMasterIds();
  expect(candidates == std::vector<dpl2::fillerRepair::MasterId>({3, 4, 5}),
         "candidate universe comes from getFillerMasters");

  const auto runRepair = [&](const char* label) -> RepairOutcome {
    RepairOutcome outcome =
        view.repair(eUNL::LeafCellID(0, 112),
                    db.design.lib_acc_.getPhysLibCell(2));  // TL4 -> TH4
    std::printf("[smoke] %s: hasSolution=%d changes=%zu\n", label,
                outcome.hasSolution ? 1 : 0, outcome.changes.size());
    for (const auto& record : outcome.changes) {
      std::printf("[smoke]   swap cell %d -> libCell %d\n",
                  record.cell_id_.getIndexValue(),
                  record.new_lib_cell_.getIndexValue());
    }
    for (const auto& diag : outcome.diagnostics) {
      std::printf("[smoke]   diag %s: %s\n", diag.code.c_str(),
                  diag.message.c_str());
    }
    return outcome;
  };

  const RepairOutcome first = runRepair("repair#1");
  expect(first.hasSolution, "repair finds a filler swap solution");
  expect(!first.changes.empty(), "solution carries FillerCellRecord changes");
  if (!first.changes.empty()) {
    const auto& record = first.changes.front();
    expect(record.op_ == dpl2::OpType::Replace, "record op is Replace");
    expect(record.new_lib_cell_.getIndexValue() == 4,
           "replacement master is the VTH filler (FH2)");
    expect(record.cell_id_.getIndexValue() == 111
               || record.cell_id_.getIndexValue() == 113,
           "swapped cell is one of the fillers next to the target");
  }

  // Determinism: identical outcome on a second run over the same state.
  const RepairOutcome second = runRepair("repair#2");
  expect(second.hasSolution == first.hasSolution
             && second.changes.size() == first.changes.size(),
         "second run matches the first");
  for (size_t i = 0; i < first.changes.size() && i < second.changes.size();
       ++i) {
    expect(first.changes[i].cell_id_ == second.changes[i].cell_id_
               && first.changes[i].new_lib_cell_
                      == second.changes[i].new_lib_cell_,
           "change " + std::to_string(i) + " identical across runs");
  }

  if (failures == 0) {
    std::printf("SMOKE OK\n");
    return 0;
  }
  std::printf("SMOKE FAILED: %d failure(s)\n", failures);
  return 1;
}
