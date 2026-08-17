#include "OpenRoadImportDb.h"

#include <dpl2/DePlace.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <odb/db.h>

#include <PlacementDRC.h>
#include <fake_udm.h>
#include <infrastructure/Grid.h>
#include <infrastructure/Objects.h>
#include <infrastructure/Padding.h>
#include <infrastructure/architecture.h>
#include <infrastructure/fillerSetting.h>
#include <infrastructure/network.h>

namespace {

std::unique_ptr<fake_udm::DesignDb> openroad_design;
bool deplace_constructed = false;

eUTL::PhysOrientation toUdmOrientation(odb::dbOrientType orientation)
{
  switch (orientation) {
    case odb::dbOrientType::R0:
      return eUTL::PhysOrientationE::R0;
    case odb::dbOrientType::R90:
      return eUTL::PhysOrientationE::R90;
    case odb::dbOrientType::R180:
      return eUTL::PhysOrientationE::R180;
    case odb::dbOrientType::R270:
      return eUTL::PhysOrientationE::R270;
    case odb::dbOrientType::MX:
      return eUTL::PhysOrientationE::MX;
    case odb::dbOrientType::MXR90:
      return eUTL::PhysOrientationE::MX90;
    case odb::dbOrientType::MY:
      return eUTL::PhysOrientationE::MY;
    case odb::dbOrientType::MYR90:
      return eUTL::PhysOrientationE::MY90;
  }
  return eUTL::PhysOrientationE::R0;
}

eLIB::PhysMacroType toUdmMacroType(odb::dbMaster* master)
{
  using Type = eLIB::PhysMacroType::TypeE;
  if (master->isFiller()) {
    return eLIB::PhysMacroType(Type::CORE_FILLER);
  }
  if (master->isBlock()) {
    return eLIB::PhysMacroType(Type::BLOCK);
  }
  if (master->isEndCap()) {
    return eLIB::PhysMacroType(Type::ENDCAP);
  }
  return eLIB::PhysMacroType(Type::CORE);
}

bool buildUdmProjection(odb::dbDatabase* database,
                        std::vector<std::string>& fillerNames,
                        std::ostream& out)
{
  if (database == nullptr || database->getChip() == nullptr
      || database->getChip()->getBlock() == nullptr) {
    out << "ERROR: OpenROAD has no loaded ODB block\n";
    return false;
  }
  odb::dbBlock* block = database->getChip()->getBlock();
  if (block->getRows().empty()) {
    out << "ERROR: the loaded ODB block has no placement rows\n";
    return false;
  }

  auto projection = std::make_unique<fake_udm::DesignDb>();
  std::unordered_map<odb::dbTechLayer*, int> layerIds;
  int nextLayerId = 0;
  if (odb::dbTech* tech = database->getTech()) {
    for (odb::dbTechLayer* layer : tech->getLayers()) {
      const bool implant = layer->getType() == odb::dbTechLayerType::IMPLANT;
      eLIB::TechLayer& projected = projection->tech().addLayer(
          layer->getName(), implant, nextLayerId, layer->getWidth(),
          layer->getSpacing());
      projected.is_routing_
          = layer->getType() == odb::dbTechLayerType::ROUTING;
      projected.is_overlap_
          = layer->getType() == odb::dbTechLayerType::OVERLAP;
      projected.routing_idx_ = layer->getRoutingLevel();
      layerIds[layer] = nextLayerId++;
    }
  }

  odb::dbRow* firstRow = nullptr;
  for (odb::dbRow* row : block->getRows()) {
    if (row->getSite() != nullptr
        && row->getSite()->getClass() != odb::dbSiteClass::PAD) {
      firstRow = row;
      break;
    }
  }
  if (firstRow == nullptr) {
    out << "ERROR: the loaded ODB block has no non-pad placement row\n";
    return false;
  }
  projection->coreSite.name_ = firstRow->getSite()->getName();
  projection->coreSite.width_ = eUTL::UvDist(firstRow->getSite()->getWidth());
  projection->coreSite.height_
      = eUTL::UvDist(firstRow->getSite()->getHeight());

  for (odb::dbRow* row : block->getRows()) {
    int originX = 0;
    int originY = 0;
    row->getOrigin(originX, originY);
    const odb::Rect bbox = row->getBBox();
    eUNL::PhysRow& projected = projection->desMgr().addRow(
        originX,
        originY,
        row->getSite()->getWidth(),
        row->getSite()->getHeight(),
        bbox.dx(),
        row->getSite()->getClass() == odb::dbSiteClass::PAD);
    projected.site_.name_ = row->getSite()->getName();
    projected.site_cnt_ = row->getSiteCount();
    projected.orient_ = toUdmOrientation(row->getOrient());
  }

  std::unordered_map<odb::dbMaster*, const eLIB::PhysLibCell*> masters;
  int nextMasterId = 0;
  for (odb::dbLib* library : database->getLibs()) {
    for (odb::dbMaster* master : library->getMasters()) {
      eLIB::PhysLibCell& projected = projection->addMaster(
          master->getName(), nextMasterId++, master->getWidth(),
          master->getHeight(), master->isFiller());
      projected.type_ = toUdmMacroType(master);
      if (master->getSite() != nullptr) {
        projected.site_ = &projection->coreSite;
      }
      for (odb::dbBox* obstruction : master->getObstructions()) {
        const auto layer = layerIds.find(obstruction->getTechLayer());
        if (layer == layerIds.end()) {
          continue;
        }
        if (projected.obs_.empty()) {
          projected.obs_.emplace_back();
        }
        projected.obs_.front().shapes_[eLIB::TechLayerRelativeID(layer->second)]
            .emplace_back(
                eLIB::TechShape::RECT,
                eUTL::Rect(eUTL::UvDist(obstruction->xMin()),
                           eUTL::UvDist(obstruction->yMin()),
                           eUTL::UvDist(obstruction->xMax()),
                           eUTL::UvDist(obstruction->yMax())));
      }
      masters[master] = &projected;
      if (master->isFiller()
          && std::find(fillerNames.begin(), fillerNames.end(), master->getName())
                 == fillerNames.end()) {
        fillerNames.push_back(master->getName());
      }
    }
  }

  int nextCellId = 0;
  for (odb::dbInst* instance : block->getInsts()) {
    if (!instance->isPlaced()) {
      continue;
    }
    const auto master = masters.find(instance->getMaster());
    if (master == masters.end()) {
      continue;
    }
    int x = 0;
    int y = 0;
    instance->getLocation(x, y);
    projection->desMgr().addCell(
        eUNL::LeafCellID(0, nextCellId++),
        master->second,
        x,
        y,
        toUdmOrientation(instance->getOrient()),
        instance->isFixed() ? eUNL::PhysObjStatus::LOC_FIXED
                            : eUNL::PhysObjStatus::PLACED,
        instance->getName());
  }

  for (odb::dbBlockage* blockage : block->getBlockages()) {
    odb::dbBox* box = blockage->getBBox();
    if (box != nullptr) {
      projection->desMgr().addBlockage(box->xMin(),
                                       box->yMin(),
                                       box->xMax(),
                                       box->yMax(),
                                       blockage->isSoft());
    }
  }

  projection->activate();
  openroad_design = std::move(projection);
  out << "dpl2 import: " << nextCellId << " placed instance(s), "
      << masters.size() << " master(s), " << block->getRows().size()
      << " row(s), " << fillerNames.size() << " filler master(s)\n";
  return true;
}

}  // namespace

namespace dpl2 {

// The delivered DePlace snapshot intentionally did not include its database
// import implementation. This local definition builds the real Grid/Network
// over the OpenROAD projection above; it is not copied with fillerRepair.
void DePlace::importDb()
{
  importClear();
  core_ = getCoreArea();
  grid_->setCore(core_);
  grid_->examineRows(desMgr_);
  network_->setCore(core_);
  initEdgeTypeTable();
  createNetwork();
  data_loaded_ = true;
}

void DePlace::importClear()
{
  network_ = std::make_unique<Network>();
  network_->setFillerSetting(filler_setting_.get());
  edge_type_table_ = std::make_unique<EdgeTypeTable>();
}

void DePlace::initEdgeTypeTable()
{
  static constexpr std::array<const char*, 1> names{"DEFAULT"};
  edge_type_table_->addNames(names.begin(), names.end());
}

void DePlace::createNetwork()
{
  if (desMgr_ == nullptr) {
    return;
  }
  for (const auto& [id, cell] : desMgr_->cells_) {
    (void) id;
    if (cell.master != nullptr) {
      network_->addMaster(
          *cell.master, *filler_setting_, grid_.get(), edge_type_table_.get());
    }
  }
  for (const auto& [id, cell] : desMgr_->cells_) {
    (void) cell;
    network_->addNode(id, desMgr_);
  }
}

void DePlace::initPlacementDRC()
{
  drc_engine_ = std::make_unique<PlacementDRC>(grid_.get());
}

namespace local {

bool importOpenRoadDb(odb::dbDatabase* database,
                      const std::vector<std::string>& requestedFillers,
                      std::ostream& out)
{
  if (deplace_constructed) {
    out << "ERROR: dpl2_import_db may be called once per OpenROAD process\n";
    return false;
  }
  std::vector<std::string> fillerNames = requestedFillers;
  if (!buildUdmProjection(database, fillerNames, out)) {
    return false;
  }

  DePlace* deplace = DePlace::get();
  deplace_constructed = true;
  fillerSetting* setting = deplace->getFillerSetting();
  try {
    for (const std::string& name : fillerNames) {
      setting->addFillerCell(name);
    }
  } catch (const std::exception& error) {
    out << "ERROR: filler configuration failed: " << error.what() << '\n';
    return false;
  }
  out << "dpl2 grid: " << deplace->getGrid()->getRowCount().v << " x "
      << deplace->getGrid()->getRowSiteCount().v
      << ", fullUtil=" << deplace->getGrid()->isFullUtil() << '\n';
  return true;
}

}  // namespace local
}  // namespace dpl2
