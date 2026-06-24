// OpenROAD (odb/dpl) adapter for the shared FillerRepair algorithm.
// Compiled only inside the OpenROAD build (depends on odb / utl).
#include "DplFillerGrid.h"

#include <algorithm>

#include "utl/Logger.h"

namespace dpl_fr {

using odb::dbBox;
using odb::dbInst;
using odb::dbMaster;
using odb::dbRow;
using odb::dbTechLayer;
using odb::Rect;

// Mirror of dpl FillerPlacement::getImplant: the IMPLANT-type obstruction layer
// name is the VT/implant identity.
Vt DplFillerGrid::implantVt(dbMaster* master)
{
  if (master == nullptr) {
    return VT_NONE;
  }
  for (dbBox* obs : master->getObstructions()) {
    dbTechLayer* layer = obs->getTechLayer();
    if (layer != nullptr && layer->getType() == odb::dbTechLayerType::IMPLANT) {
      return layer->getName();
    }
  }
  return VT_NONE;
}

DplFillerGrid::DplFillerGrid(odb::dbBlock* block,
                             const std::set<dbInst*>& dirty,
                             const std::vector<dbMaster*>& filler_masters,
                             utl::Logger* logger)
    : block_(block), logger_(logger), filler_masters_(filler_masters)
{
  // --- geometry from the rows ---
  Rect core = block_->getCoreArea();
  core_x_ = core.xMin();
  core_y_ = core.yMin();
  int x_max = core.xMax();
  int y_max = core.yMax();
  for (dbRow* row : block_->getRows()) {
    odb::dbSite* site = row->getSite();
    if (site == nullptr) {
      continue;
    }
    site_w_ = static_cast<int>(site->getWidth());
    row_h_ = static_cast<int>(site->getHeight());
    break;
  }
  if (site_w_ <= 0 || row_h_ <= 0) {
    logger_->error(utl::DPL, 204, "DplFillerGrid: bad site/row size.");
  }
  n_rows_ = (y_max - core_y_) / row_h_;
  n_cols_ = (x_max - core_x_) / site_w_;
  grid_.assign(n_rows_, std::vector<Cell>(n_cols_));

  // --- (vt,w,h) -> master map for placement ---
  for (dbMaster* m : filler_masters_) {
    const Vt vt = implantVt(m);
    const int w = static_cast<int>(m->getWidth()) / site_w_;
    const int h = static_cast<int>(m->getHeight()) / row_h_;
    master_map_.emplace(std::make_tuple(vt, w, h), m);
  }

  // --- paint placed instances onto the grid ---
  for (dbInst* inst : block_->getInsts()) {
    if (!inst->isPlaced() && !inst->isFixed()) {
      continue;
    }
    dbMaster* m = inst->getMaster();
    Rect bbox = inst->getBBox()->getBox();
    const int c0 = colOf(bbox.xMin());
    const int c1 = colOf(bbox.xMax());
    const int r0 = rowOf(bbox.yMin());
    const int r1 = rowOf(bbox.yMax());

    SiteKind kind;
    Vt vt = VT_NONE;
    const bool is_filler = (m->getType() == odb::dbMasterType::CORE_SPACER);
    if (m->isBlock()) {
      kind = SiteKind::Blocked;
    } else if (is_filler) {
      kind = dirty.count(inst) ? SiteKind::DirtyFiller : SiteKind::CleanFiller;
      vt = implantVt(m);
    } else {
      kind = SiteKind::Cell;
      vt = implantVt(m);
    }
    for (int r = r0; r < r1 && r < n_rows_; ++r) {
      for (int c = c0; c < c1 && c < n_cols_; ++c) {
        if (r >= 0 && c >= 0) {
          grid_[r][c] = Cell{kind, vt, inst};
        }
      }
    }
  }
}

int DplFillerGrid::rowOf(int y) const
{
  return (y - core_y_) / row_h_;
}
int DplFillerGrid::colOf(int x) const
{
  return (x - core_x_) / site_w_;
}
int DplFillerGrid::xOfCol(int col) const
{
  return core_x_ + col * site_w_;
}
int DplFillerGrid::yOfRow(int row) const
{
  return core_y_ + row * row_h_;
}

int DplFillerGrid::numRows() const
{
  return n_rows_;
}
int DplFillerGrid::numCols(int /*row*/) const
{
  return n_cols_;
}
SiteKind DplFillerGrid::kindAt(int row, int col) const
{
  return grid_[row][col].kind;
}
Vt DplFillerGrid::vtAt(int row, int col) const
{
  return grid_[row][col].vt;
}

void DplFillerGrid::clearSite(int row, int col)
{
  dbInst* inst = grid_[row][col].inst;
  if (inst != nullptr && cleared_.insert(inst).second) {
    odb::dbInst::destroy(inst);
  }
  grid_[row][col] = Cell{SiteKind::Empty, VT_NONE, nullptr};
}

void DplFillerGrid::placeFiller(const PlacedFiller& f)
{
  dbMaster* master = masterFor(f.vt, f.width, f.height);
  if (master == nullptr) {
    logger_->error(utl::DPL, 205,
                   "DplFillerGrid: no master for vt={} {}x{}.",
                   f.vt,
                   f.width,
                   f.height);
    return;
  }
  const std::string name = "FILLER_REPAIR_" + std::to_string(f.row) + "_"
                           + std::to_string(f.col) + "_"
                           + std::to_string(placed_seq_++);
  dbInst* inst = odb::dbInst::create(block_,
                                     master,
                                     name.c_str(),
                                     /*physical_only=*/true);
  // Orientation: match the row the filler sits in (rails/implant alignment).
  odb::dbOrientType orient = odb::dbOrientType::R0;
  for (dbRow* row : block_->getRows()) {
    const odb::Point o = row->getOrigin();
    if (rowOf(o.y()) == f.row) {
      orient = row->getOrient();
      break;
    }
  }
  inst->setOrient(orient);
  inst->setLocation(xOfCol(f.col), yOfRow(f.row));
  inst->setPlacementStatus(odb::dbPlacementStatus::PLACED);
  inst->setSourceType(odb::dbSourceType::DIST);

  for (int r = f.row; r < f.row + f.height && r < n_rows_; ++r) {
    for (int c = f.col; c < f.col + f.width && c < n_cols_; ++c) {
      grid_[r][c] = Cell{SiteKind::CleanFiller, f.vt, inst};
    }
  }
}

dbMaster* DplFillerGrid::masterFor(const Vt& vt, int width, int height) const
{
  auto it = master_map_.find(std::make_tuple(vt, width, height));
  return it == master_map_.end() ? nullptr : it->second;
}

std::vector<Filler> DplFillerGrid::buildLibrary() const
{
  std::vector<Filler> lib;
  for (dbMaster* m : filler_masters_) {  // user order preserved
    lib.push_back(Filler{static_cast<int>(m->getWidth()) / site_w_,
                         static_cast<int>(m->getHeight()) / row_h_,
                         implantVt(m),
                         m->getName()});
  }
  return lib;
}

RepairResult repairDirtyFillers(odb::dbBlock* block,
                                const std::set<dbInst*>& dirty,
                                const std::vector<dbMaster*>& masters,
                                bool preserve_user_order,
                                int min_implant_width,
                                utl::Logger* logger)
{
  DplFillerGrid grid(block, dirty, masters, logger);
  Rules rules;
  rules.min_implant_width = min_implant_width;
  FillerRepair repair(grid.buildLibrary(), preserve_user_order, rules);
  RepairResult result = repair.repair(grid);
  logger->info(utl::DPL, 206,
               "Filler repair: placed {}, unsolved {}.",
               result.placed.size(),
               result.unsolved.size());
  return result;
}

}  // namespace dpl_fr
