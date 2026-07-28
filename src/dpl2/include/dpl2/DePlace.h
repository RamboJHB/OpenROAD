// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors
#pragma once

#include <boost/geometry/core/cs.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

// UDM
#include <phys/fpManager.hh>
#include <phys/physDesMgr.hh>
#include <phys/physHier.hh>
#include <physLibMgr.hh>
#include <techRuleCheck.hh>
#include <techObjTypes.hh>
#include <util/iter.hh>
#include <fnlObjTypes.hh>
#include <libObjAccessor.hh>
#include <timlib/libCell.hh>
#include <unl/unlObjTypes.hh>

using eUNL::PhysBlockage;
using eUNL::PhysRow;
using eUNL::PhysCell;
using eUNL::LeafCellID;
using eUNL::LibCellID;
using eUNL::PhysPin;
using eUNL::PhysSWire;
using eUNL::PhysShape;
using eUNL::PhysObjStatus;
using eUNL::PhysDesMgr;
using eUTL::uvRIter;
using eLIB::ShapeUsageE;
using eLIB::TechLayerID;
using eLIB::TechLayer;
using eLIB::TechShape;
using eLIB::PhysLib;
using eLIB::PhysLibObs;
using eLIB::PhysLibCell;
using eLIB::TechSite;
using eUTL::UvDist;
using eUTL::Rect;
using eUTL::Point2D;
using eUTL::PhysOrientationE;
using eUTL::PhysOrientation;

// Forward declarations
namespace dpl2{
  class TestDePlaceCmd;
  class TestObjectsCmd;
  class TestPlacementDRCCmd;
  class TestImplantCmd;
  class TestEcoFlowCmd;
}

namespace dpl2 {
class Grid;
class Node;
class Pixel;
class PixelPt;
class GridPt;
class GridRect;
class DbuPt;
class DbuRect;
class Padding;
class EdgeTypeTable;
class Network;
class TestDePlaceCmd;
class PlacementDRC;
class Architecture;
class fillerSetting;
enum class OpType : uint8_t;
struct FillerCellRecord;

template <typename T>
struct TypedCoordinate;
// These have to be defined here even though they are only used
// in the implementation section.  C++ doesn't allow you to forward
// declare types of this sort.
struct GridXType;
using GridX = TypedCoordinate<GridXType>;

struct GridYType;
using GridY = TypedCoordinate<GridYType>;

struct DbuXType;
using DbuX = TypedCoordinate<DbuXType>;

struct DbuYType;
using DbuY = TypedCoordinate<DbuYType>;

inline constexpr unsigned Symmetry_UNKNOWN = 0x00000000;
inline constexpr unsigned Symmetry_X = 0x00000001;
inline constexpr unsigned Symmetry_Y = 0x00000002;
inline constexpr unsigned Symmetry_ROT90 = 0x00000004;

class DePlace {
public:

static DePlace* get() {
  static std::unique_ptr<DePlace> de_place_ = std::make_unique<DePlace>();
  return de_place_.get();
}

DePlace();
DePlace(PhysDesMgr* desMgr);
~DePlace();

DePlace(const DePlace&) = delete;
DePlace& operator=(const DePlace&) = delete;
void setFixedGridCells();
void setPlacedGridCells();
void setGridCell(Node* cell, Pixel* pixel);
void setPaddingGlobal(int left, int right);
void setPadding(PhysLibCell* master, int left, int right);
void setPadding(LeafCellID cellId, int left, int right);
std::pair<int, int> findLeg(LeafCellID cellId, int diameter,
    std::string moduleName);
std::pair<int, int> findLeg(LeafCellID cellId, std::string moduleName);
bool isLegal(LeafCellID cellId, LibCellID lcId,
    std::vector<FillerCellRecord>& fcRecord);
PhysDesMgr* getDesMgr() {return desMgr_;};
Grid* getGrid() {return grid_.get();};
const Grid* getGrid() const {return grid_.get();};
Network* getNetwork() {return network_.get();};
const Network* getNetwork() const {return network_.get();};
Rect getCoreArea();

/**
 * Bounding box of @p region grown by @p rings whole placed CELLS left/right
 * and @p rings rows up/down.
 *
 * Rings count cells, not sites or DBU: the walk steps over each placed
 * instance regardless of kind, so a std cell is a ring member like any other
 * and never stops it. Cells overlapping @p region are snapped in whole, empty
 * sites are skipped without consuming a ring, and the result clamps at the
 * core so it never leaves the placeable area.
 *
 * Coordinates are core-relative DBU -- the frame Node::getBBox() and
 * Grid::gridX(DbuX) use, NOT the absolute frame of getCoreArea().
 */
Rect getBoundingBox(const Rect& region, int rings = 3) const;
fillerSetting* getFillerSetting() { return filler_setting_.get();};

private:
using bgPoint
    = boost::geometry::model::d2::point_xy<int,
                                     boost::geometry::cs::cartesian>;
using bgBox = boost::geometry::model::box<bgPoint>;

using RtreeBox
    = boost::geometry::index::rtree<bgBox,
                                    boost::geometry::index::quadratic<16>>;

friend class TestDePlaceCmd;
friend class TestObjectsCmd;
friend class TestPlacementDRCCmd;
friend class TestFindLegCmd;
friend class TestImplantCmd;
friend class TestEcoFlowCmd;
void importDb();
void importClear();
void initEdgeTypeTable();
void createNetwork();
void deleteGrid();
bool hasOneSiteMaster(PhysDesMgr* desMgr);
void setUpPlacementGroups();
void groupInitPixels();

// Legalization methods
DbuPt initialLocation(const Node* cell, bool padded) const;
DbuPt legalPt(const Node* cell, const DbuPt& pt) const;
DbuPt legalPt(const Node* cell, bool padded) const;
GridPt legalGridPt(const Node* cell, const DbuPt& pt) const;
GridPt legalGridPt(const Node* cell, bool padded) const;
DbuPt nearestPt(const Node* cell, const DbuRect& rect) const;
DbuPt nearestBlockEdge(const Node* cell, const DbuPt& pt,
    const Rect& block_bbox) const;

// Placement methods
bool canBePlaced(const Node* cell, GridX x, GridY y) const;
bool moveHopeless(const Node* cell, GridX& grid_x, GridY& grid_y) const;
bool diamondMove(Node* cell);
bool diamondMove(Node* cell, const GridPt& grid_pt);
void placeCell(Node* cell, const GridX x, const GridY y);
void unplaceCell(Node* cell);
void setGridLoc(Node* cell, const GridX x, const GridY y);
bool checkPixels(const Node* cell,
                 GridX x,
                 GridY y,
                 GridX x_end,
                 GridY y_end) const;
unsigned getMasterSymmetry(int symmetry) const;
bool checkMasterSym(unsigned masterSym, eUTL::PhysOrientation cellOri) const;

PixelPt diamondSearch(const Node* cell, GridX x, GridY y) const;
// Search methods
PixelPt diamondSearch(const Node* cell,
                      GridX x,
                      GridY y,
                      GridX x_min,
                      GridX x_max,
                      GridY y_min,
                      GridY y_max) const;
int calcDist(const GridPt& p1, const GridPt& p2) const;

// Rip-up and replace
bool ripUpAndReplaceInRect(Node* target_cell,
                           const GridRect& target_grid_rect,
                           const GridPt& start_pt);
bool legalCellInRect(const Rect& rect, Node* cell);

// Grid initialization
void initGrid();
void initPlacementDRC();

// Member variables
eUNL::Design* design_;
eUNL::PhysDesMgr* desMgr_;

std::unique_ptr<Architecture> arch_;
std::unique_ptr<Network> network_;    // The netlist, cells, etc.
std::shared_ptr<Padding> padding_;
std::unique_ptr<PlacementDRC> drc_engine_;
std::unique_ptr<EdgeTypeTable> edge_type_table_;
Rect core_;
bool disallow_one_site_gaps_ = false;
int max_displacement_x_ = 0;  // sites
int max_displacement_y_ = 0;  // sites
bool data_loaded_  = false;

// 2D pixel grid
std::unique_ptr<Grid> grid_;
RtreeBox regions_rtree_;

// filler cell config
std::unique_ptr<fillerSetting> filler_setting_;

// Placement tracking
std::vector<Node*> placement_failures_;
};



} // namespace dpl2