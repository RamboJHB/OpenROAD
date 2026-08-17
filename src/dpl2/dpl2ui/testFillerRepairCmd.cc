#include <testFillerRepairCmd.hh>
#include <FillerRepairDumpReplay.hh>

#include <dpl2/DePlace.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

// UDM
#include <phys/physDesMgr.hh>
#include <util/iter.hh>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace eUNL;

namespace dpl2 {

// =============================================================================
// Below this line: no command-framework dependency.
// =============================================================================

namespace {

// A real design has millions of cells; every proposal is a full checker call
// plus, on a violation, a repair search. These bound the sweep to something
// interactive. Raise them for a soak run. Targeted mode ignores both.
constexpr int kMaxProposals = 2000;
constexpr int kMaxReportedLines = 50;

enum class CommandOperation
{
  Sweep,
  Replace,
  Delete,
  Add
};

std::string lowerCase(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

const char* operationName(CommandOperation operation)
{
  switch (operation) {
    case CommandOperation::Sweep:
      return "SWEEP";
    case CommandOperation::Replace:
      return "REPLACE";
    case CommandOperation::Delete:
      return "DELETE";
    case CommandOperation::Add:
      return "ADD";
  }
  return "UNKNOWN";
}

std::optional<CommandOperation> parseOperation(const std::string& text)
{
  const std::string normalized = lowerCase(text);
  if (normalized == "replace") {
    return CommandOperation::Replace;
  }
  if (normalized == "delete") {
    return CommandOperation::Delete;
  }
  if (normalized == "add") {
    return CommandOperation::Add;
  }
  return {};
}

const char* orientationName(eUTL::PhysOrientation orientation)
{
  switch (orientation.getValue()) {
    case eUTL::PhysOrientationE::R0:
      return "R0";
    case eUTL::PhysOrientationE::R90:
      return "R90";
    case eUTL::PhysOrientationE::R180:
      return "R180";
    case eUTL::PhysOrientationE::R270:
      return "R270";
    case eUTL::PhysOrientationE::MX:
      return "MX";
    case eUTL::PhysOrientationE::MX90:
      return "MX90";
    case eUTL::PhysOrientationE::MY:
      return "MY";
    case eUTL::PhysOrientationE::MY90:
      return "MY90";
  }
  return "UNKNOWN";
}

std::optional<eUTL::PhysOrientation> parseOrientation(
    const std::string& text)
{
  std::string normalized = text;
  std::transform(normalized.begin(),
                 normalized.end(),
                 normalized.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::toupper(ch));
                 });
  if (normalized == "R0") {
    return eUTL::PhysOrientationE::R0;
  }
  if (normalized == "R180") {
    return eUTL::PhysOrientationE::R180;
  }
  if (normalized == "MX") {
    return eUTL::PhysOrientationE::MX;
  }
  if (normalized == "MY") {
    return eUTL::PhysOrientationE::MY;
  }
  return {};
}

// Same footprint means the Grid occupancy does not change when the master is
// swapped, so a proposal needs no unplace/place around it -- which is what
// keeps this command non-destructive.
struct Footprint
{
  int64_t width = 0;
  int64_t height = 0;

  bool operator<(const Footprint& other) const
  {
    return width != other.width ? width < other.width : height < other.height;
  }

  bool operator!=(const Footprint& other) const
  {
    return width != other.width || height != other.height;
  }
};

Footprint footprintOf(const eLIB::PhysLibCell& cell)
{
  return Footprint{cell.getWidth().getStorage(),
                   cell.getHeight().getStorage()};
}

// Candidates are drawn ONLY from masters already registered in Network. The
// immutable engine snapshot resolves new_lib_cell_ through that registry;
// registering masters here would also grow shared state without an edge table.
std::map<Footprint, std::vector<const eLIB::PhysLibCell*>> buildCandidateIndex(
    Network* network)
{
  std::map<Footprint, std::vector<const eLIB::PhysLibCell*>> index;
  for (const auto& [masterId, master] : network->getMasters()) {
    (void) masterId;
    if (!master) {
      continue;
    }
    const eLIB::PhysLibCell* cell = master->getPhysLibCell();
    if (cell == nullptr || master->isFiller()) {
      continue;
    }
    index[footprintOf(*cell)].push_back(cell);
  }
  return index;
}

bool isAllDigits(const std::string& text)
{
  return !text.empty()
      && text.find_first_not_of("0123456789") == std::string::npos;
}

bool parseNonNegativeInt(const std::string& text, int& value)
{
  if (!isAllDigits(text)) {
    return false;
  }
  try {
    const unsigned long long parsed = std::stoull(text);
    if (parsed > static_cast<unsigned long long>(
                     std::numeric_limits<int>::max())) {
      return false;
    }
    value = static_cast<int>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// `-inst` accepts either form: the node id the sweep prints (always available,
// nothing to look up) or the instance name (what a user reading their own
// netlist has to hand).
Node* findNode(Network* network,
               PhysDesMgr* desMgr,
               const std::string& instance)
{
  if (isAllDigits(instance)) {
    int nodeId = -1;
    return parseNonNegativeInt(instance, nodeId) ? network->getNode(nodeId)
                                                  : nullptr;
  }
  for (auto& [nodeId, node] : network->getNodes()) {
    (void) nodeId;
    if (!node) {
      continue;
    }
    const PhysCell cell = desMgr->getPhysCell(node->getDbInst());
    if (cell.isValid() && cell.getName() == instance) {
      return node.get();
    }
  }
  return nullptr;
}

// Same resolution fillerSetting::addFillerCell uses: name -> module -> lib
// cell -> physical lib cell. Also accepts a numeric master ID (as printed by
// ImplantLayerChecker::printStats) when Network is non-null.
const eLIB::PhysLibCell* findMaster(eUNL::Design* design,
                                    Network* network,
                                    const std::string& masterName)
{
  if (isAllDigits(masterName) && network != nullptr) {
    int masterId = -1;
    if (!parseNonNegativeInt(masterName, masterId)) {
      return nullptr;
    }
    Master* master = network->getMaster(masterId);
    if (master != nullptr) {
      return master->getPhysLibCell();
    }
    return nullptr;
  }
  const eFNL::ModuleID moduleId = design->getLibAcc().findModule(masterName);
  const eLIB::LibCell* libCell = moduleId.isValid()
      ? design->getLibAcc().getLibCell(moduleId) : nullptr;
  if (libCell == nullptr) {
    return nullptr;
  }
  return &design->getLibAcc().getPhysLibCell(libCell->getId());
}

// Everything one transaction needs, so sweep and targeted paths share the
// same non-mutating checker entry.
struct ProposalResult
{
  bool evaluated = false;
  bool legal = false;
  std::vector<CellChangeRecord> changes;
};

// [FRPORT] Exercise the public DePlace -> checker -> engine call chain.
ProposalResult evaluateTarget(const DePlace& dePlace,
                              const CellChangeRecord& targetChange)
{
  ProposalResult result;
  result.evaluated = true;
  result.legal = dePlace.repairFillers(targetChange, result.changes);
  return result;
}

std::optional<CellChangeRecord> makeExistingTarget(
    PhysDesMgr* desMgr,
    const Node& node,
    OpType operation,
    LibCellID newMaster,
    std::optional<eUTL::PhysOrientation> orientation = {})
{
  const eUNL::PhysCell physical = desMgr->getPhysCell(node.getDbInst());
  const Master* original = node.getMaster();
  if (!physical.isValid() || original == nullptr) {
    return {};
  }
  const eUTL::Point2D origin = physical.getOrigin();
  const LibCellID originalMaster = original->getDbMaster();
  return CellChangeRecord{operation,
                          CellData{node.getDbInst()},
                          origin.getX(),
                          origin.getY(),
                          originalMaster,
                          operation == OpType::Delete ? originalMaster
                                                      : newMaster,
                          orientation.value_or(physical.getOrient())};
}

struct ChangeCounts
{
  int adds = 0;
  int deletes = 0;
  int replaces = 0;
};

ChangeCounts countChanges(const std::vector<CellChangeRecord>& changes)
{
  ChangeCounts counts;
  for (const CellChangeRecord& change : changes) {
    if (change.op_ == OpType::Add) {
      ++counts.adds;
    } else if (change.op_ == OpType::Delete) {
      ++counts.deletes;
    } else {
      ++counts.replaces;
    }
  }
  return counts;
}

bool validateTransaction(CommandOperation targetOperation,
                         const ProposalResult& proposal,
                         const std::function<bool(LibCellID)>& isFiller,
                         std::string& error)
{
  if (!proposal.legal) {
    if (!proposal.changes.empty()) {
      error = "failed repair returned a partial filler transaction";
      return false;
    }
    return true;
  }

  const ChangeCounts counts = countChanges(proposal.changes);
  if (targetOperation == CommandOperation::Delete && counts.adds == 0) {
    error = "successful std-cell Delete did not add filler into its hole";
    return false;
  }
  if (targetOperation == CommandOperation::Add && counts.deletes == 0) {
    error = "successful std-cell Add did not delete any covered filler";
    return false;
  }
  if (targetOperation == CommandOperation::Replace
      && (counts.adds != 0 || counts.deletes != 0)) {
    error = "same-footprint Replace returned an unexpected filler Add/Delete";
    return false;
  }
  for (const CellChangeRecord& change : proposal.changes) {
    const bool hasLeaf = std::holds_alternative<LeafCellID>(change.cell_data_);
    if ((change.op_ == OpType::Add && hasLeaf)
        || (change.op_ != OpType::Add && !hasLeaf)) {
      error = "filler transaction contains an operation/cell-data mismatch";
      return false;
    }
    const std::string* addName = std::get_if<std::string>(&change.cell_data_);
    if (change.op_ == OpType::Add
        && (addName == nullptr || addName->empty())) {
      error = "filler Add record has an empty request-local name";
      return false;
    }
    const bool validMasters
        = change.op_ == OpType::Add
              ? isFiller(change.new_lib_cell_)
          : change.op_ == OpType::Delete
              ? isFiller(change.orig_lib_cell_)
              : isFiller(change.orig_lib_cell_)
                    && isFiller(change.new_lib_cell_);
    if (!validMasters) {
      error = "filler transaction references a non-filler master";
      return false;
    }
  }
  return true;
}

// A command-side fingerprint keeps the pre-commit contract observable on a
// real design without copying the database. Master registration happens
// before this snapshot and is deliberately outside the transaction check.
struct RuntimeFingerprint
{
  uint64_t network = 1469598103934665603ULL;
  uint64_t physical = 1469598103934665603ULL;
  uint64_t grid = 1469598103934665603ULL;

  bool operator==(const RuntimeFingerprint& other) const
  {
    return network == other.network && physical == other.physical
           && grid == other.grid;
  }
};

void hashValue(uint64_t& hash, int64_t value)
{
  hash ^= static_cast<uint64_t>(value);
  hash *= 1099511628211ULL;
}

void hashText(uint64_t& hash, const std::string& text)
{
  for (unsigned char ch : text) {
    hashValue(hash, ch);
  }
  hashValue(hash, -1);
}

RuntimeFingerprint fingerprintRuntime(const Grid& grid,
                                      Network& network,
                                      PhysDesMgr* desMgr)
{
  RuntimeFingerprint fingerprint;
  for (const auto& [nodeId, ownedNode] : network.getNodes()) {
    (void) nodeId;
    const Node* node = ownedNode.get();
    hashValue(fingerprint.network, node != nullptr);
    if (node == nullptr) {
      continue;
    }
    hashValue(fingerprint.network, node->getId());
    hashValue(fingerprint.network, node->getDbInst().getIndexValue());
    hashValue(fingerprint.network, node->getLeft().v);
    hashValue(fingerprint.network, node->getBottom().v);
    hashValue(fingerprint.network, node->getWidth().v);
    hashValue(fingerprint.network, node->getHeight().v);
    hashValue(fingerprint.network, static_cast<int>(node->getType()));
    hashValue(fingerprint.network, node->isFixed());
    hashValue(fingerprint.network, node->isPlaced());
    hashValue(fingerprint.network,
              static_cast<int>(node->getOrient().getValue()));
    hashValue(fingerprint.network,
              node->getMaster() != nullptr ? node->getMaster()->getId() : -1);

    const eUNL::PhysCell cell = desMgr->getPhysCell(node->getDbInst());
    hashValue(fingerprint.physical, cell.isValid());
    if (!cell.isValid()) {
      continue;
    }
    hashText(fingerprint.physical, cell.getName());
    hashValue(fingerprint.physical,
              cell.getPhysMaster().getLibCellId().getIndexValue());
    hashValue(fingerprint.physical,
              cell.getOrigin().getX().getStorage());
    hashValue(fingerprint.physical,
              cell.getOrigin().getY().getStorage());
    hashValue(fingerprint.physical,
              static_cast<int>(cell.getOrient().getValue()));
    hashValue(fingerprint.physical, static_cast<int>(cell.getStatus()));
  }

  for (int row = 0; row < grid.getRowCount().v; ++row) {
    for (int col = 0; col < grid.getRowSiteCount().v; ++col) {
      const Pixel* pixel = grid.gridPixel(GridX{col}, GridY{row});
      hashValue(fingerprint.grid, pixel != nullptr);
      if (pixel == nullptr) {
        continue;
      }
      hashValue(fingerprint.grid, pixel->is_valid);
      hashValue(fingerprint.grid, pixel->is_hopeless);
      hashValue(fingerprint.grid, pixel->blocked_layers);
      hashValue(fingerprint.grid,
                pixel->cell != nullptr ? pixel->cell->getId() : -1);
      hashValue(fingerprint.grid,
                pixel->padding_reserved_by != nullptr
                    ? pixel->padding_reserved_by->getId()
                    : -1);
    }
  }
  return fingerprint;
}

void printChanges(const std::vector<CellChangeRecord>& changes,
                  const std::function<std::string(LibCellID)>& nameOf,
                  const char* indent)
{
  int index = 0;
  for (const CellChangeRecord& record : changes) {
    const LeafCellID* leafId = std::get_if<LeafCellID>(&record.cell_data_);
    const std::string* addName = std::get_if<std::string>(&record.cell_data_);
    const char* operation = record.op_ == OpType::Add
                                ? "ADD"
                            : record.op_ == OpType::Delete ? "DELETE"
                                                          : "REPLACE";
    std::cout << indent << "change[" << index++ << "]\n";
    std::cout << indent << "  operation   : " << operation << "\n";
    std::cout << indent << "  filler      : ";
    if (leafId != nullptr) {
      std::cout << leafId->getIndexValue();
    } else {
      std::cout << (addName != nullptr ? *addName : "<invalid>");
    }
    std::cout << "\n";
    std::cout << indent << "  origin      : (" << record.x_.getStorage()
              << ", " << record.y_.getStorage() << ")\n";
    std::cout << indent << "  master      : ";
    if (record.op_ != OpType::Add) {
      std::cout << nameOf(record.orig_lib_cell_);
    }
    if (record.op_ == OpType::Replace) {
      std::cout << " -> " << nameOf(record.new_lib_cell_);
    } else if (record.op_ == OpType::Add) {
      std::cout << nameOf(record.new_lib_cell_);
    }
    std::cout << "\n";
    std::cout << indent << "  orientation : "
              << orientationName(record.orientation_) << "\n";
  }
}

}  // namespace

bool TestFillerRepairCmd::exec()
{
  std::cout << "========================================\n";
  std::cout << "  test_filler_repair: implant + repair  \n";
  std::cout << "========================================\n";

  const std::string instanceOpt = instOpt_.getValue();
  const std::string masterOpt = masterOpt_.getValue();
  const std::string operationOpt = operationOpt_.getValue();
  const std::string orientOpt = orientOpt_.getValue();
  const std::string rowOpt = rowOpt_.getValue();
  const std::string colOpt = colOpt_.getValue();
  const std::string loadOpt = loadOpt_.getValue();
  const bool haveInstance = !instanceOpt.empty();
  const bool haveMaster = !masterOpt.empty();
  const bool haveOrientation = !orientOpt.empty();
  const bool haveRow = !rowOpt.empty();
  const bool haveCol = !colOpt.empty();

  CommandOperation operation = CommandOperation::Sweep;
  if (operationOpt.empty()) {
    if (haveInstance && haveMaster && !haveRow && !haveCol) {
      operation = CommandOperation::Replace;
    } else if (haveInstance || haveMaster || haveRow || haveCol
               || haveOrientation) {
      std::cout << "ERROR: omit all target options to sweep, or provide "
                   "-inst and -master for the backward-compatible Replace "
                   "form\n";
      return false;
    }
  } else {
    const std::optional<CommandOperation> parsed
        = parseOperation(operationOpt);
    if (!parsed.has_value()) {
      std::cout << "ERROR: -operation must be replace, delete, or add\n";
      return false;
    }
    operation = *parsed;
  }

  if (operation == CommandOperation::Replace
      && (!haveInstance || !haveMaster || haveRow || haveCol)) {
    std::cout << "ERROR: Replace requires -inst and -master, and does not "
                 "accept -row/-col\n";
    return false;
  }
  if (operation == CommandOperation::Delete
      && (!haveInstance || haveMaster || haveRow || haveCol
          || haveOrientation)) {
    std::cout << "ERROR: Delete requires only -inst; its original master, "
                 "origin, and orientation come from the existing cell\n";
    return false;
  }
  if (operation == CommandOperation::Add
      && (haveInstance || !haveMaster || !haveRow || !haveCol)) {
    std::cout << "ERROR: Add requires -master, -row, and -col, and does not "
                 "accept -inst\n";
    return false;
  }
  const bool targeted = operation != CommandOperation::Sweep;

  std::optional<eUTL::PhysOrientation> requestedOrientation;
  if (haveOrientation) {
    requestedOrientation = parseOrientation(orientOpt);
    if (!requestedOrientation.has_value()) {
      std::cout << "ERROR: -orient must be R0, R180, MX, or MY\n";
      return false;
    }
  }

  int requestedRow = -1;
  int requestedCol = -1;
  if (operation == CommandOperation::Add
      && (!parseNonNegativeInt(rowOpt, requestedRow)
          || !parseNonNegativeInt(colOpt, requestedCol))) {
    std::cout << "ERROR: -row and -col must be non-negative integers\n";
    return false;
  }

  // [FRPORT] A helper dump is self-contained: rebuild its Grid/Network/checker and
  // run the pure planner before touching Session, DePlace, or UDM.
  if (!loadOpt.empty()) {
    if (operation == CommandOperation::Add
        || operation == CommandOperation::Delete || haveOrientation
        || haveRow || haveCol) {
      std::cout << "ERROR: -load supports only sweep or numeric Replace; "
                   "Add/Delete require a loaded UDM-backed design\n";
      return false;
    }
    FillerRepairDumpReplayOptions options;
    options.maxProposals = kMaxProposals;
    options.maxReportedLines = kMaxReportedLines;
    if (targeted
        && (!parseNonNegativeInt(instanceOpt, options.instanceId)
            || !parseNonNegativeInt(masterOpt, options.masterId))) {
      std::cout << "ERROR: with -load, -inst and -master must be numeric "
                   "node/master ids stored in the dump\n";
      return false;
    }
    const FillerRepairDumpReplayResult replay
        = replayFillerRepairDump(loadOpt, options, std::cout);
    if (!replay.completed) {
      std::cout << "ERROR: " << replay.error << "\n";
      return false;
    }
    std::cout << "\n========================================\n";
    std::cout << (replay.passed
                      ? "  test_filler_repair PASSED\n"
                      : "  test_filler_repair FAILED (dirty baseline)\n");
    std::cout << "========================================\n";
    return replay.passed;
  }

  DePlace* de_place = DePlace::get();
  eUNL::Design* design = de_place->getDesign();
  if (!design) {
    std::cout << "ERROR: no design loaded\n";
    return false;
  }

  PhysDesMgr* desMgr = de_place->getDesMgr();
  if (!desMgr) {
    std::cout << "ERROR: DePlace not initialized (no desMgr)\n";
    return false;
  }
  Grid* grid = de_place->getGrid();
  Network* network = de_place->getNetwork();
  if (!grid || !network || grid->getPixelYSize() == 0) {
    std::cout << "ERROR: empty placement grid\n";
    return false;
  }
  std::cout << "grid: " << grid->getRowCount() << " x "
            << grid->getRowSiteCount() << "\n";

  if (!grid->isFullUtil()) {
    std::cout << "ERROR: placement grid is not fully utilized; filler repair "
                 "assumes every legal site is occupied\n";
    return false;
  }

  // [FRPORT] Read DePlace's owned setting and register its candidates through
  // the real infrastructure edge table before engine initialization.
  fillerSetting* setting = de_place->getFillerSetting();

  if (!setting || setting->getFillerPhysCells().empty()) {
    std::cout << "ERROR: no filler masters configured -- run "
                 "set_filler_option first\n";
    return false;
  }
  std::cout << "configured filler masters: "
            << setting->getFillerPhysCells().size() << "\n";

  // Resolve the target before constructing the immutable engine. A Replace
  // or Add master not already used by the design still needs a complete
  // Network Master built with DePlace's real edge table.
  Node* targetedNode = nullptr;
  const eLIB::PhysLibCell* targetedOriginal = nullptr;
  const eLIB::PhysLibCell* targetedCandidate = nullptr;
  std::vector<const eLIB::PhysLibCell*> targetMasters;
  if (operation == CommandOperation::Replace
      || operation == CommandOperation::Delete) {
    targetedNode = findNode(network, desMgr, instanceOpt);
    if (targetedNode == nullptr) {
      std::cout << "ERROR: no such instance: " << instanceOpt << "\n";
      return false;
    }
    if (!targetedNode->isStdCell()) {
      std::cout << "ERROR: instance is not a standard cell\n";
      return false;
    }
    if (targetedNode->isFixed()) {
      std::cout << "ERROR: instance is fixed\n";
      return false;
    }
    Master* originalMaster = targetedNode->getMaster();
    targetedOriginal = originalMaster != nullptr
                           ? originalMaster->getPhysLibCell()
                           : nullptr;
    if (targetedOriginal == nullptr) {
      std::cout << "ERROR: instance has no physical master\n";
      return false;
    }
  }
  if (operation == CommandOperation::Replace
      || operation == CommandOperation::Add) {
    targetedCandidate = findMaster(design, network, masterOpt);
    if (targetedCandidate == nullptr) {
      std::cout << "ERROR: no such master: " << masterOpt << "\n";
      return false;
    }
    if (setting->isFillerCell(targetedCandidate->getLibCellId())) {
      std::cout << "ERROR: target master is a filler; the target of a repair "
                   "must be a standard cell\n";
      return false;
    }
    targetMasters.push_back(targetedCandidate);
  }

  if (operation == CommandOperation::Replace
      && footprintOf(*targetedOriginal) != footprintOf(*targetedCandidate)) {
    std::cout << "ERROR: Replace requires an unchanged footprint; use Delete "
                 "and Add as separate opto transactions for a size change\n";
    return false;
  }
  if (operation == CommandOperation::Add
      && (requestedRow >= grid->getRowCount().v
          || requestedCol >= grid->getRowSiteCount().v)) {
    std::cout << "ERROR: Add location is outside the Grid: row range [0, "
              << grid->getRowCount().v << "), column range [0, "
              << grid->getRowSiteCount().v << ")\n";
    return false;
  }

  // [FRPORT] DePlace owns and publishes the revision-scoped checker/engine
  // pair after registering the complete target-master universe.
  if (!de_place->initializeFillerRepair(targetMasters)) {
    std::cout << "ERROR: DePlace could not initialize filler repair with the "
                 "configured filler/target masters\n";
    return false;
  }

  const auto nameOf = [design](LibCellID lcId) -> std::string {
    return design->getLibAcc().getPhysLibCell(lcId).getLibCell().getName();
  };
  const auto isFiller = [setting](LibCellID lcId) {
    return setting->isFillerCell(lcId);
  };

  const RuntimeFingerprint runtimeBefore
      = fingerprintRuntime(*grid, *network, desMgr);

  // =============================================================================
  // Targeted mode: one complete std-cell transaction.
  // =============================================================================
  if (targeted) {
    CellChangeRecord targetChange;
    if (operation == CommandOperation::Replace) {
      const std::optional<CellChangeRecord> change = makeExistingTarget(
          desMgr,
          *targetedNode,
          OpType::Replace,
          targetedCandidate->getLibCellId(),
          requestedOrientation);
      if (!change.has_value()) {
        std::cout << "ERROR: could not read the existing target cell\n";
        return false;
      }
      targetChange = *change;
    } else if (operation == CommandOperation::Delete) {
      const std::optional<CellChangeRecord> change = makeExistingTarget(
          desMgr,
          *targetedNode,
          OpType::Delete,
          targetedOriginal->getLibCellId());
      if (!change.has_value()) {
        std::cout << "ERROR: could not read the existing target cell\n";
        return false;
      }
      targetChange = *change;
    } else {
      if (!requestedOrientation.has_value()) {
        const eLIB::TechSite* site = targetedCandidate->getTechSite();
        if (site == nullptr) {
          std::cout << "ERROR: Add master has no technology site; specify a "
                       "valid standard-cell master\n";
          return false;
        }
        requestedOrientation = grid->getSiteOrientation(
            GridX{requestedCol}, GridY{requestedRow}, site->getName());
        if (!requestedOrientation.has_value()) {
          std::cout << "ERROR: no legal orientation for master site \""
                    << site->getName() << "\" at row=" << requestedRow
                    << " col=" << requestedCol << "\n";
          return false;
        }
      }
      const Rect core = grid->getCore();
      const int64_t x = core.getXL().getStorage()
                        + static_cast<int64_t>(requestedCol)
                              * grid->getSiteWidth().v;
      const int64_t y = core.getYL().getStorage()
                        + grid->gridYToDbu(GridY{requestedRow}).v;
      targetChange = CellChangeRecord{
          OpType::Add,
          CellData{"test_filler_repair_buffer_" + std::to_string(requestedRow)
                   + "_" + std::to_string(requestedCol)},
          UvDist{x},
          UvDist{y},
          LibCellID{},
          targetedCandidate->getLibCellId(),
          *requestedOrientation};
    }

    std::cout << "\n--- targeted transaction ---\n";
    std::cout << "  operation   : " << operationName(operation) << "\n";
    if (targetedNode != nullptr) {
      std::cout << "  instance    : " << instanceOpt << " (node "
                << targetedNode->getId() << ")\n";
    } else {
      std::cout << "  instance    : "
                << std::get<std::string>(targetChange.cell_data_) << "\n";
    }
    std::cout << "  origin      : (" << targetChange.x_.getStorage() << ", "
              << targetChange.y_.getStorage() << ")\n";
    if (operation != CommandOperation::Add) {
      std::cout << "  old master  : "
                << nameOf(targetChange.orig_lib_cell_) << "\n";
    }
    if (operation != CommandOperation::Delete) {
      std::cout << "  new master  : "
                << nameOf(targetChange.new_lib_cell_) << "\n";
    }
    std::cout << "  orientation : "
              << orientationName(targetChange.orientation_) << "\n";

    ProposalResult proposal;
    if (operation == CommandOperation::Replace
        && targetedNode != nullptr
        && targetChange.orientation_.getValue()
               == targetedNode->getOrient().getValue()) {
      // [FRPORT] Exercise the DePlace-owned, request-local isLegal path used by
      // opto for an ordinary fixed-origin master swap.
      proposal.evaluated = true;
      proposal.legal = de_place->isLegal(targetedNode->getDbInst(),
                                         targetChange.new_lib_cell_,
                                         proposal.changes);
    } else if (operation == CommandOperation::Add) {
      // [FRPORT] Exercise DePlace::findLegal at the requested site. A zero
      // radius keeps this command deterministic while still traversing the
      // complete Add -> filler Delete/collateral Add repair path.
      proposal.evaluated = true;
      CellChangeRecord placedTarget = targetChange;
      proposal.legal
          = de_place->findLegal(placedTarget, 0, proposal.changes);
    } else {
      proposal = evaluateTarget(*de_place, targetChange);
    }
    if (!proposal.evaluated) {
      std::cout << "ERROR: could not evaluate the CellChangeRecord request\n";
      return false;
    }

    std::cout << "\n--- Result ---\n";
    if (proposal.legal && proposal.changes.empty()) {
      std::cout << "  LEGAL as-is: the VT change needs no filler repair\n";
    } else if (proposal.legal) {
      std::cout << "  REPAIRED: " << proposal.changes.size()
                << " filler change(s), checker-verified\n";
      printChanges(proposal.changes, nameOf, "    ");
    } else {
      std::cout << "  ILLEGAL: no complete filler transaction was found\n";
    }

    const ChangeCounts counts = countChanges(proposal.changes);
    std::cout << "  output counts\n";
    std::cout << "    Add     : " << counts.adds << "\n";
    std::cout << "    Delete  : " << counts.deletes << "\n";
    std::cout << "    Replace : " << counts.replaces << "\n";

    std::string transactionError;
    const bool transactionValid
        = validateTransaction(operation,
                              proposal,
                              isFiller,
                              transactionError);
    if (!transactionValid) {
      std::cout << "  ERROR: " << transactionError << "\n";
    }

    const RuntimeFingerprint runtimeAfter
        = fingerprintRuntime(*grid, *network, desMgr);
    const bool unchanged = runtimeAfter == runtimeBefore;
    std::cout << "  pre-commit state\n";
    std::cout << "    Network : "
              << (runtimeAfter.network == runtimeBefore.network ? "unchanged"
                                                                 : "CHANGED")
              << "\n";
    std::cout << "    UDM     : "
              << (runtimeAfter.physical == runtimeBefore.physical
                      ? "unchanged"
                      : "CHANGED")
              << "\n";
    std::cout << "    Grid    : "
              << (runtimeAfter.grid == runtimeBefore.grid ? "unchanged"
                                                           : "CHANGED")
              << "\n";
    std::cout << "  set FR_VERBOSE=0 to silence the [fr] decision "
                 "transcript\n";

    const bool passed = proposal.legal && transactionValid && unchanged;
    std::cout << "\n========================================\n";
    std::cout << (passed ? "  test_filler_repair PASSED\n"
                         : "  test_filler_repair FAILED\n");
    std::cout << "========================================\n";
    return passed;
  }

  // =============================================================================
  // Phase 1 -- baseline. Every movable standard cell at its CURRENT master.
  // =============================================================================
  std::cout << "\n--- phase 1: baseline (current masters) ---\n";
  int baselineChecked = 0;
  int baselineIllegal = 0;
  int baselineReported = 0;
  for (auto& [nodeId, node] : network->getNodes()) {
    (void) nodeId;
    if (!node || node->isFixed() || !node->isStdCell()) {
      continue;
    }
    ++baselineChecked;
    // [FRPORT] Baseline uses the same DePlace-owned, request-local entry as
    // opto. It never reaches into the checker/engine ownership chain.
    std::vector<CellChangeRecord> fcRecord;
    const bool legal = de_place->isLegal(node->getDbInst(),
                                         node->getMaster()->getDbMaster(),
                                         fcRecord);
    if (legal && fcRecord.empty()) {
      continue;
    }
    ++baselineIllegal;
    if (baselineReported++ < kMaxReportedLines) {
      std::cout << "  dirty as placed: node=" << node->getId() << " master="
                << nameOf(node->getMaster()->getDbMaster()) << " pos=("
                << node->getLeft().v << "," << node->getBottom().v << ")"
                << (legal ? "  (repairable)" : "  (no repair found)") << "\n";
    }
  }
  std::cout << "  checked: " << baselineChecked
            << "  not clean: " << baselineIllegal << "\n";

  // =============================================================================
  // Phase 2 -- propose same-footprint master swaps.
  // =============================================================================
  std::cout << "\n--- phase 2: proposed VT swaps (max " << kMaxProposals
            << ") ---\n";
  const auto candidates = buildCandidateIndex(network);

  int proposals = 0;
  int cleanRightAway = 0;
  int repaired = 0;
  int unrepairable = 0;
  int totalSwaps = 0;
  int invalidTransactions = 0;
  int reported = 0;
  bool truncated = false;

  for (auto& [nodeId, node] : network->getNodes()) {
    (void) nodeId;
    if (proposals >= kMaxProposals) {
      truncated = true;
      break;
    }
    if (!node || node->isFixed() || !node->isStdCell()) {
      continue;
    }
    Master* master = node->getMaster();
    const eLIB::PhysLibCell* original =
        master != nullptr ? master->getPhysLibCell() : nullptr;
    if (original == nullptr) {
      continue;
    }
    const auto slot = candidates.find(footprintOf(*original));
    if (slot == candidates.end()) {
      continue;
    }
    const LibCellID originalId = original->getLibCellId();
    for (const eLIB::PhysLibCell* candidate : slot->second) {
      if (candidate->getLibCellId() == originalId) {
        continue;
      }
      ++proposals;
      const std::optional<CellChangeRecord> targetChange = makeExistingTarget(
          desMgr,
          *node,
          OpType::Replace,
          candidate->getLibCellId());
      if (!targetChange.has_value()) {
        std::cout << "ERROR: could not build proposal for node="
                  << node->getId() << "\n";
        return false;
      }
      const ProposalResult proposal
          = evaluateTarget(*de_place, *targetChange);
      std::string transactionError;
      if (!validateTransaction(CommandOperation::Replace,
                               proposal,
                               isFiller,
                               transactionError)) {
        ++invalidTransactions;
        std::cout << "ERROR: node=" << node->getId() << ": "
                  << transactionError << "\n";
      }

      if (proposal.legal && proposal.changes.empty()) {
        ++cleanRightAway;
      } else if (proposal.legal) {
        ++repaired;
        totalSwaps += static_cast<int>(proposal.changes.size());
        if (reported++ < kMaxReportedLines) {
          std::cout << "  repairable: node=" << node->getId() << "  "
                    << nameOf(originalId) << " -> "
                    << nameOf(candidate->getLibCellId())
                    << "  swaps=" << proposal.changes.size() << "\n";
          printChanges(proposal.changes, nameOf, "    ");
        }
      } else {
        ++unrepairable;
        if (reported++ < kMaxReportedLines) {
          std::cout << "  NO repair: node=" << node->getId() << "  "
                    << nameOf(originalId) << " -> "
                    << nameOf(candidate->getLibCellId()) << "\n";
        }
      }
      break;  // one proposal per cell keeps the sweep linear
    }
  }

  // ----------------------------------------------------------------------------
  std::cout << "\n--- Result ---\n";
  std::cout << "  baseline cells checked : " << baselineChecked << "\n";
  std::cout << "  baseline not clean     : " << baselineIllegal << "\n";
  std::cout << "  VT proposals evaluated : " << proposals
            << (truncated ? "  (stopped at the cap)" : "") << "\n";
  std::cout << "    legal without repair : " << cleanRightAway << "\n";
  std::cout << "    repaired by fillers  : " << repaired << "  ("
            << totalSwaps << " filler swaps proposed)\n";
  std::cout << "    no repair found      : " << unrepairable << "\n";
  std::cout << "    invalid transactions : " << invalidTransactions << "\n";
  std::cout << "  re-run with -inst <node id> -master <name> to drill into "
               "one case\n";
  std::cout << "  set FR_VERBOSE=0 to silence the [fr] decision transcript\n";

  const RuntimeFingerprint runtimeAfter
      = fingerprintRuntime(*grid, *network, desMgr);
  const bool unchanged = runtimeAfter == runtimeBefore;
  std::cout << "  pre-commit state       : "
            << (unchanged ? "unchanged" : "CHANGED") << "\n";

  const bool passed
      = baselineIllegal == 0 && invalidTransactions == 0 && unchanged;
  std::cout << "\n========================================\n";
  std::cout << (passed ? "  test_filler_repair PASSED\n"
                       : "  test_filler_repair FAILED (dirty baseline)\n");
  std::cout << "========================================\n";
  return passed;
}

}  // namespace dpl2
