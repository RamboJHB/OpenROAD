// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors
#pragma once

#include <boost/geometry/core/cs.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <cstddef>
#include <mutex>
#include <vector>

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
#include <unl/nlChange.hh>
#include <unl/nlEditor.hh>
#include <mv/mvObj.hh>

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
  class TestFillerRepairCmd;
}

namespace dpl2 {

class Grid;
class Node;
class Master;
namespace ipl {
class ImplantLayerChecker;
}
namespace fillerRepair {
class FillerRepairEngine;
}
struct Pixel;
class PixelPt;
struct GridPt;
struct GridRect;
struct DbuPt;
struct DbuRect;
class Padding;
class EdgeTypeTable;
class Network;
class TestDePlaceCmd;
class PlacementDRC;
class Architecture;
class fillerSetting;
enum class OpType : uint8_t;
struct CellChangeRecord;

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

struct CellChangeRecord;

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
  // shared body of the two function below: paints one node's
  // footprint (and its padding reservation) into the grid.
  void paintGridCell(Node* cell);
  void setFixedGridCells();
  void setPlacedGridCells();
  void setGridCell(Node* cell, Pixel* pixel);
  void setPaddingGlobal(int left, int right);
  void setPadding(PhysLibCell* master, int left, int right);
  void setPadding(LeafCellID cellId, int left, int right);
  std::pair<int, int> findLeg(eUNL::PinID startLoc, \
                              eUNL::VoltageArea* va,
                              int diameter, \
                              LibCellID masterId, \
                              std::vector<CellChangeRecord>& ccRecords);
  bool isLegal(LeafCellID instId, LibCellID masterId,
      std::vector<CellChangeRecord>& ccRecords);
  bool findLegal(CellChangeRecord& targetAdd,
                 int diameter,
                 std::vector<CellChangeRecord>& fillerChanges) const;
  bool commit(const std::vector<CellChangeRecord>& ccRecords);
  Rect getBoundingBox(const Rect& operableRect);
  PhysDesMgr* getDesMgr() {return desMgr_;};
  eUNL::Design* getDesign() { return design_; }
  const eUNL::Design* getDesign() const { return design_; }
  Grid* getGrid() {return grid_.get();};
  const Grid* getGrid() const {return grid_.get();};
  Network* getNetwork() {return network_.get();};
  const Network* getNetwork() const {return network_.get();};
  PlacementDRC* getPlacementDRC() {return drc_engine_.get();};
  Rect getCoreArea();
  fillerSetting* getFillerSetting() { return filler_setting_.get();};
  // [FRPORT] Setup-time publication of one immutable repair revision. Call
  // after fillerSetting and the complete opto target-master set are known.
  bool initializeFillerRepair(
      const std::vector<const PhysLibCell*>& targetMasters = {});
  bool isFillerRepairReady() const;
  bool repairFillers(const CellChangeRecord& targetChange,
                     std::vector<CellChangeRecord>& fillerChanges) const;
  // [FRPORT] Infrastructure-owned master registration uses the real edge
  // table. This remains available to callers that only need classification.
  bool registerFillerRepairMasters();
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
  friend class TestIsLegalCmd;
  friend class TestFindLegCmd;
  friend class TestImplantCmd;
  friend class TestEcoFlowCmd;
  void importDb();
  void importClear();
  void initEdgeTypeTable();
  void createNetwork();
  bool registerFillerRepairMasters(
      const std::vector<const PhysLibCell*>& targetMasters);
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
  bool canBePlaced(const Node* cell, GridX x, GridY y,
                   std::vector<CellChangeRecord>* fillerChanges = nullptr) const;
  bool moveHopeless(const Node* cell, GridX& grid_x, GridY& grid_y) const;
  bool diamondMove(Node* cell);
  bool diamondMove(Node* cell, const GridPt& grid_pt);
  void placeCell(Node* cell, const GridX x, const GridY y);
  void unplaceCell(Node* cell);
  void setGridLoc(Node* cell, const GridX x, const GridY y) const;
  bool checkPixels(const Node* cell,
                   GridX x,
                   GridY y,
                   GridX x_end,
                   GridY y_end,
                   std::vector<CellChangeRecord>* fillerChanges = nullptr) const;
  unsigned getMasterSymmetry(int symmetry) const;
  bool checkMasterSym(unsigned masterSym, eUTL::PhysOrientation cellOri) const;

  PixelPt diamondSearch(const Node* cell, GridX x, GridY y,
                        std::vector<CellChangeRecord>* fillerChanges = nullptr) const;
  // Search methods
  PixelPt diamondSearch(const Node* cell,
                        GridX x,
                        GridY y,
                        GridX x_min,
                        GridX x_max,
                        GridY y_min,
                        GridY y_max,
                        std::vector<CellChangeRecord>* fillerChanges = nullptr) const;
  int calcDist(const GridPt& p1, const GridPt& p2) const;

  // Rip-up and replace
  std::pair<int, int> ripUpAndReplaceInRect(Node* target_cell,
                                    const GridRect& target_grid_rect,
                                    const GridPt& start_pt);
  std::pair<int, int> legalCellInRect(const Rect& rect, Node* cell,
                    std::vector<CellChangeRecord>* fillerChanges = nullptr);

  // Read-only probe helpers (no in-memory placement mutation):
  // Initialize a throw-away stack Node for @p master at @p origin, binding a
  // valid db instance id from the occupied origin site if any.
  void initTempNode(Node& cell, Master* master, const PhysLibCell& pcell,
                    const Point2D& origin) const;

  // Read-only overlay DRC of swapping @p target to master @p masterId at its
  // current location, without touching in-memory placement state (the target
  // cell and any fillers its new footprint covers are fed to the checkers as
  // a removed/replaced overlay).
  bool isLegalProbe(LibCellID masterId, const Node* target,
                    std::vector<CellChangeRecord>& cellChanges);
  std::pair<int, int> findLegalAdd(
      const std::string& targetName,
      const PhysLibCell& master,
      GridX preferredX,
      GridY preferredY,
      GridX xMin,
      GridX xMax,
      GridY yMin,
      GridY yMax,
      std::vector<CellChangeRecord>& fillerChanges) const;

  // Grid initialization
  void initGrid();
  void initPlacementDRC();

  // Member variables
  eUNL::Design* design_;
  eUNL::PhysDesMgr* desMgr_;

  std::unique_ptr<Architecture> arch_;
  std::unique_ptr<Network> network_;     // The netlist, cells, etc.
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

  // [FRPORT] DePlace owns the revision. PlacementDRC owns the checker; the
  // engine only borrows it, while the checker publishes a non-owning atomic
  // engine pointer to concurrent check() calls. Declaration order guarantees
  // the engine is destroyed before PlacementDRC destroys the checker.
  ipl::ImplantLayerChecker* implant_layer_checker_{nullptr};
  std::unique_ptr<fillerRepair::FillerRepairEngine> filler_repair_engine_;
  std::vector<LibCellID> filler_repair_filler_ids_;
  std::size_t filler_repair_master_count_{0};
  mutable std::mutex filler_repair_init_mutex_;

  // Placement tracking
  std::vector<Node*> placement_failures_;
};

} // namespace dpl2
