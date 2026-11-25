#include <PlacementDRC.h>
#include <dpl2/DRCChecker.h>
#include <dpl2/DePlace.h>
#include <drc/ImplantLayerChecker.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <testFillerRepairCmd.hh>
#include <vector>

namespace dpl2 {
namespace {

bool parseNonNegativeInt(const std::string& text, int& value)
{
  if (text.empty()
      || text.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  try {
    const unsigned long long parsed = std::stoull(text);
    if (parsed
        > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
      return false;
    }
    value = static_cast<int>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::vector<std::string> splitIdentifiers(std::string text)
{
  std::replace(text.begin(), text.end(), ',', ' ');
  std::istringstream stream(text);
  std::vector<std::string> result;
  std::string token;
  while (stream >> token) {
    result.push_back(token);
  }
  return result;
}

Node* findNode(Network& network,
               eUNL::Design& design,
               const std::string& nameOrId)
{
  int nodeId = -1;
  if (parseNonNegativeInt(nameOrId, nodeId)) {
    return network.getNode(nodeId);
  }
  const auto* hierMgr = design.getHierMgr();
  if (hierMgr == nullptr) {
    return nullptr;
  }
  for (const auto& [id, owned] : network.getNodes()) {
    (void) id;
    if (owned == nullptr || !owned->getDbInst().isValid()) {
      continue;
    }
    if (hierMgr->getLeafCell(owned->getDbInst()).getName() == nameOrId) {
      return owned.get();
    }
  }
  return nullptr;
}

Master* findMaster(eUNL::Design& design,
                   Network& network,
                   const std::string& nameOrId)
{
  int masterId = -1;
  if (parseNonNegativeInt(nameOrId, masterId)) {
    return network.getMaster(masterId);
  }
  const eFNL::ModuleID moduleId = design.getLibAcc().findModule(nameOrId);
  const eLIB::LibCell* libCell
      = moduleId.isValid() ? design.getLibAcc().getLibCell(moduleId) : nullptr;
  return libCell != nullptr ? network.getMaster(libCell->getId()) : nullptr;
}

bool validFillerChanges(const std::vector<CellChangeRecord>& changes,
                        const fillerSetting& setting)
{
  return std::all_of(
      changes.begin(),
      changes.end(),
      [&setting](const CellChangeRecord& change) {
        if (change.op_ == OpType::Add) {
          const auto* name = std::get_if<std::string>(&change.cell_data_);
          return name != nullptr && !name->empty()
                 && setting.isFillerCell(change.new_lib_cell_);
        }
        if (change.op_ == OpType::Delete) {
          return std::holds_alternative<eUNL::LeafCellID>(change.cell_data_)
                 && setting.isFillerCell(change.orig_lib_cell_);
        }
        return change.op_ == OpType::Replace
               && std::holds_alternative<eUNL::LeafCellID>(change.cell_data_)
               && setting.isFillerCell(change.orig_lib_cell_)
               && setting.isFillerCell(change.new_lib_cell_);
      });
}

void printChanges(const char* phase,
                  const std::vector<CellChangeRecord>& changes,
                  const ipl::ImplantLayerChecker& checker,
                  bool details = true,
                  bool simulation = false)
{
  size_t deletes = 0;
  size_t adds = 0;
  size_t replaces = 0;
  for (const auto& change : changes) {
    deletes += change.op_ == OpType::Delete;
    adds += change.op_ == OpType::Add;
    replaces += change.op_ == OpType::Replace;
  }
  std::cout << "\n[test_filler_repair] ===== " << phase << " =====\n"
            << "  summary: records=" << changes.size() << " Delete=" << deletes
            << " Add=" << adds << " Replace=" << replaces << '\n';
  if (!details) {
    return;
  }
  Network& network = *checker.getNetwork();
  const Grid& grid = *checker.getGrid();
  const auto* hierarchy = checker.getDesign()->getHierMgr();
  const auto orientationName = [](PhysOrientation orientation) {
    if (orientation == PhysOrientationE::R0) return "R0";
    if (orientation == PhysOrientationE::R180) return "R180";
    if (orientation == PhysOrientationE::MX) return "MX";
    if (orientation == PhysOrientationE::MY) return "MY";
    return "OTHER";
  };
  const auto state = [&](const char* label, const Master* master, LibCellID libId,
                         UvDist x, UvDist y, PhysOrientation orientation) {
    const auto* physical = master != nullptr ? master->getPhysLibCell() : nullptr;
    std::cout << "      " << label << " master: libId=" << libId.getIndexValue()
              << " networkId=" << (master != nullptr ? master->getId() : -1)
              << " name=" << (physical != nullptr ? physical->getLibCell().getName() : "<unknown>")
              << '\n'
              << "        origin(core DBU)=(" << x.getStorage() << ',' << y.getStorage() << ')';
    if (physical != nullptr) {
      Node footprint;
      footprint.setLeft(DbuX{x.getStorage()});
      footprint.setBottom(DbuY{y.getStorage()});
      footprint.setWidth(DbuX{physical->getWidth().getStorage()});
      footprint.setHeight(DbuY{physical->getHeight().getStorage()});
      const auto box = grid.gridCovering(&footprint);
      std::cout << " size(DBU)=" << footprint.getWidth().v << 'x' << footprint.getHeight().v
                << " sites/rows=" << (box.xhi - box.xlo).v << 'x' << (box.yhi - box.ylo).v;
    }
    std::cout << " orient=" << orientationName(orientation)
              << '(' << static_cast<int>(orientation.getValue()) << ")\n";
  };
  for (size_t index = 0; index < changes.size(); ++index) {
    const auto& change = changes[index];
    const bool add = change.op_ == OpType::Add;
    const bool remove = change.op_ == OpType::Delete;
    const auto* id = std::get_if<eUNL::LeafCellID>(&change.cell_data_);
    const Node* old = !add && id != nullptr ? network.getNode(*id) : nullptr;
    const Master* before = add ? nullptr : old != nullptr ? old->getMaster()
                                                          : network.getMaster(change.orig_lib_cell_);
    const Master* after = !remove ? network.getMaster(change.new_lib_cell_) : nullptr;
    const bool filler = add ? after != nullptr && after->isFiller()
                            : old != nullptr ? old->isFiller() : before != nullptr && before->isFiller();
    const auto* name = std::get_if<std::string>(&change.cell_data_);
    std::cout << "  [" << index + 1 << "] op=" << (add ? "ADD" : remove ? "DELETE" : "REPLACE")
              << " kind=" << (filler ? "FILLER" : "STD") << '\n'
              << "      name=" << (add ? (name != nullptr ? *name : "<invalid>")
                  : old != nullptr && hierarchy != nullptr
                        ? hierarchy->getLeafCell(old->getDbInst()).getName() : "<unknown>") << '\n'
              << "      nodeId=" << (old != nullptr ? old->getId() : -1)
              << " leafId=" << (id != nullptr ? id->getIndexValue() : -1) << '\n';
    if (add) {
      std::cout << "      before: <no instance; ID assigned when the instance is created>\n";
    } else {
      state("before", before, change.orig_lib_cell_,
            old != nullptr ? UvDist(old->getLeft().v) : change.x_,
            old != nullptr ? UvDist(old->getBottom().v) : change.y_,
            old != nullptr ? old->getOrient() : change.orientation_);
    }
    if (remove) {
      std::cout << (simulation ? "      after: <hidden in this simulated overlay; NOT COMMITTED>\n"
                              : "      after: <removed from final placement>\n");
    } else {
      state("after", after, change.new_lib_cell_, change.x_, change.y_, change.orientation_);
    }
  }
}

CellChangeRecord deleteRecord(const Node& node)
{
  return CellChangeRecord{OpType::Delete,
                          node.getDbInst(),
                          UvDist(node.getLeft().v),
                          UvDist(node.getBottom().v),
                          node.getMaster()->getDbMaster(),
                          node.getMaster()->getDbMaster(),
                          node.getOrient()};
}

void printRejectedRequest(const ipl::CheckResult& result)
{
  for (const ipl::Diagnostic& diagnostic : result.diagnostics) {
    std::cout << "    diagnostic=" << diagnostic.status << ": "
              << diagnostic.message << '\n';
  }
  if (!result.violations.empty()) {
    std::cout << "    directViolations=" << result.violations.size() << '\n';
  }
}

}  // namespace

bool TestFillerRepairCmd::probeResize(
    DePlace& placement,
    const ipl::ImplantLayerChecker& checker,
    const Node& old,
    Node& target,
    int diameter,
    std::vector<CellChangeRecord>& changes) const
{
  const Grid& grid = *placement.getGrid();
  const Master& master = *target.getMaster();
  const eLIB::PhysLibCell& physical = *master.getPhysLibCell();
  std::vector<CellChangeRecord> removals{deleteRecord(old)};
  std::vector<CellChangeRecord> trial;
  std::cout << "\n[test_filler_repair] ===== REMOVAL PREPARATION BEGIN =====\n"
            << "  simulation only: pending records are NOT COMMITTED\n";
  // The virtual deletion seeds the search. Failure to refill the whole old
  // footprint here need not reject a target that will cover part of it.
  const bool prepared = checker.check(nullptr, grid.gridX(&old), grid.gridSnapDownY(&old),
                                      old.getOrient(), trial, removals);
  printChanges("REMOVAL PREPARATION (PENDING; DO NOT COMMIT)", trial, checker, true, true);
  std::cout << "  preparation status=" << (prepared ? "READY" : "NO SEED") << '\n'
            << "  These records are search-overlay input, not the final commit list.\n"
            << "  Pending Adds are discarded/re-tiled; pending Replaces are merged as seeds.\n"
            << "\n[test_filler_repair] ===== FINAL PLACEMENT CHECK BEGIN =====\n"
            << "  validators: full registered checker set\n";

  const Rect core = grid.getCore();
  const int64_t xl = int64_t{core.getXL().getStorage()} + old.getLeft().v;
  const int64_t yl = int64_t{core.getYL().getStorage()} + old.getBottom().v;
  const Rect box(
      UvDist(std::max<int64_t>(core.getXL().getStorage(), xl - diameter)),
      UvDist(std::max<int64_t>(core.getYL().getStorage(), yl - diameter)),
      UvDist(std::min<int64_t>(core.getXH().getStorage(),
                              xl + physical.getWidth().getStorage() + diameter)),
      UvDist(std::min<int64_t>(core.getYH().getStorage(),
                              yl + physical.getHeight().getStorage() + diameter)));
  // The command checks its own temporary node through the lower-level search.
  eUTL::PhysOrientation rtOrient{eUTL::PhysOrientationE::R0};
  const auto [col, row]
      = placement.legalCellInRect(box, &target, trial, rtOrient);
  if (col < 0 || row < 0) {
    std::cout << "NO REPAIR: operation=resize targetMaster=" << master.getId()
              << " diameter=" << diameter << '\n';
    std::cout << "\n[test_filler_repair] ===== COMMIT RESULT =====\n"
              << "  committed: records=0\n"
              << "  status=NOT_COMMITTED; preparation records were not committed\n";
    return false;
  }
  target.setLeft(DbuX{col * grid.getSiteWidth().v});
  target.setBottom(grid.gridYToDbu(GridY{row}));
  target.setOrient(rtOrient);
  changes = std::move(trial);
  return true;
}

bool TestFillerRepairCmd::exec()
{
  const std::vector<std::string> instanceIds
      = splitIdentifiers(instOpt_.getValue());
  const std::vector<std::string> masterIds
      = splitIdentifiers(masterOpt_.getValue());
  const std::string diameterText = diameterOpt_.getValue();
  int diameter = 0;
  if (!diameterText.empty() && !parseNonNegativeInt(diameterText, diameter)) {
    std::cout << "ERROR: -diameter must be a nonnegative integer in DBU\n";
    return false;
  }
  const bool removeOnly
      = masterOpt_.getValue().find_first_not_of(" \t\r\n\f\v") == std::string::npos;
  if (instanceIds.empty() || (!removeOnly && masterIds.size() != 1)) {
    std::cout << "ERROR: -inst is required; -master accepts one name/Network id "
                 "or no value for std-cell removal\n";
    return false;
  }

  DePlace* dePlace = DePlace::get();
  Network* network = dePlace->getNetwork();
  Grid* grid = dePlace->getGrid();
  fillerSetting* setting = dePlace->getFillerSetting();
  PlacementDRC* placementDrc = dePlace->getPlacementDRC();
  DRCChecker* checkerBase
      = placementDrc != nullptr
            ? placementDrc->getChecker(DRCCheckerType::ImplantLayer)
            : nullptr;
  auto* checker = dynamic_cast<ipl::ImplantLayerChecker*>(checkerBase);
  eUNL::Design* design = checker != nullptr ? checker->getDesign() : nullptr;
  if (network == nullptr || grid == nullptr || grid->getPixelYSize() == 0) {
    std::cout << "ERROR: DePlace infrastructure is not initialized\n";
    return false;
  }
  if (checker == nullptr) {
    std::cout << "ERROR: ImplantLayerChecker is not registered\n";
    return false;
  }
  if (design == nullptr) {
    std::cout << "ERROR: ImplantLayerChecker has no bound design\n";
    return false;
  }

  Master* targetMaster = removeOnly ? nullptr
      : findMaster(*design, *network, masterIds.front());
  if (!removeOnly
      && (targetMaster == nullptr || targetMaster->getPhysLibCell() == nullptr)) {
    std::cout << "ERROR: unknown or unregistered -master value '"
              << masterIds.front() << "'\n";
    return false;
  }

  std::vector<Node*> overlays;
  std::set<int> seenNodeIds;
  overlays.reserve(instanceIds.size());
  for (const std::string& identifier : instanceIds) {
    Node* node = findNode(*network, *design, identifier);
    if (node == nullptr || node->getMaster() == nullptr
        || !node->getDbInst().isValid()) {
      std::cout << "ERROR: unknown or incomplete -inst value '" << identifier
                << "'\n";
      return false;
    }
    if (!seenNodeIds.insert(node->getId()).second) {
      std::cout << "ERROR: duplicate -inst resolves to Network node "
                << node->getId() << '\n';
      return false;
    }
    overlays.push_back(node);
  }

  int left = std::numeric_limits<int>::max();
  int bottom = std::numeric_limits<int>::max();
  const Node* replacedStd = nullptr;
  for (const Node* node : overlays) {
    left = std::min(left, node->getLeft().v);
    bottom = std::min(bottom, node->getBottom().v);
    if (!node->isFiller()) {
      if (replacedStd != nullptr) {
        std::cout << "ERROR: at most one existing standard cell can be replaced\n";
        return false;
      }
      replacedStd = node;
    }
  }
  if (replacedStd != nullptr) {
    left = replacedStd->getLeft().v;
    bottom = replacedStd->getBottom().v;
  }
  const bool singleStd = overlays.size() == 1 && replacedStd != nullptr;
  if (!diameterText.empty() && (removeOnly || !singleStd)) {
    std::cout << "ERROR: -diameter requires one std instance and a target master\n";
    return false;
  }
  const eLIB::PhysLibCell* targetCell
      = targetMaster != nullptr ? targetMaster->getPhysLibCell() : nullptr;
  const bool resize = singleStd && targetCell != nullptr
                      && (!diameterText.empty()
                          || replacedStd->getWidth().v != targetCell->getWidth().getStorage()
                          || replacedStd->getHeight().v != targetCell->getHeight().getStorage());
  const char* operation = removeOnly ? "remove" : resize ? "resize" : "place";
  if ((removeOnly || resize)
      && (overlays.size() != 1 || replacedStd == nullptr
          || (!replacedStd->isStdCell() && replacedStd->getType() != Node::CELL)
          || replacedStd->isFixed() || replacedStd->isTerminal()
          || replacedStd->inGroup() || replacedStd->isBlock())) {
    std::cout << "ERROR: removal/resize requires one movable, ungrouped standard cell\n";
    return false;
  }
  const GridX x = grid->gridX(DbuX{left});
  const GridY y = grid->gridSnapDownY(DbuY{bottom});
  Node temporary;
  eUTL::PhysOrientation orientation = PhysOrientationE::R0;
  if (removeOnly) {
    orientation = replacedStd->getOrient();
  } else {
    if (targetCell->getTechSite() == nullptr) {
      std::cout << "ERROR: target master has no placement site\n";
      return false;
    }
    if (!resize) {
      const auto siteOrientation = grid->getPlacementSiteOrientation(x, y, *targetCell);
      if (!siteOrientation.has_value()) {
        std::cout << "ERROR: target origin has no compatible site orientation\n";
        return false;
      }
      orientation = *siteOrientation;
    } else if (targetMaster->isFiller() || targetCell->getType().isBlock()
               || targetCell->getType().isPad() || targetCell->getType().isCover()
               || targetCell->getWidth().getStorage() <= 0
               || targetCell->getHeight().getStorage() <= 0) {
      std::cout << "ERROR: resize requires a site-aligned standard-cell master\n";
      return false;
    }
    temporary.setMaster(targetMaster);
    temporary.setType(targetMaster->isFiller() ? Node::FILLER : Node::CELL);
    temporary.setWidth(DbuX{targetCell->getWidth().getStorage()});
    temporary.setHeight(DbuY{targetCell->getHeight().getStorage()});
    temporary.setOrient(orientation);
    temporary.setFixed(false);
    temporary.setPlaced(false);
    temporary.setLeft(DbuX{left});
    temporary.setBottom(DbuY{bottom});
    const Pixel* originPixel = grid->gridPixel(x, y);
    const Node* anchor = replacedStd != nullptr ? replacedStd
        : originPixel != nullptr && originPixel->cell != nullptr
              ? originPixel->cell : overlays.front();
    temporary.setId(anchor->getId());
    temporary.setDbInst(anchor->getDbInst());
  }

  std::vector<CellChangeRecord> overlayChanges;
  overlayChanges.reserve(overlays.size());
  std::cout << "\n[test_filler_repair] ===== REQUEST =====\n"
            << "  operation=" << operation << '\n'
            << "  targetMaster=" << (targetMaster != nullptr ? targetMaster->getId() : -1) << '\n'
            << "  origin(core DBU)=(" << left << ',' << bottom << ")\n"
            << "  origin(grid column,row)=(" << x.v << ',' << y.v << ")\n"
            << "  Delete=remove from layout; Add=create in layout; Replace=keep ID/update master+placement\n";
  for (const Node* node : overlays) {
    overlayChanges.push_back(deleteRecord(*node));
  }
  printChanges("INPUT OVERLAY (SIMULATION ONLY)", overlayChanges, *checker, true, true);
  std::cout << "  Input Deletes describe the checker snapshot; they are NOT COMMITTED here.\n";

  std::vector<CellChangeRecord> changes;
  const Node* target = removeOnly ? nullptr : &temporary;
  bool accepted = false;
  if (resize) {
    if (!probeResize(*dePlace, *checker, *replacedStd, temporary, diameter, changes)) {
      return false;
    }
    left = temporary.getLeft().v;
    bottom = temporary.getBottom().v;
    orientation = temporary.getOrient();
    accepted = true;
  } else {
    std::cout << "\n[test_filler_repair] ===== FINAL CHECK BEGIN =====\n"
              << "  validator: ImplantLayerChecker\n";
    accepted = checker->check(target, x, y, orientation, changes, overlayChanges);
  }
  if (!changes.empty()
      && (setting == nullptr || !validFillerChanges(changes, *setting))) {
    std::cout << "ERROR: repair returned an invalid filler transaction record\n";
    return false;
  }
  std::cout << (accepted ? "LEGAL" : "NO REPAIR")
            << ": operation=" << operation
            << " targetMaster=" << (targetMaster != nullptr ? targetMaster->getId() : -1)
            << " overlays=" << overlays.size()
            << " fillerChanges=" << changes.size() << '\n';
  printChanges("FINAL FILLER CHANGES", changes, *checker, false);
  if (accepted) {
    std::cout << "  This is the final FR result, not preparation records.\n"
              << "  Accepted record details appear once in FINAL COMMIT TRANSACTION.\n";
  } else {
    std::cout << "  No accepted FR plan; no final transaction will be committed.\n";
  }
  if (!accepted) {
    const ipl::CheckRequest request{
        target, x, y, orientation, overlayChanges};
    printRejectedRequest(checker->checkDirect(request));
    std::cout << "\n[test_filler_repair] ===== COMMIT RESULT =====\n"
              << "  committed: records=0\n"
              << "  status=NOT_COMMITTED; no accepted transaction\n";
    return false;
  }

  // The temporary Node's bound ID identifies an overlay, not a newly created
  // instance. A std replacement preserves its identity and net connections;
  // buffer insertion needs a fresh DB cell. FR supplies all filler operations.
  std::vector<CellChangeRecord> transaction;
  if (removeOnly) {
    transaction = overlayChanges;  // The old std Delete is owned by the caller.
  } else if (replacedStd != nullptr) {
    transaction.push_back(
        CellChangeRecord{OpType::Replace,
                         replacedStd->getDbInst(),
                         UvDist(left),
                         UvDist(bottom),
                         replacedStd->getMaster()->getDbMaster(),
                         targetMaster->getDbMaster(),
                         orientation});
  } else {
    std::set<std::string> names;
    for (const auto& [id, node] : network->getNodes()) {
      (void) id;
      if (node != nullptr && node->getDbInst().isValid()) {
        names.insert(
            design->getHierMgr()->getLeafCell(node->getDbInst()).getName());
      }
    }
    for (const auto& change : changes) {
      if (const auto* name = std::get_if<std::string>(&change.cell_data_)) {
        names.insert(*name);
      }
    }
    const std::string base
        = "FR_TARGET_" + std::to_string(temporary.getDbInst().getIndexValue());
    std::string name = base;
    for (int suffix = 1; names.count(name) != 0; ++suffix) {
      name = base + "_" + std::to_string(suffix);
    }
    transaction.push_back(CellChangeRecord{OpType::Add,
                                           name,
                                           UvDist(left),
                                           UvDist(bottom),
                                           eLIB::LibCellID(),
                                           targetMaster->getDbMaster(),
                                           orientation});
  }
  transaction.insert(transaction.end(), changes.begin(), changes.end());
  printChanges("FINAL COMMIT TRANSACTION", transaction, *checker);
  std::cout << "  Only this complete list is committed once. Do not add overlay/pending records again.\n"
            << "  FILLER Delete/Add/Replace: UDM + Network + Grid\n"
            << "  STD REPLACE/DELETE: UDM unchanged; caller-owned\n"
            << "  STD ADD: this command creates UDM first; DePlace imports its ID into Network/Grid\n";
  // This command is the caller: it creates a std instance before DePlace
  // imports its real ID. DePlace itself never changes std-cell UDM state.
  eUNL::NlEditor editor(design);
  std::unique_ptr<eUNL::UnlChange_addCell> standardAdd;
  if (!transaction.empty() && transaction.front().op_ == OpType::Add
      && !targetMaster->isFiller()) {
    const auto& record = transaction.front();
    standardAdd = std::make_unique<eUNL::UnlChange_addCell>(
        &editor, *design, eUNL::UnlChangePhaseE::PRE_CHANGE,
        std::get<std::string>(record.cell_data_),
        design->getLibAcc().getLibCell(record.new_lib_cell_).getMaster(),
        Point2D(record.x_ + grid->getCore().getXL(),
                record.y_ + grid->getCore().getYL()), record.orientation_);
    if (!standardAdd->feasible()) {
      std::cout << "ERROR: cannot create the target std UDM instance\n";
      std::cout << "\n[test_filler_repair] ===== COMMIT RESULT =====\n"
                << "  committed: records=0\n"
                << "  status=NOT_COMMITTED; target std creation is infeasible\n";
      return false;
    }
    standardAdd->commit();
  }
  if (!dePlace->commit(transaction)) {
    if (standardAdd != nullptr) {
      eUNL::UnlChange_removeCell remove(
          &editor, *design, eUNL::UnlChangePhaseE::PRE_CHANGE,
          standardAdd->getCellId());
      remove.commit();
    }
    std::cout << "\n[test_filler_repair] ===== COMMIT RESULT =====\n"
              << "  committed: records=0\n"
              << "  status=REJECTED; no placement transaction committed\n";
    if (standardAdd != nullptr) {
      std::cout << "  Caller-created std UDM instance was removed; UDM may retain its ID/tombstone.\n";
    }
    std::cout << "ERROR: commit rejected; Network/Grid placement was not changed\n";
    return false;
  }
  std::cout << "\n[test_filler_repair] ===== COMMIT RESULT =====\n"
            << "  committed: records=" << transaction.size() << '\n'
            << "COMMITTED: operation=" << operation << " origin(core DBU)=(" << left << ','
            << bottom << ")\n"
            << "  final filler records=" << changes.size() << '\n'
            << "  Finalized FR records only; preparation is not counted separately.\n";
  for (const auto& record : transaction) {
    const auto* id = std::get_if<eUNL::LeafCellID>(&record.cell_data_);
    if (record.op_ == OpType::Delete) {
      const Master* master = network->getMaster(record.orig_lib_cell_);
      const bool filler = master != nullptr && master->isFiller();
      std::cout << "    op=DELETE kind=" << (filler ? "FILLER" : "STD")
                << " leafId=" << (id != nullptr ? id->getIndexValue() : -1) << '\n'
                << "      removed from " << (filler ? "UDM + Network + Grid" : "Network + Grid; UDM unchanged")
                << '\n';
      continue;
    }
    Node* node = id != nullptr
                     ? network->getNode(*id)
                     : findNode(*network,
                                *design,
                                std::get<std::string>(record.cell_data_));
    if (node == nullptr || node->getMaster() == nullptr
        || node->getMaster()->getPhysLibCell() == nullptr) {
      std::cout << "    WARNING: committed record was not found in Network\n";
      continue;
    }
    const auto& physical = *node->getMaster()->getPhysLibCell();
    std::cout << "    op=" << (record.op_ == OpType::Add ? "ADD" : "REPLACE")
              << " kind=" << (node->isFiller() ? "FILLER" : "STD") << '\n'
              << "      name=" << design->getHierMgr()->getLeafCell(node->getDbInst()).getName() << '\n'
              << "      nodeId=" << node->getId() << " leafId=" << node->getDbInst().getIndexValue() << '\n'
              << "      master=" << physical.getLibCell().getName()
              << " networkId=" << node->getMaster()->getId() << '\n'
              << "      widthSites=" << grid->gridWidth(node).v
              << " height(DBU)=" << node->getHeight().v << " origin(core DBU)=("
              << node->getLeft().v << ',' << node->getBottom().v
              << ") orient=" << static_cast<int>(node->getOrient().getValue())
              << '\n';
    if (!node->isFiller()) {
      std::cout << (record.op_ == OpType::Add
                        ? "      UDM std instance created by command; Network/Grid imported\n"
                        : "      UDM std master/placement unchanged; caller-owned\n");
    }
  }
  return true;
}

}  // namespace dpl2
