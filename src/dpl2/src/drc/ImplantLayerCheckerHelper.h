#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// UDM
#include <phys/fpManager.hh>
#include <phys/physDesMgr.hh>
#include <phys/physHier.hh>
#include <physHierImpl.hh>
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
using eUNL::PhysCellImpl;
using eUNL::PhysPin;
using eUNL::PhysWire;
using eUNL::PhysShape;
using eUNL::PhysObjStatus;
using eUNL::PhysDesMgr;
using eUTL::uvRIter;
using eLIB::ShapeUsageE;
using eLIB::TechLayerID;
using eLIB::TechLayer;
using eLIB::TechShape;
using eLIB::PhysLib;
using eLIB::PhysLibObjs;
using eLIB::PhysLibCell;
using eLIB::TechSite;
using eUTL::UvDist;
using eUTL::Rect;
using eUTL::Point2D;
using eUTL::PhysOrientationE;
using eUTL::PhysOrientation;

namespace dpl2 {
namespace ipl {

using DbCoord = int64_t;
using LayerId = int32_t;
using MasterId = int32_t;
using InstanceId = int32_t;
using ShapeId = int32_t;
using RowId = int32_t;
using GroupId = int32_t;

enum class BandSlot { Bottom, Top };
enum class Polarity { N, P };
enum class Family { VTS, VTL, VTH, VTUL, Unknown };
enum class RuleSource { Width, Spacing, Lef58Width, Lef58Spacing };
enum class RuleDirection { Any, Horizontal, Vertical };

struct XInterval
{
  DbCoord xl = 0;
  DbCoord xh = 0;
};

// Use eUTL::Rect (included via using eUTL::Rect above) rather than defining
// our own to avoid name conflicts with the UDM Rect type.

struct ImplantLayer
{
  LayerId id = 0;
  std::string name;
  Family family = Family::Unknown;
  Polarity polarity = Polarity::N;
};

struct Rule
{
  int ruleId = 0;
  RuleSource source = RuleSource::Width;
  LayerId primaryLayer = 0;
  std::optional<LayerId> secondaryLayer;
  DbCoord minValue = 0;
  RuleDirection direction = RuleDirection::Any;
  std::optional<DbCoord> prl;
  bool zeroPrl = false;
  bool exceptAbutted = false;
  bool exceptCornerTouch = false;
  std::optional<DbCoord> length;
  std::optional<std::string> checkGroup;
  std::vector<LayerId> intersectLayers;
  std::vector<std::string> unsupportedClauses;
  std::optional<int> containmentGroup;
  std::vector<int> containedByRuleIds;
  int specificityRank = 0;
};

struct MasterShape
{
  MasterId masterId = 0;
  ShapeId shapeId = 0;
  LayerId layer = 0;
  ::Rect rect;  // eUTL::Rect via using eUTL::Rect
};

struct MasterInput
{
  MasterId masterId = 0;
  DbCoord width = 0;
  DbCoord height = 0;
  std::vector<MasterShape> shapes;     // rebuilt band shapes (output of rebuildMasterShapes)
  std::vector<MasterShape> rawShapes;  // original raw shapes (preserved input)
  DbCoord siteHeight = 0;              // site height from the master's site type
  bool isFiller = false;
};

struct PlacedInst
{
  InstanceId instanceId = 0;
  MasterId masterId = 0;
  RowId rowId = 0;
  DbCoord columnId = 0;
  PhysOrientation orientation = PhysOrientationE::R0;
  bool isFiller = false;
};

struct RowInput
{
  RowId rowId = 0;
};

struct SlotRef
{
  RowId rowId = 0;
  BandSlot bandSlot = BandSlot::Bottom;
};

struct TrackPattern
{
  std::map<std::pair<RowId, BandSlot>, LayerId> layerBySlot;
  std::map<std::pair<RowId, RowId>, Polarity> activeKindByBoundary;

  std::optional<LayerId> layerForSlot(RowId rowId, BandSlot bandSlot) const;
  std::vector<SlotRef> adjacentSlots(RowId rowId, BandSlot bandSlot) const;
  std::optional<Polarity> activeInterRowKind(RowId rowA, RowId rowB) const;
};

struct ImplantInput
{
  std::vector<ImplantLayer> layers;
  std::vector<Rule> rules;
  std::unordered_map<std::string, std::vector<LayerId>> groups;
  std::vector<MasterInput> masters;
  std::vector<PlacedInst> placedInsts;
  std::vector<RowInput> rows;
  TrackPattern tracks;
  DbCoord rowHeight = 0;
  DbCoord siteWidth = 0;
};

class ImplantLayerCheckerHelper
{
 public:
  ImplantLayerCheckerHelper();
  ImplantLayerCheckerHelper(const ImplantInput& input) : data_(input) {};

  // Initialize by extracting data from UDM PhysDesMgr
  void init(const PhysDesMgr& desMgr);

  // Get the populated checker input
  ImplantInput& getImplantInput() { return data_; }

  // Get a string summary of the extracted data for statistics display
  std::string toString() const;

 private:
  // Parse layer name to extract family (VTL/VTH/VTUL) and polarity (N/P)
  static void parseLayerName(const std::string& name, Family& family, Polarity& polarity);

  // Build row track pattern from rows and implant layers
  void buildTrackPattern();

  // Rebuild master shapes into canonical band-level shapes.
  void rebuildMasterShapes();

  // Lookup helper: find checker LayerId from TechLayerRelativeID
  LayerId findLayerId(eLIB::TechLayerRelativeID relId) const;

  // diagnostics
  void addDiag(const std::string& msg);

  ImplantInput data_;
  std::vector<std::string> diags_;

  // Mapping from TechLayerRelativeID to checker's sequential LayerId
  std::map<eLIB::TechLayerRelativeID, LayerId> techLayerToCheckerId_;
};

}  // namespace ipl
}  // namespace dpl2
