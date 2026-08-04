// 1-808
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors

#include <algorithm>
#include <cmath>
#include <Grid.h>

// [fillerRepair-fix] tbb::task_arena is used below (iterateAllPhysNets) but
// no header here declared it; builds only worked where another header leaked
// it transitively.
#include <tbb/task_arena.h>

#include <phys/physNet.hh>

// NOLINT(misc-include-cleaner) Boost polygon headers require a specific
// include order.
#include <boost/polygon/polygon.hpp>
#include <boost/polygon/polygon_90_set_data.hpp>
#include <boost/polygon/rectangle_concept.hpp>
#include <boost/polygon/rectangle_data.hpp>

namespace dpl2 {

PixelPt::PixelPt(Pixel* pixel1, GridX grid_x, GridY grid_y)
    : pixel(pixel1), x(grid_x), y(grid_y)
{
}

/////////////////////////////////////////////////////////////////////
void Grid::clear()
{
  pixels_.clear();
  row_y_dbu_to_index_.clear();
  row_index_to_y_dbu_.clear();
}

void Grid::allocateGrid()
{
  // Make pixel grid
  if (pixels_.empty()) {
    resize(row_count_);
    for (GridY y{0}; y < row_count_; y++) {
      resize(y, row_site_count_);
    }
  }

  for (GridY y{0}; y < row_count_; y++) {
    for (GridX x{0}; x < row_site_count_; x++) {
      Pixel& pixel = pixels_[y.v][x.v];
      pixel.cell = nullptr;
      pixel.group = nullptr;
      pixel.util = 0.0;
      pixel.is_valid = false;
      pixel.is_hopeless = false;
      pixel.blocked_layers = 0;
    }
  }

  row_sites_.clear();
  row_sites_.resize(row_count_.v);
}

void Grid::examineRows(PhysDesMgr* desMgr)
{
  desMgr_ = desMgr;
  pixels_.clear();
  row_y_dbu_to_index_.clear();
  row_index_to_y_dbu_.clear();
  row_index_to_pixel_height_.clear();
  row_sites_.clear();
  uniform_row_height_.reset();
  site_width_ = DbuX{0};
  row_count_ = GridY{0};
  row_site_count_ = GridX{0};
  if (desMgr_ == nullptr) {
    return;
  }

  const TechSite* first_site = nullptr;

  visitDbRows(desMgr, [&](const PhysRow& row) {
    const TechSite& site = row.getSite();

    const int y_lo = (row.getOrigin().getY() - core_.getYL()).getStorage();
    const DbuY dbu_y{y_lo};
    row_y_dbu_to_index_[dbu_y] = GridY{0};
    row_y_dbu_to_index_[dbu_y + site.getHeight().getStorage()] = GridY{0};

    // Check all sites have equal width
    if (!first_site) {
      first_site = &site;
      site_width_ = DbuX{site.getWidth().getStorage()};
    } else if (site.getWidth().getStorage() != site_width_) {
      // logger_->error(DPL,
      //                51,
      //                "Site widths are not equal: {}={} != {}={}",
      //                first_site->getName(),
      //                site_width_,
      //                site->getName(),
      //                site->getWidth());
    }
  });

  GridY index{0};
  DbuY prev_y{0};
  for (auto& [dbu_y, grid_y] : row_y_dbu_to_index_) {
    grid_y = index++;
    row_index_to_y_dbu_.push_back(dbu_y);
    row_index_to_pixel_height_.push_back(dbu_y - prev_y);
    prev_y = dbu_y;
  }
  uniform_row_height_.reset();
  bool is_uniform = true;
  visitDbRows(desMgr, [&](const PhysRow& db_row) {
    if (!is_uniform) {
      return;
    }
    const int site_height = db_row.getSite().getHeight().getStorage();
    if (uniform_row_height_.has_value()) {
      // check if the bigger of both; the new and old heights, is a multiple of
      // the smaller
      const auto smaller = std::min(site_height, uniform_row_height_.value().v);
      const auto larger = std::max(site_height, uniform_row_height_.value().v);
      if (larger % smaller != 0) {
        // not uniform
        uniform_row_height_.reset();
        is_uniform = false;
      } else {
        // uniform
        uniform_row_height_ = DbuY{smaller};
      }
    } else {
      uniform_row_height_ = DbuY{site_height};
    }
  });
  if (site_width_.v > 0) {
    row_site_count_
        = GridX{divFloor(getCore().dx().getStorage(), getSiteWidth().v)};
  }
  if (!row_y_dbu_to_index_.empty()) {
    row_count_
        = GridY{static_cast<int>(row_y_dbu_to_index_.size() - 1)};
  }
}

std::unordered_set<int> Grid::getRowCoordinates() const
{
  std::unordered_set<int> coords;
  visitDbRows(desMgr_, [&](const PhysRow& row) {
    coords.insert((row.getOrigin().getY() - core_.getYL()).getStorage());
  });
  return coords;
}

void Grid::markHopeless(PhysDesMgr* desMgr,
                        const int max_displacement_x,
                        const int max_displacement_y)
{
  if (desMgr == nullptr || site_width_.v <= 0 || row_count_ <= 0
      || row_site_count_ <= 0) {
    return;
  }

  namespace gtl = boost::polygon;
  using gtl::operators::operator+=;
  using gtl::operators::operator-=;

  gtl::polygon_90_set_data<int> hopeless;
  hopeless += gtl::rectangle_data<int>{0, 0, row_site_count_.v, row_count_.v};
  const Rect core = getCore();

  // Fragmented row support; mark valid sites.
  visitDbRows(desMgr, [&](const PhysRow& db_row) {
    const Point2D orig = db_row.getOrigin();
    const GridX x_start{(orig.getX() - core.getXL()).getStorage() / site_width_.v};
    const GridX x_end{x_start + db_row.getSiteCnt()};
    const GridY y_row{gridSnapDownY(DbuY{(orig.getY()
        - core_.getYL()).getStorage()})};
    if (y_row.v < 0 || static_cast<size_t>(y_row.v) >= row_sites_.size()) {
      return;
    }
    for (GridX x{x_start}; x < x_end; x++) {
      Pixel* pixel = gridPixel(x, y_row);
      if (pixel != nullptr) {
        pixel->is_valid = true;
      }
    }
    row_sites_[y_row.v].add(
        {{x_start.v, x_end.v},
         {{(db_row.getSite().getName()), db_row.getOrient()}}});
    // The safety margin is to avoid having only a very few sites
    // within the diamond search that may still lead to failures.
    const int safety = 20;
    const GridX xl = std::max(GridX{0}, x_start - max_displacement_x + safety);
    const GridX xh = std::min(row_site_count_, x_end + max_displacement_x - safety);

    const GridY yl = std::max(GridY{0}, y_row - max_displacement_y + safety);
    const GridY yh = std::min(row_count_, y_row + max_displacement_y - safety);
    hopeless -= gtl::rectangle_data<int>{xl.v, yl.v, xh.v, yh.v};
  });

  std::vector<gtl::rectangle_data<int>> rects;
  hopeless.get_rectangles(rects);
  for (const auto& rect : rects) {
    for (int y = gtl::yl(rect); y < gtl::yh(rect); y++) {
      for (int x = gtl::xl(rect); x < gtl::xh(rect); x++) {
        pixels_[y][x].is_hopeless = true;
      }
    }
  }
}

void Grid::markBlocked(PhysDesMgr* desMgr)
{
  if (desMgr == nullptr) {
    return;
  }
  const Rect core = getCore();
  std::function<void(const Rect&, TechLayer)> addBlockedLayers
      = [&](Rect wire_rect, TechLayer tech_layer) {
        if (!tech_layer.isRouting()) {
          return;
        }
        auto routing_level = tech_layer.getRoutingIdx().getNumValue();
        if (routing_level <= 1 || routing_level > 3) {  // considering M2, M3
          return;
        }
        if (wire_rect.dx() >= wire_rect.dy()) {
          return;
        }
        wire_rect.move(-core.getXL(), -core.getYL());
        GridRect grid_rect = gridCovering(wire_rect);
        GridRect core{.xlo = GridX{0},
                      .ylo = GridY{0},
                      .xhi = GridX{row_site_count_},
                      .yhi = GridY{row_count_}};
        grid_rect = grid_rect.intersect(core);
        for (GridY y = grid_rect.ylo; y < grid_rect.yhi; y++) {
          for (GridX x = grid_rect.xlo; x < grid_rect.xhi; x++) {
            auto pixel1 = gridPixel(x, y);
            if (pixel1) {
              pixel1->blocked_layers |= 1 << routing_level;
            }
          }
        }
      };

struct ShieldWireVisitor :
  public eUNL::UnlBaseVisitor<eUNL::PhysNet, eUNL::PhysNetID> {
  const std::function<void(const Rect&, TechLayer)>& addBlockedLayers;
  const PhysDesMgr* desMgr;

  ShieldWireVisitor(const std::function<void(const Rect&, TechLayer)>& a,
    const PhysDesMgr* d)
    : addBlockedLayers(a), desMgr(d) {}

  bool filter(const eUNL::PhysNet& net, const eUNL::PhysNetID& netId) override {
    (void) netId;
    return net.hasSWire();
  }

  eUNL::UnlIterStatus visit(const eUNL::PhysNet& net,
    const eUNL::PhysNetID& netId) override {
    (void) netId;
    const eUNL::PhysSWire& swire = net.getSWire();
    for (const eUNL::PhysShape& sbox : swire.getShapes()) {
      if (sbox.isVia() || sbox.getUsage() == eUNL::ShapeUsageE::DRCFILL) {
        continue;
      }
      const Rect& wire_rect = sbox.getRect();
      TechLayerID tech_layer_id = sbox.getLayer();
      TechLayer tech_layer =
        desMgr->getTopTech().getTechLayer(tech_layer_id.getLocalId());
      addBlockedLayers(wire_rect, tech_layer);
    }
    return eUNL::UnlIterStatus::CONTINUE;
  }
};

tbb::task_arena arena;
ShieldWireVisitor swVisitor{addBlockedLayers, desMgr};
desMgr->iterateAllPhysNets(arena, swVisitor, /*inclFlat=*/true, /*inclPg=*/false);
for (const PhysBlockage& blockage : desMgr->getPhysBlockageIter()) {
  if (blockage.isSoft()) {
    continue;
  }
  for (const eLIB::TechShape& bbox : blockage.getShapes()) {
    Rect box = bbox.getBbox(true);
    box.move(-core.getXL(), -core.getYL());
    GridRect grid_rect = gridCovering(box);
      // Clip to the core area
    GridRect core{.xlo = GridX{0},
                  .ylo = GridY{0},
                  .xhi = GridX{row_site_count_},
                  .yhi = GridY{row_count_}};
    grid_rect = grid_rect.intersect(core);
    for (GridY y = grid_rect.ylo; y < grid_rect.yhi; y++) {
      for (GridX x = grid_rect.xlo; x < grid_rect.xhi; x++) {
        Pixel& pixel1 = pixel(y, x);
        pixel1.is_valid = false;
      }
    }
  }
}
}

void Grid::initGrid(PhysDesMgr* desMgr,
                    std::shared_ptr<Padding> padding,
                    int max_displacement_x,
                    int max_displacement_y)
{
  if (desMgr == nullptr || desMgr_ != desMgr || padding == nullptr) {
    return;
  }
  padding_ = std::move(padding);

  allocateGrid();

  markHopeless(desMgr, max_displacement_x, max_displacement_y);

  markBlocked(desMgr);
}
Pixel* Grid::gridPixel(GridX grid_x, GridY grid_y) const
{
  if (grid_x >= 0 && grid_x < row_site_count_ && grid_y >= 0
      && grid_y < row_count_
      && static_cast<size_t>(grid_y.v) < pixels_.size()
      && static_cast<size_t>(grid_x.v) < pixels_[grid_y.v].size()) {
    return const_cast<Pixel*>(&pixels_[grid_y.v][grid_x.v]);
  }
  return nullptr;
}

void Grid::visitDbRows(const PhysDesMgr* desMgr,
                    const std::function<void(const PhysRow&)>& func) const
{
  if (desMgr == nullptr || !func) {
    return;
  }
  for (const auto& row : desMgr->getPhysRowIter()) {
    if (row.getSite().getIsPad() == false) {
      func(row);
    }
  }
}

int Grid::countValidPixels(GridX x_begin,
                            GridY y_begin,
                            GridX x_end,
                            GridY y_end) const
{
  int count = 0;
  for (GridY y = y_begin; y < y_end; y++) {
    for (GridX x = x_begin; x < x_end; x++) {
      Pixel* pixel = gridPixel(x, y);
      if (pixel != nullptr && pixel->is_valid) {
        ++count;
      }
    }
  }
  return count;
}

void Grid::applyCellContribution(Node* node,
                                 GridX x_begin,
                                 GridY y_begin,
                                 GridX x_end,
                                 GridY y_end,
                                 float scale)
{
  if (node == nullptr) {
    return;
  }
  const int cell_pixel_count = countValidPixels(x_begin, y_begin, x_end, y_end);
  if (cell_pixel_count == 0) {
    return;
  }
  if (total_area_.empty()) {
    return;
  }

  const float cell_area
      = static_cast<float>(node->getWidth().v * node->getHeight().v);
  const float area_per_pixel
      = (cell_area / static_cast<float>(cell_pixel_count)) * scale;
  // const float pins_per_pixel
      // = (static_cast<float>(node->getNumPins()) / cell_pixel_count) * scale;

  const int grid_size = static_cast<int>(total_area_.size());
  for (GridY y = y_begin; y < y_end; y++) {
    for (GridX x = x_begin; x < x_end; x++) {
      Pixel* pixel = gridPixel(x, y);
      if (pixel == nullptr || !pixel->is_valid) {
        continue;
      }
      const int pixel_idx = (y.v * row_site_count_.v) + x.v;
      if (pixel_idx < 0 || pixel_idx >= grid_size) {
        continue;
      }
      total_area_[pixel_idx] += area_per_pixel;
      // total_pins_[pixel_idx] += pins_per_pixel;
      total_area_[pixel_idx] = std::max(total_area_[pixel_idx], 0.0f);
      // total_pins_[pixel_idx] = std::max(total_pins_[pixel_idx], 0.0f);
    }
  }
}

GridX Grid::gridX(DbuX x) const
{
  return getSiteWidth().v > 0 ? GridX{x.v / getSiteWidth().v} : GridX{0};
}

GridX Grid::gridEndX(DbuX x) const
{
  return getSiteWidth().v > 0 ? GridX{divCeil(x.v, getSiteWidth().v)}
                              : GridX{0};
}

GridX Grid::gridX(const Node* cell) const
{
  return cell != nullptr ? gridX(cell->getLeft()) : GridX{0};
}

GridX Grid::gridEndX(const Node* cell) const
{
  if (cell == nullptr || getSiteWidth().v <= 0) {
    return GridX{0};
  }
  return GridX{
      divCeil((cell->getLeft() + cell->getWidth()).v, getSiteWidth().v)};
}

GridX Grid::gridPaddedX(const Node* cell) const
{
  if (cell == nullptr) {
    return GridX{0};
  }
  const GridX pad = padding_ != nullptr ? padding_->padLeft(cell) : GridX{0};
  return gridX(cell->getLeft() - gridToDbu(pad, getSiteWidth()));
}

GridX Grid::gridPaddedEndX(const Node* cell) const
{
  if (cell == nullptr || getSiteWidth().v <= 0) {
    return GridX{0};
  }
  const DbuX site_width = getSiteWidth();
  const GridX pad = padding_ != nullptr ? padding_->padRight(cell) : GridX{0};
  const DbuX end_x = cell->getLeft() + cell->getWidth()
                     + gridToDbu(pad, site_width);
  return GridX{divCeil(end_x.v, site_width.v)};
}

GridY Grid::gridSnapDownY(DbuY y) const
{
  auto it = std::upper_bound(  // NOLINT(modernize-use-ranges)
      row_index_to_y_dbu_.begin(),
      row_index_to_y_dbu_.end(),
      y);
  if (it == row_index_to_y_dbu_.begin()) {
    return GridY{0};
  }
  --it;
  return GridY{static_cast<int>(it - row_index_to_y_dbu_.begin())};
}

GridY Grid::gridRoundY(DbuY y) const
{
  if (row_index_to_y_dbu_.empty()) {
    return GridY{0};
  }
  const auto grid_y = gridSnapDownY(y);
  if (grid_y < row_index_to_y_dbu_.size() - 1) {
    const auto grid_next = grid_y + 1;
    if (std::abs(row_index_to_y_dbu_[grid_y.v].v - y.v)
        >= std::abs(row_index_to_y_dbu_[grid_next.v].v - y.v)) {
      return grid_next;
    }
  }
  return grid_y;
}

GridY Grid::gridEndY(DbuY y) const
{
  auto it = std::lower_bound(  // NOLINT(modernize-use-ranges)
      row_index_to_y_dbu_.begin(),
      row_index_to_y_dbu_.end(),
      y);
  if (it == row_index_to_y_dbu_.end()) {
    return GridY{static_cast<int>(row_index_to_y_dbu_.size())};
  }
  return GridY{static_cast<int>(it - row_index_to_y_dbu_.begin())};
}

GridY Grid::gridSnapDownY(const Node* cell) const
{
  return cell != nullptr ? gridSnapDownY(cell->getBottom()) : GridY{0};
}

// [fillerRepair-fix] see Grid.h: (column, row) of a placed node.
std::pair<GridX, GridY> Grid::gridXY(const Node* cell) const
{
  return cell != nullptr
             ? std::make_pair(gridX(cell), gridSnapDownY(cell))
             : std::make_pair(GridX{0}, GridY{0});
}

// See Grid.h. Walks pixels outward and counts DISTINCT placed cells; a cell
// spans several pixels, so the count advances when the occupant changes.
Rect Grid::getBoundingBox(const Rect& region, int rings) const
{
  const int lastRow = row_count_.v - 1;
  const int lastCol = row_site_count_.v - 1;
  if (lastRow < 0 || lastCol < 0 || site_width_.v <= 0) {
    return region;
  }
  const int steps = std::max(rings, 0);

  const int64_t inXl = region.getXL().getStorage();
  const int64_t inYl = region.getYL().getStorage();
  const int64_t inXh
      = std::max<int64_t>(region.getXH().getStorage(), inXl + 1);
  const int64_t inYh
      = std::max<int64_t>(region.getYH().getStorage(), inYl + 1);

  const int rowLo = std::clamp(
      gridSnapDownY(DbuY{static_cast<int>(inYl)}).v - steps, 0, lastRow);
  const int rowHi = std::clamp(
      gridSnapDownY(DbuY{static_cast<int>(inYh - 1)}).v + steps, 0, lastRow);
  const int colLo = std::clamp(gridX(DbuX{static_cast<int>(inXl)}).v,
                               0, lastCol);
  const int colHi = std::clamp(gridX(DbuX{static_cast<int>(inXh - 1)}).v,
                               0, lastCol);

  int64_t outXl = inXl;
  int64_t outXh = inXh;
  const auto absorb = [&](const Node* cell) {
    outXl = std::min<int64_t>(outXl, cell->getLeft().v);
    outXh = std::max<int64_t>(outXh, cell->getLeft().v + cell->getWidth().v);
  };

  for (int row = rowLo; row <= rowHi; ++row) {
    // Cells overlapping the region are snapped in whole.
    for (int col = colLo; col <= colHi; ++col) {
      const Pixel* pixel = gridPixel(GridX{col}, GridY{row});
      if (pixel != nullptr && pixel->cell != nullptr) {
        absorb(pixel->cell);
      }
    }
    // Then `steps` further distinct cells on each side. Seed `previous` with
    // the occupant of the boundary column: a cell that already overlaps the
    // region was absorbed above and must not consume a ring when the walk
    // steps over the rest of its sites.
    const Pixel* loPixel = gridPixel(GridX{colLo}, GridY{row});
    const Node* previous = loPixel != nullptr ? loPixel->cell : nullptr;
    int counted = 0;
    for (int col = colLo - 1; col >= 0 && counted < steps; --col) {
      const Pixel* pixel = gridPixel(GridX{col}, GridY{row});
      const Node* cell = pixel != nullptr ? pixel->cell : nullptr;
      if (cell == nullptr || cell == previous) {
        continue;  // empty site or the same cell again: not a new ring
      }
      previous = cell;
      ++counted;
      absorb(cell);
    }
    const Pixel* hiPixel = gridPixel(GridX{colHi}, GridY{row});
    previous = hiPixel != nullptr ? hiPixel->cell : nullptr;
    counted = 0;
    for (int col = colHi + 1; col <= lastCol && counted < steps; ++col) {
      const Pixel* pixel = gridPixel(GridX{col}, GridY{row});
      const Node* cell = pixel != nullptr ? pixel->cell : nullptr;
      if (cell == nullptr || cell == previous) {
        continue;
      }
      previous = cell;
      ++counted;
      absorb(cell);
    }
  }

  const int64_t coreWidth
      = core_.getXH().getStorage() - core_.getXL().getStorage();
  outXl = std::clamp<int64_t>(outXl, 0, coreWidth);
  outXh = std::clamp<int64_t>(outXh, 0, coreWidth);
  return Rect(UvDist(outXl), UvDist(gridYToDbu(GridY{rowLo}).v),
              UvDist(outXh), UvDist(gridYToDbu(GridY{rowHi + 1}).v));
}

GridY Grid::gridRoundY(const Node* cell) const
{
  return cell != nullptr ? gridRoundY(cell->getBottom()) : GridY{0};
}

GridY Grid::gridEndY(const Node* cell) const
{
  return cell != nullptr
             ? gridEndY(cell->getBottom() + cell->getHeight())
             : GridY{0};
}

DbuY Grid::gridYToDbu(GridY y) const
{
  if (y.v < 0 || row_index_to_y_dbu_.empty()) {
    return DbuY{0};
  }
  if (y == row_index_to_y_dbu_.size()) {
    return DbuY{core_.getYH().getStorage() - core_.getYL().getStorage()};
  }
  if (static_cast<size_t>(y.v) > row_index_to_y_dbu_.size()) {
    return DbuY{0};
  }
  return row_index_to_y_dbu_.at(y.v);
}

GridX Grid::gridPaddedWidth(const Node* cell) const
{
  if (cell == nullptr || getSiteWidth().v <= 0) {
    return GridX{0};
  }
  const DbuX width = padding_ != nullptr ? padding_->paddedWidth(cell)
                                        : cell->getWidth();
  return GridX{divCeil(width.v, getSiteWidth().v)};
}

GridX Grid::gridWidth(const Node* cell) const
{
  return cell != nullptr && getSiteWidth().v > 0
             ? GridX{divCeil(cell->getWidth().v, getSiteWidth().v)}
             : GridX{0};
}

GridY Grid::gridHeight(const PhysLibCell& master) const
{
  if (uniform_row_height_) {
    DbuY row_height = uniform_row_height_.value();
    return GridY{std::max(1,
        divCeil(master.getHeight().getStorage(), row_height.v))};
  }

  if (!master.hasSitePattern()) {
    return GridY{1};
  }

  return GridY{static_cast<int>(master.getSitePatterns().size())};
}

GridY Grid::gridHeight(const Node* cell) const
{
  if (cell == nullptr) {
    return GridY{1};
  }
  if (uniform_row_height_) {
    DbuY row_height = uniform_row_height_.value();
    return GridY{std::max(1, divCeil(cell->getHeight().v, row_height.v))};
  }
  if (!cell->getDbInst().isValid()) {
    return GridY{1};
  }
  const Master* networkMaster = cell->getMaster();
  if (networkMaster == nullptr || networkMaster->getPhysLibCell() == nullptr) {
    return GridY{1};
  }
  const PhysLibCell* master = networkMaster->getPhysLibCell();
  if (!master->hasSitePattern()) {
    return GridY{1};
  }

  return GridY{static_cast<int>(master->getSitePatterns().size())};
}

DbuY Grid::rowHeight(GridY index)
{
  return row_index_to_pixel_height_.at(index.v);
}

bool Grid::isMultiHeight(const PhysLibCell& master) const
{
  if (uniform_row_height_) {
    return master.getHeight().getStorage() > uniform_row_height_.value();
  }
  return master.hasSitePattern();
}

// [fillerRepair-fix] True when every site the rows actually offer carries an
// occupant. Sites under a hard blockage are is_valid == false and are not
// counted; so are the pixels of a fragmented row outside its site span.
//
// Two corrections here beyond the caller-side one (see
// DePlace::paintGridCell, which was dropping fillers):
//
//  - iterate the LOGICAL grid, row_count_ x row_site_count_, not the pixel
//    vector's extent. allocateGrid() only resizes when pixels_ is empty and
//    resets exactly the logical range, so a re-init onto a smaller design
//    leaves stale rows behind that nothing else can reach -- gridPixel()
//    bounds-checks against the logical size, so no cell is ever painted
//    there. Scanning them made the answer depend on a previous design.
//  - an empty grid is not "full". It used to warn and then return true,
//    which is a false positive at exactly the moment the function knows it
//    has nothing to report on.
bool Grid::isFullUtil() const
{
  if (pixels_.empty() || row_count_ <= 0 || row_site_count_ <= 0) {
    std::cout << "WARN: grid pixel is empty." << std::endl;
    return false;
  }
  for (GridY y{0}; y < row_count_; y++) {
    for (GridX x{0}; x < row_site_count_; x++) {
      const Pixel* pixel = gridPixel(x, y);
      if (pixel != nullptr && pixel->is_valid && pixel->cell == nullptr) {
        std::cout << "Grid not fully utilized: first empty site at row "
                  << y.v << " site " << x.v << std::endl;
        return false;
      }
    }
  }
  return true;
}

void Grid::visitCellPixels(
    Node* cell,
    bool padded,
    const std::function<void(Pixel* pixel, bool padded)>& visitor) const
{
  if (cell == nullptr || desMgr_ == nullptr || !visitor) {
    return;
  }
  const Master* networkMaster = cell->getMaster();
  if (networkMaster == nullptr || networkMaster->getPhysLibCell() == nullptr) {
    return;
  }
  const PhysCell& inst = desMgr_->getPhysCell(cell->getDbInst());
  if (!inst.isValid()) {
    return;
  }
  auto obstructions = networkMaster->getPhysLibCell()->getObstruction();
  bool have_obstructions = false;
  const Rect core = getCore();

  for (const PhysLibObs& lib_obs : obstructions) {
    const auto& tech_obs = lib_obs.getShapes(inst.getOrient());
    for (const auto& obs : tech_obs) {
      if (desMgr_->getTopTech().getTechLayer(obs.first).isOverlap()) {
        have_obstructions = true;
        for (const TechShape& obstruction : obs.second) {
          Rect rect = obstruction.getRect();
          rect.move(-core.getXL(), -core.getYL());
          GridRect grid_rect = gridCovering(rect);
          for (GridX x = grid_rect.xlo; x < grid_rect.xhi; x++) {
            for (GridY y = grid_rect.ylo; y < grid_rect.yhi; y++) {
              Pixel* pixel = gridPixel(x, y);
              if (pixel) {
                visitor(pixel, false);
              }
            }
          }
        }
      }
    }
  }
  if (!have_obstructions) {
    const auto grid_box = gridCovering(cell);
    auto pad_left = padded && padding_ != nullptr ? padding_->padLeft(cell)
                                                  : GridX{0};
    auto pad_right = padded && padding_ != nullptr ? padding_->padRight(cell)
                                                   : GridX{0};

    auto x_start = grid_box.xlo - pad_left;
    auto x_end = grid_box.xhi + pad_right;
    for (GridX x{x_start}; x < x_end; x++) {
      for (GridY y{grid_box.ylo}; y < grid_box.yhi; y++) {
        Pixel* pixel = gridPixel(x, y);
        if (pixel == nullptr) {
          continue;
        }
        const bool within_cell = x >= grid_box.xlo && x < grid_box.xhi;
        visitor(pixel, !within_cell);
      }
    }
  }
}

void Grid::visitCellBoundaryPixels(
    Node& cell,
    const std::function<
        void(Pixel* pixel, int edgeDirection, GridX x, GridY y)>& visitor)
const
{
  if (desMgr_ == nullptr || !visitor || cell.getMaster() == nullptr
      || cell.getMaster()->getPhysLibCell() == nullptr) {
    return;
  }
  const PhysCell& inst = desMgr_->getPhysCell(cell.getDbInst());
  if (!inst.isValid()) {
    return;
  }

  auto visit = [&visitor, this](const GridX x_start,
                                const GridX x_end,
                                const GridY y_start,
                                const GridY y_end) {
    for (GridX x = x_start; x < x_end; x++) {
      Pixel* pixel = gridPixel(x, y_start);
      if (pixel) {
        visitor(pixel, 1, x, y_start);
      }
      pixel = gridPixel(x, y_end - 1);
      if (pixel) {
        visitor(pixel, 2, x, y_end - 1);
      }
    }
    for (GridY y = y_start; y < y_end; y++) {
      Pixel* pixel = gridPixel(x_start, y);
      if (pixel) {
        visitor(pixel, 3, x_start, y);
      }
      pixel = gridPixel(x_end - 1, y);
      if (pixel) {
        visitor(pixel, 4, x_end - 1, y);
      }
    }
  };

  auto obstructions = cell.getMaster()->getPhysLibCell()->getObstruction();
  bool have_obstructions = false;
  const Rect core = getCore();

  for (const PhysLibObs& lib_obs : obstructions) {
    const auto& layer_obs = lib_obs.getShapes(inst.getOrient());
    for (const auto& obs : layer_obs) {
      if (desMgr_->getTopTech().getTechLayer(obs.first).isOverlap()) {
        have_obstructions = true;
        for (const TechShape& obstruction : obs.second) {
          Rect rect = obstruction.getRect();
          rect.move(-core.getXL(), -core.getYL());
          GridRect grid_rect = gridCovering(rect);
          visit(grid_rect.xlo, grid_rect.xhi, grid_rect.ylo, grid_rect.yhi);
        }
      }
    }
  }
  if (!have_obstructions) {
    const auto grid_rect = gridCovering(&cell);
    // debugPrint(logger_,
    //           DPL,
    //           "hybrid",
    //           1,
    //           "Checking cell {} isHybrid {} in rows. Y start {} y end {}",
    //           cell.getDbInst().getName(),
    //           cell.isHybrid(),
    //           grid_rect.ylo,
    //           grid_rect.yhi);
    visit(grid_rect.xlo, grid_rect.xhi, grid_rect.ylo, grid_rect.yhi);
  }
}

void Grid::paintPixel(Node* cell)
{
  if (cell == nullptr) {
    return;
  }
  paintPixel(cell, gridX(cell), gridSnapDownY(cell));
}

void Grid::erasePixel(Node* cell)
{
  if (cell == nullptr) {
    return;
  }
  const auto grid_rect = gridCoveringPadded(cell);
  // debugPrint(logger_,
  //           DPL,
  //           "hybrid",
  //           1,
  //           "Checking cell {} isHybrid {}",
  //           cell->getDbInst().getName(),
  //           cell->isHybrid());
  // debugPrint(logger_,
  //           DPL,
  //           "hybrid",
  //           1,
  //           "Checking cell {} in rows. Y start {} y end {}",
  //           cell->getDbInst().getName(),
  //           grid_rect.ylo,
  //           grid_rect.yhi);

  // Clear cell occupancy and padding reservations for this cell
  for (GridX x = grid_rect.xlo; x < grid_rect.xhi; x++) {
    for (GridY y = grid_rect.ylo; y < grid_rect.yhi; y++) {
      Pixel* pixel = gridPixel(x, y);
      if (pixel == nullptr) {
        continue;
      }

      // Clear cell occupancy
      if (pixel->cell == cell) {
        pixel->cell = nullptr;
        pixel->util = 0;
      }

      // Clear padding reservations made by this cell
      if (pixel->padding_reserved_by == cell) {
        pixel->padding_reserved_by = nullptr;
      }
    }
  }
}

void Grid::paintPixel(Node* cell, GridX grid_x, GridY grid_y)
{
  if (cell == nullptr) {
    return;
  }
  // Paint the actual cell footprint (not including padding)
  GridX cell_x_end = grid_x + gridWidth(cell);
  GridY cell_y_end = gridEndY(gridYToDbu(grid_y) + cell->getHeight());
  // Mark actual cell pixels
  for (GridX x{grid_x}; x < cell_x_end; x++) {
    for (GridY y{grid_y}; y < cell_y_end; y++) {
      Pixel* pixel = gridPixel(x, y);
      if (pixel == nullptr) {
        continue;
      }
      pixel->cell = cell;
      pixel->util = 1.0;
    }
  }

  // Mark spacing reservations around the cell
  paintCellPadding(cell, grid_x, grid_y, cell_x_end, cell_y_end);
}
void Grid::paintCellPadding(Node* cell)
{
  if (cell == nullptr) {
    return;
  }
  auto grid_x_begin = gridX(cell);
  auto grid_y_begin = gridSnapDownY(cell);
  auto grid_x_end = grid_x_begin + gridWidth(cell);
  auto grid_y_end = gridEndY(gridYToDbu(grid_y_begin) + cell->getHeight());

  paintCellPadding(cell, grid_x_begin, grid_y_begin, grid_x_end, grid_y_end);
}
void Grid::paintCellPadding(Node* cell,
                            const GridX grid_x_begin,
                            const GridY grid_y_begin,
                            const GridX grid_x_end,
                            const GridY grid_y_end)
{
  if (cell == nullptr || padding_ == nullptr) {
    return;
  }
  GridX left_pad = padding_->padLeft(cell);
  GridX right_pad = padding_->padRight(cell);

  // Reserve left spacing pixels
  for (GridX x{grid_x_begin - left_pad}; x < grid_x_begin; x++) {
    for (GridY y{grid_y_begin}; y < grid_y_end; y++) {
      Pixel* pixel = gridPixel(x, y);
      if (pixel == nullptr) {
        continue;
      }
      pixel->padding_reserved_by = cell;
    }
  }

  // Reserve right spacing pixels
  for (GridX x{grid_x_end}; x < grid_x_end + right_pad; x++) {
    for (GridY y{grid_y_begin}; y < grid_y_end; y++) {
      Pixel* pixel = gridPixel(x, y);
      if (pixel == nullptr) {
        continue;
      }
      pixel->padding_reserved_by = cell;
    }
  }
}

GridRect Grid::gridCovering(const Rect& rect) const
{
  return {.xlo = gridX(DbuX{rect.getXL().getStorage()}),
          .ylo = gridSnapDownY(DbuY{rect.getYL().getStorage()}),
          .xhi = gridEndX(DbuX{rect.getXH().getStorage()}),
          .yhi = gridEndY(DbuY{rect.getYH().getStorage()})};
}

GridRect Grid::gridCovering(const Node* cell) const
{
  if (cell == nullptr) {
    return GridRect{GridX{0}, GridY{0}, GridX{0}, GridY{0}};
  }
  return {.xlo = gridX(cell),
          .ylo = gridSnapDownY(cell),
          .xhi = gridEndX(cell),
          .yhi = gridEndY(cell)};
}

GridRect Grid::gridCoveringPadded(const Node* cell) const
{
  if (cell == nullptr) {
    return GridRect{GridX{0}, GridY{0}, GridX{0}, GridY{0}};
  }
  return {.xlo = gridPaddedX(cell),
          .ylo = gridSnapDownY(cell),
          .xhi = gridPaddedEndX(cell),
          .yhi = gridEndY(cell)};
}

GridRect Grid::gridWithin(const DbuRect& rect) const
{
  return {.xlo = dbuToGridCeil(rect.xl, getSiteWidth()),
          .ylo = gridEndY(rect.yl),
          .xhi = dbuToGridFloor(rect.xh, getSiteWidth()),
          .yhi = gridSnapDownY(rect.yh)};
}

std::optional<PhysOrientation>
Grid::getSiteOrientation(GridX x, GridY y, std::string site_name) const
{
  if (y.v < 0 || static_cast<size_t>(y.v) >= row_sites_.size()) {
    return {};
  }
  const RowSitesMap& sites_map = row_sites_[y.v];
  auto interval_it = sites_map.find(x.v);
  if (interval_it == sites_map.end()) {
    return {};
  }
  const SiteToOrientation& sites_orient = interval_it->second;

  if (auto it = sites_orient.find(site_name); it != sites_orient.end()) {
    return it->second;
  }
  return {};
}

} // namespace dpl2
