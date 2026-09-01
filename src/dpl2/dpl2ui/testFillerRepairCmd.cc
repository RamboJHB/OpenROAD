#include <PlacementDRC.h>
#include <dpl2/DRCChecker.h>
#include <dpl2/DePlace.h>
#include <drc/ImplantLayerChecker.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

#include <algorithm>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
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
        return change.op_ == OpType::Replace
               && std::holds_alternative<eUNL::LeafCellID>(change.cell_data_)
               && setting.isFillerCell(change.orig_lib_cell_)
               && setting.isFillerCell(change.new_lib_cell_);
      });
}

void printChanges(const std::vector<CellChangeRecord>& changes)
{
  for (const CellChangeRecord& change : changes) {
    if (change.op_ == OpType::Add) {
      const auto* name = std::get_if<std::string>(&change.cell_data_);
      std::cout << "    add=" << (name != nullptr ? *name : "<invalid>")
                << " master=" << change.new_lib_cell_.getIndexValue()
                << " origin=(" << change.x_.getStorage() << ','
                << change.y_.getStorage() << ") orient="
                << static_cast<int>(change.orientation_.getValue()) << '\n';
      continue;
    }
    const auto* id = std::get_if<eUNL::LeafCellID>(&change.cell_data_);
    std::cout << "    filler=" << (id != nullptr ? id->getIndexValue() : -1)
              << " master=" << change.orig_lib_cell_.getIndexValue() << " -> "
              << change.new_lib_cell_.getIndexValue() << '\n';
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

bool TestFillerRepairCmd::exec()
{
  const std::vector<std::string> instanceIds
      = splitIdentifiers(instOpt_.getValue());
  const std::vector<std::string> masterIds
      = splitIdentifiers(masterOpt_.getValue());
  if (instanceIds.empty() || masterIds.size() != 1) {
    std::cout << "ERROR: -inst and -master are both required; -inst accepts "
                 "one or more names/Network ids and -master accepts one\n";
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

  Master* targetMaster = findMaster(*design, *network, masterIds.front());
  if (targetMaster == nullptr || targetMaster->getPhysLibCell() == nullptr) {
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
  for (const Node* node : overlays) {
    left = std::min(left, node->getLeft().v);
    bottom = std::min(bottom, node->getBottom().v);
  }
  const GridX x = grid->gridX(DbuX{left});
  const GridY y = grid->gridSnapDownY(DbuY{bottom});
  const eLIB::PhysLibCell& targetCell = *targetMaster->getPhysLibCell();
  const eLIB::TechSite* targetSite = targetCell.getTechSite();
  if (targetSite == nullptr) {
    std::cout << "ERROR: target master has no placement site\n";
    return false;
  }
  const std::optional<eUTL::PhysOrientation> orientation
      = grid->getSiteOrientation(x, y, targetSite->getName());
  if (!orientation.has_value()) {
    std::cout << "ERROR: target origin has no compatible site orientation\n";
    return false;
  }

  Node temporary;
  temporary.setMaster(targetMaster);
  temporary.setType(targetMaster->isFiller() ? Node::FILLER : Node::CELL);
  temporary.setWidth(DbuX{targetCell.getWidth().getStorage()});
  temporary.setHeight(DbuY{targetCell.getHeight().getStorage()});
  temporary.setOrient(*orientation);
  temporary.setFixed(false);
  temporary.setPlaced(false);
  temporary.setLeft(DbuX{left});
  temporary.setBottom(DbuY{bottom});
  const Pixel* originPixel = grid->gridPixel(x, y);
  Node* anchor = originPixel != nullptr ? originPixel->cell : nullptr;
  if (anchor != nullptr) {
    temporary.setId(anchor->getId());
    temporary.setDbInst(anchor->getDbInst());
  } else {
    temporary.setId(overlays.front()->getId());
    temporary.setDbInst(overlays.front()->getDbInst());
  }

  std::vector<CellChangeRecord> overlayChanges;
  overlayChanges.reserve(overlays.size());
  std::cout << "checker probe: targetMaster=" << targetMaster->getId()
            << " overlays=" << overlays.size() << " origin=(" << left << ','
            << bottom << ")\n";
  for (const Node* node : overlays) {
    overlayChanges.push_back(deleteRecord(*node));
    std::cout << "    node=" << node->getId()
              << " master=" << node->getMaster()->getId()
              << " kind=" << (node->isFiller() ? "filler" : "std") << '\n';
  }

  std::vector<CellChangeRecord> changes;
  const bool accepted
      = checker->check(&temporary, x, y, *orientation, changes, overlayChanges);
  if (!changes.empty()
      && (setting == nullptr || !validFillerChanges(changes, *setting))) {
    std::cout << "ERROR: repair returned an invalid Add/Replace filler record\n";
    return false;
  }
  std::cout << (accepted ? "LEGAL" : "NO REPAIR")
            << ": targetMaster=" << targetMaster->getId()
            << " overlays=" << overlays.size()
            << " fillerChanges=" << changes.size() << '\n';
  printChanges(changes);
  if (!accepted) {
    const ipl::CheckRequest request{
        &temporary, x, y, *orientation, overlayChanges};
    printRejectedRequest(checker->checkDirect(request));
  }
  return accepted;
}

}  // namespace dpl2
