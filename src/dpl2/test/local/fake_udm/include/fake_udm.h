// Fake UDM for the tier-1 local build (interface names match the real UDM;
// implementations are in-memory data models tests can populate).
//
// Scope: everything the repair chain compiles against --
// infrastructure (Network/Grid/fillerSetting), the final
// ipl::ImplantLayerChecker, and the fillerRepair runtime engine (including
// the test-only fixture that wires supplied Grid/Network).
// NOT a behavioral UDM: only the accessors those files call are modeled.
//
// Data flow for tests: build a fake_udm::DesignDb (tech layers, lib cells,
// rows, cells), then db.activate() to make it the Session's current design.
// Everything hands out stable pointers/references into the Db (std::deque
// storage).

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// UDM assertion macro.
#ifndef uvAssert
#define uvAssert(expr) assert(expr)
#endif

// ---------------------------------------------------------------------------
// eUTL: geometry / orientation
// ---------------------------------------------------------------------------
namespace eUTL {

class UvDist
{
 public:
  UvDist() = default;
  explicit UvDist(int64_t value) : value_(static_cast<int32_t>(value)) {}

  int32_t getStorage() const { return value_; }

  friend bool operator<(UvDist a, UvDist b) { return a.value_ < b.value_; }
  friend bool operator>(UvDist a, UvDist b) { return b < a; }
  friend bool operator<=(UvDist a, UvDist b) { return !(b < a); }
  friend bool operator>=(UvDist a, UvDist b) { return !(a < b); }
  friend bool operator==(UvDist a, UvDist b) { return a.value_ == b.value_; }
  friend bool operator!=(UvDist a, UvDist b) { return !(a == b); }
  friend UvDist operator+(UvDist a, UvDist b)
  {
    return UvDist(static_cast<int64_t>(a.value_) + b.value_);
  }
  friend UvDist operator-(UvDist a, UvDist b)
  {
    return UvDist(static_cast<int64_t>(a.value_) - b.value_);
  }
  friend UvDist operator-(UvDist value)
  {
    return UvDist(-static_cast<int64_t>(value.value_));
  }

 private:
  int32_t value_ = 0;
};

struct Point2D
{
  Point2D() = default;
  Point2D(UvDist x, UvDist y) : x_(x), y_(y) {}
  UvDist getX() const { return x_; }
  UvDist getY() const { return y_; }
  UvDist x_;
  UvDist y_;
};

class Rect
{
 public:
  Rect() = default;
  Rect(UvDist xl, UvDist yl, UvDist xh, UvDist yh)
      : _xl(xl), _yl(yl), _xh(xh), _yh(yh)
  {
  }

  UvDist getXL() const { return _xl; }
  UvDist getYL() const { return _yl; }
  UvDist getXH() const { return _xh; }
  UvDist getYH() const { return _yh; }
  void setXL(UvDist v) { _xl = v; }
  void setYL(UvDist v) { _yl = v; }
  void setXH(UvDist v) { _xh = v; }
  void setYH(UvDist v) { _yh = v; }
  UvDist dx() const { return _xh - _xl; }
  UvDist dy() const { return _yh - _yl; }
  void move(UvDist dx, UvDist dy)
  {
    _xl = _xl + dx;
    _xh = _xh + dx;
    _yl = _yl + dy;
    _yh = _yh + dy;
  }
  Point2D center() const
  {
    return Point2D(
        UvDist((static_cast<int64_t>(_xl.getStorage()) + _xh.getStorage()) / 2),
        UvDist((static_cast<int64_t>(_yl.getStorage()) + _yh.getStorage()) / 2));
  }
  bool intersect(const Rect& other, bool strict = true) const
  {
    const auto lt = [strict](int32_t a, int32_t b) {
      return strict ? a < b : a <= b;
    };
    return lt(_xl.getStorage(), other._xh.getStorage())
           && lt(other._xl.getStorage(), _xh.getStorage())
           && lt(_yl.getStorage(), other._yh.getStorage())
           && lt(other._yl.getStorage(), _yh.getStorage());
  }
  Rect overlap(const Rect& other, bool strict = true) const
  {
    if (!intersect(other, strict)) {
      return Rect{};
    }
    return Rect(UvDist(std::max(_xl.getStorage(), other._xl.getStorage())),
                UvDist(std::max(_yl.getStorage(), other._yl.getStorage())),
                UvDist(std::min(_xh.getStorage(), other._xh.getStorage())),
                UvDist(std::min(_yh.getStorage(), other._yh.getStorage())));
  }
  // Mutating point-expansion (Object.cpp hpwl) alongside the pure form.
  void expand(const Point2D& point)
  {
    _xl = UvDist(std::min(_xl.getStorage(), point.getX().getStorage()));
    _yl = UvDist(std::min(_yl.getStorage(), point.getY().getStorage()));
    _xh = UvDist(std::max(_xh.getStorage(), point.getX().getStorage()));
    _yh = UvDist(std::max(_yh.getStorage(), point.getY().getStorage()));
  }
  UvDist halfPerimeter() const { return dx() + dy(); }
  Rect expand(const Rect& other) const
  {
    return Rect(UvDist(std::min(_xl.getStorage(), other._xl.getStorage())),
                UvDist(std::min(_yl.getStorage(), other._yl.getStorage())),
                UvDist(std::max(_xh.getStorage(), other._xh.getStorage())),
                UvDist(std::max(_yh.getStorage(), other._yh.getStorage())));
  }
  std::string toString() const
  {
    std::ostringstream out;
    out << '[' << _xl.getStorage() << ',' << _yl.getStorage() << ','
        << _xh.getStorage() << ',' << _yh.getStorage() << ']';
    return out.str();
  }

  UvDist _xl;
  UvDist _yl;
  UvDist _xh;
  UvDist _yh;
};

enum class PhysOrientationE
{
  R0,
  R90,
  R180,
  R270,
  MX,
  MX90,
  MY,
  MY90
};

// Real UDM shape: a class wrapping the enum with getValue(); implicit
// conversions both ways keep enum-style comparisons and switches working.
class PhysOrientation
{
 public:
  PhysOrientation() = default;
  PhysOrientation(PhysOrientationE value) : value_(value) {}
  // Integer round-trip used by the checker helper's dump/load enum codecs.
  explicit PhysOrientation(int value)
      : value_(static_cast<PhysOrientationE>(value))
  {
  }
  explicit operator int() const { return static_cast<int>(value_); }
  PhysOrientationE getValue() const { return value_; }
  operator PhysOrientationE() const { return value_; }
  explicit operator unsigned() const
  {
    return static_cast<unsigned>(value_);
  }

 private:
  PhysOrientationE value_ = PhysOrientationE::R0;
};

template <typename T>
class uvRIter
{
};

// Scoped performance logger: the real one times a region and reports it.
// Nothing in the repair chain reads timings, so this is a no-op with the
// same construction shape.
class PerfLogger
{
 public:
  explicit PerfLogger(const std::string&, bool singleLine = false)
  {
    (void) singleLine;
  }
};

}  // namespace eUTL

using Rect = eUTL::Rect;

// ---------------------------------------------------------------------------
// id types (shared shape: (block, index); default-constructed = invalid)
// ---------------------------------------------------------------------------
namespace fake_udm {

struct IdBase
{
  int block = 0;
  int index = -1;

  IdBase() = default;
  IdBase(int b, int i) : block(b), index(i) {}

  int getIndexValue() const { return index; }
  int getValue() const { return index; }
  bool isValid() const { return index >= 0; }
  friend bool operator==(const IdBase& a, const IdBase& b)
  {
    return a.block == b.block && a.index == b.index;
  }
  friend bool operator!=(const IdBase& a, const IdBase& b) { return !(a == b); }
  friend bool operator<(const IdBase& a, const IdBase& b)
  {
    return a.block != b.block ? a.block < b.block : a.index < b.index;
  }
};

}  // namespace fake_udm

namespace eUNL {
struct LeafCellID : fake_udm::IdBase
{
  using fake_udm::IdBase::IdBase;
};
struct LibCellID : fake_udm::IdBase
{
  using fake_udm::IdBase::IdBase;
};
struct PinID : fake_udm::IdBase
{
  using fake_udm::IdBase::IdBase;
  PinID asFlatPin() const { return *this; }
};
struct PhysPinID : fake_udm::IdBase
{
  using fake_udm::IdBase::IdBase;
  PhysPinID(PinID id) : fake_udm::IdBase(id.block, id.index) {}
};
}  // namespace eUNL

namespace eFNL {
struct ModuleID;
}

// ---------------------------------------------------------------------------
// eLIB: tech + library
// ---------------------------------------------------------------------------
namespace eLIB {

using LibCellID = eUNL::LibCellID;

struct TechLayerRelativeID
{
  int v = -1;
  TechLayerRelativeID() = default;
  explicit TechLayerRelativeID(int value) : v(value) {}
  friend bool operator<(TechLayerRelativeID a, TechLayerRelativeID b)
  {
    return a.v < b.v;
  }
  friend bool operator==(TechLayerRelativeID a, TechLayerRelativeID b)
  {
    return a.v == b.v;
  }
};

struct TechLayerID
{
  TechLayerRelativeID local;
  TechLayerRelativeID getLocalId() const { return local; }
  operator int() const { return local.v; }
};

class TechShape
{
 public:
  enum Type
  {
    RECT,
    POLYGON
  };
  TechShape() = default;
  TechShape(Type type, const eUTL::Rect& rect) : type_(type), rect_(rect) {}
  Type getType() const { return type_; }
  bool isRect() const { return type_ == RECT; }
  bool isPolygon() const { return type_ == POLYGON; }
  const eUTL::Rect& getRect() const { return rect_; }
  class Polygon
  {
   public:
    explicit Polygon(const eUTL::Rect& bbox) : bbox_(bbox) {}
    const eUTL::Rect& getBbox() const { return bbox_; }

   private:
    eUTL::Rect bbox_;
  };
  Polygon getPolygon() const { return Polygon(rect_); }
  eUTL::Rect getBbox(bool) const { return rect_; }
  TechLayerID getLayer() const { return layer_; }
  int getMaskId() const { return mask_id_; }

  Type type_ = RECT;
  eUTL::Rect rect_;
  TechLayerID layer_;
  int mask_id_ = 0;
};

using ShapeMaskID = int;

// Per-orientation obstruction shape map keyed by layer; the alias the real
// library exports and PhysLibObs::getShapes returns.
using LayerShapeMapT = std::map<TechLayerRelativeID, std::vector<TechShape>>;

// --- LEF58 tech-rule model (checker buildRules input) ----------------------
// Minimal mirror of the UDM rule-check classes: a TechRule owns one
// polymorphic RuleCheck whose _type discriminates the concrete class the
// checker dynamic_casts to. Fixtures construct rules directly.
enum class RuleCheckType
{
  WIDTH_RULE,
  SPACING_IMPLANT_RULE,
  LIB_CELL_EDGE_SPACING_TABLE_RULE,
  OTHER_RULE,
};

enum class RuleCheckOrthoTypeE
{
  NONE,
  HORIZONTAL,
  VERTICAL,
};

struct RuleCheck
{
  explicit RuleCheck(RuleCheckType type) : _type(type) {}
  virtual ~RuleCheck() = default;
  RuleCheckType getType() const { return _type; }
  RuleCheckType _type;
};

class WidthRule : public RuleCheck
{
 public:
  WidthRule() : RuleCheck(RuleCheckType::WIDTH_RULE) {}
  eUTL::UvDist getWidth() const { return width_; }
  const std::string& getOtherImplLayerName() const { return other_layer_; }
  bool getZeroPRL() const { return zero_prl_; }
  bool getExceptCornerTouch() const { return except_corner_touch_; }
  eUTL::UvDist getLength() const { return length_; }
  const std::string& getCheckImplantGroup() const { return check_group_; }

  eUTL::UvDist width_;
  std::string other_layer_;
  bool zero_prl_ = false;
  bool except_corner_touch_ = false;
  eUTL::UvDist length_;
  std::string check_group_;
};

class SpacingImplantRule : public RuleCheck
{
 public:
  struct SIItem
  {
    eUTL::UvDist minSpacing;
    std::string layerName2;
    RuleCheckOrthoTypeE prlOrient = RuleCheckOrthoTypeE::NONE;
    eUTL::UvDist prl;
    bool exceptAbutted = false;
    bool exceptCornerTouch = false;
    eUTL::UvDist length;
    std::vector<std::string> layerNameList;
  };

  SpacingImplantRule() : RuleCheck(RuleCheckType::SPACING_IMPLANT_RULE) {}
  const std::vector<SIItem>& getSpacingImplantTable() const { return table_; }

  std::vector<SIItem> table_;
};

class TechRule
{
 public:
  explicit TechRule(std::shared_ptr<RuleCheck> check)
      : check_(std::move(check))
  {
  }
  const RuleCheck& getCheck() const { return *check_; }
  RuleCheckType getCheckType() const { return check_->_type; }
  const RuleCheck* getCheckPtr() const { return check_.get(); }

 private:
  std::shared_ptr<RuleCheck> check_;
};

class TechCellEdgeSpacingTable : public RuleCheck
{
 public:
  struct Entry
  {
    const std::string& getEdgeType1() const { return edgeType1; }
    const std::string& getEdgeType2() const { return edgeType2; }
    std::string edgeType1;
    std::string edgeType2;
  };

  TechCellEdgeSpacingTable()
      : RuleCheck(RuleCheckType::LIB_CELL_EDGE_SPACING_TABLE_RULE)
  {
  }
  const std::vector<Entry>& getEntries() const { return entries_; }

  std::vector<Entry> entries_;
};

class TechLayer
{
 public:
  class RoutingIndex
  {
   public:
    explicit RoutingIndex(int value = -1) : value_(value) {}
    int getNumValue() const { return value_; }

   private:
    int value_ = -1;
  };

  bool isImplant() const { return is_implant_; }
  bool isRouting() const { return is_routing_; }
  bool isOverlap() const { return is_overlap_; }
  RoutingIndex getRoutingIdx() const { return RoutingIndex(routing_idx_); }
  const std::string& getName() const { return name_; }
  TechLayerID getId() const
  {
    return TechLayerID{TechLayerRelativeID{rel_id_}};
  }
  eUTL::UvDist getWidth() const { return width_; }
  eUTL::UvDist getMinSpacing() const { return min_spacing_; }
  const std::vector<TechRule>& getRuleIter() const { return rules_; }

  std::string name_;
  bool is_implant_ = false;
  bool is_routing_ = false;
  bool is_overlap_ = false;
  int routing_idx_ = -1;
  int rel_id_ = -1;
  eUTL::UvDist width_;
  eUTL::UvDist min_spacing_;
  std::vector<TechRule> rules_;
};

class TechSite
{
 public:
  bool getIsPad() const { return is_pad_; }
  eUTL::UvDist getWidth() const { return width_; }
  eUTL::UvDist getHeight() const { return height_; }
  const std::string& getName() const { return name_; }

  bool is_pad_ = false;
  eUTL::UvDist width_;
  eUTL::UvDist height_;
  std::string name_ = "coreSite";
};

class TechLib
{
 public:
  const std::deque<TechLayer>& getLayerIter() const { return layers_; }
  const std::vector<TechRule>& getRuleIter() const { return rules_; }
  const TechLayer& getTechLayer(TechLayerRelativeID relId) const
  {
    for (const TechLayer& layer : layers_) {
      if (layer.rel_id_ == relId.v) {
        return layer;
      }
    }
    static const TechLayer kMissing{};
    return kMissing;
  }

  TechLayer& addLayer(const std::string& name,
                      bool isImplant,
                      int relId,
                      int64_t width = 0,
                      int64_t minSpacing = 0)
  {
    TechLayer layer;
    layer.name_ = name;
    layer.is_implant_ = isImplant;
    layer.rel_id_ = relId;
    layer.width_ = eUTL::UvDist(width);
    layer.min_spacing_ = eUTL::UvDist(minSpacing);
    layers_.push_back(layer);
    return layers_.back();
  }

  std::deque<TechLayer> layers_;
  std::vector<TechRule> rules_;
};

enum class ShapeUsageE
{
  UNKNOWN
};

using PhysMacroUsageSet = std::unordered_set<int>;

enum class SignalTypeE
{
  SIGNAL,
  POWER,
  GROUND
};

enum class MacroEdgeDir
{
  RIGHT = 0,
  LEFT = 1,
  TOP = 2,
  BOTTOM = 3,
  INVALID = 4
};

struct MacroEdge
{
  MacroEdgeDir edgeDir = MacroEdgeDir::RIGHT;
  std::string edgeTypeName = "DEFAULT";
  std::pair<eUTL::UvDist, eUTL::UvDist> range;
  bool has_range_ = false;
  int cellRow = 0;  // 1-based when set
  int halfRow = 0;  // 1-based when set
  bool hasRange() const { return has_range_; }
  bool hasCellRow() const { return cellRow > 0; }
  bool hasHalfRow() const { return halfRow > 0; }
};

class PhysLibTerm
{
 public:
  const std::map<TechLayerRelativeID, std::vector<TechShape>>& getShapes() const
  {
    return shapes_;
  }
  std::map<TechLayerRelativeID, std::vector<TechShape>> shapes_;
};

class PhysLibPort
{
 public:
  SignalTypeE getUse() const { return use_; }
  bool isPgPort() const
  {
    return use_ == SignalTypeE::POWER || use_ == SignalTypeE::GROUND;
  }
  const std::deque<PhysLibTerm>& getLibTermIter() const { return terms_; }
  std::vector<const PhysLibTerm*> getLibTerms() const
  {
    std::vector<const PhysLibTerm*> result;
    result.reserve(terms_.size());
    for (const PhysLibTerm& term : terms_) {
      result.push_back(&term);
    }
    return result;
  }

  SignalTypeE use_ = SignalTypeE::SIGNAL;
  std::deque<PhysLibTerm> terms_;
};

class PhysMacroType
{
 public:
  enum class TypeE
  {
    CORE,
    CORE_ANTENNACELL,
    CORE_FEEDTHRU,
    CORE_TIEHIGH,
    CORE_TIELOW,
    CORE_WELLTAP,
    CORE_FILLER,
    ENDCAP,
    ENDCAP_PRE,
    ENDCAP_POST,
    ENDCAP_TOPLEFT,
    ENDCAP_TOPRIGHT,
    ENDCAP_BOTTOMLEFT,
    ENDCAP_BOTTOMRIGHT,
    ENDCAP_LEF58_RIGHTEDGE,
    ENDCAP_LEF58_LEFTEDGE,
    ENDCAP_LEF58_BOTTOMEDGE,
    ENDCAP_LEF58_TOPEDGE,
    ENDCAP_LEF58_RIGHTBOTTOMEDGE,
    ENDCAP_LEF58_LEFTBOTTOMEDGE,
    ENDCAP_LEF58_RIGHTTOPEDGE,
    ENDCAP_LEF58_LEFTTOPEDGE,
    ENDCAP_LEF58_RIGHTBOTTOMCORNER,
    ENDCAP_LEF58_LEFTBOTTOMCORNER,
    ENDCAP_LEF58_RIGHTTOPCORNER,
    ENDCAP_LEF58_LEFTTOPCORNER,
    COVER,
    COVER_BUMP,
    RING,
    BLOCK,
    BLOCK_BLACKBOX,
    BLOCK_SOFT,
    PAD,
    PAD_AREAIO,
    PAD_INPUT,
    PAD_OUTPUT,
    PAD_INOUT,
    PAD_POWER,
    PAD_FILLER
  };

  PhysMacroType() = default;
  PhysMacroType(TypeE type) : type_(type) {}
  TypeE getType() const { return type_; }
  bool isCore() const
  {
    return type_ == TypeE::CORE || type_ == TypeE::CORE_ANTENNACELL
           || type_ == TypeE::CORE_FEEDTHRU || type_ == TypeE::CORE_TIEHIGH
           || type_ == TypeE::CORE_TIELOW || type_ == TypeE::CORE_WELLTAP
           || type_ == TypeE::CORE_FILLER;
  }
  bool isCoreFiller() const { return type_ == TypeE::CORE_FILLER; }
  bool isPadFiller() const { return type_ == TypeE::PAD_FILLER; }
  bool isPad() const
  {
    return type_ >= TypeE::PAD && type_ <= TypeE::PAD_FILLER;
  }
  bool isCover() const
  {
    return type_ == TypeE::COVER || type_ == TypeE::COVER_BUMP;
  }
  bool isEndcap() const
  {
    return type_ >= TypeE::ENDCAP
           && type_ <= TypeE::ENDCAP_LEF58_LEFTTOPCORNER;
  }
  bool isBlock() const
  {
    return type_ == TypeE::BLOCK || type_ == TypeE::BLOCK_BLACKBOX
           || type_ == TypeE::BLOCK_SOFT;
  }

  TypeE type_ = TypeE::CORE;
};

class PhysLibObs
{
 public:
  const std::map<TechLayerRelativeID, std::vector<TechShape>>& getShapes(
      eUTL::PhysOrientationE) const
  {
    return shapes_;
  }
  std::map<TechLayerRelativeID, std::vector<TechShape>> shapes_;
};

class LibCell;

class PhysLibCell
{
 public:
  // The real UDM reaches the timing-library view from the physical one;
  // commands print its name. Defined out of line: LibCell comes later.
  const LibCell& getLibCell() const;
  eUTL::UvDist getWidth() const { return width_; }
  eUTL::UvDist getHeight() const { return height_; }
  const PhysMacroType& getType() const { return type_; }
  const std::vector<PhysLibObs>& getObstruction() const { return obs_; }
  LibCellID getLibCellId() const { return lib_cell_id_; }
  const TechSite* getTechSite() const { return site_; }
  const std::vector<PhysLibPort*>& getPorts() const { return ports_; }
  const std::vector<MacroEdge>& getEdgeTypeVec() const { return edges_; }
  int getSymmetry() const { return symmetry_; }
  bool hasSitePattern() const { return !site_patterns_.empty(); }
  const std::vector<int>& getSitePatterns() const { return site_patterns_; }

  eUTL::UvDist width_;
  eUTL::UvDist height_;
  PhysMacroType type_;
  std::vector<PhysLibObs> obs_;
  LibCellID lib_cell_id_;
  const TechSite* site_ = nullptr;
  std::vector<PhysLibPort*> ports_;
  std::vector<MacroEdge> edges_;
  int symmetry_ = 0;
  std::vector<int> site_patterns_;
  // Owned by value so getLibCell() can hand back a reference.
  std::shared_ptr<LibCell> lib_cell_;
};

class PhysLib
{
};
class PhysLibObjs
{
};

class LibCell
{
 public:
  int getId() const { return id_; }
  const std::string& getName() const { return name_; }
  eFNL::ModuleID getMaster() const;
  int id_ = -1;
  std::string name_;
};

inline const LibCell& PhysLibCell::getLibCell() const
{
  static const LibCell kUnnamed;
  return lib_cell_ ? *lib_cell_ : kUnnamed;
}

}  // namespace eLIB

namespace std {
template <>
struct hash<eLIB::LibCellID>
{
  size_t operator()(const eLIB::LibCellID& id) const
  {
    return std::hash<long long>()(
        (static_cast<long long>(id.block) << 32) ^ id.index);
  }
};
}  // namespace std

// ---------------------------------------------------------------------------
// eFNL
// ---------------------------------------------------------------------------
namespace eFNL {
struct ModuleID : fake_udm::IdBase
{
  using fake_udm::IdBase::IdBase;
};
}  // namespace eFNL

inline eFNL::ModuleID eLIB::LibCell::getMaster() const
{
  return eFNL::ModuleID(0, id_);
}

// ---------------------------------------------------------------------------
// eUNL: design / placement
// ---------------------------------------------------------------------------
namespace eUNL {

enum class PhysObjStatus
{
  UNKNOWN,
  PLACED,
  LOC_FIXED
};

// Backing record for one placed cell; PhysCell is a value handle onto it.
struct PhysCellData
{
  bool valid = false;
  const eLIB::PhysLibCell* master = nullptr;
  PhysObjStatus status = PhysObjStatus::PLACED;
  eUTL::Point2D origin;
  eUTL::PhysOrientation orient = eUTL::PhysOrientationE::R0;
  std::string name;
};

class PhysCell
{
 public:
  PhysCell() = default;
  explicit PhysCell(const PhysCellData* data) : data_(data) {}
  bool isValid() const { return data_ != nullptr && data_->valid; }
  const eLIB::PhysLibCell& getPhysMaster() const { return *data_->master; }
  PhysObjStatus getStatus() const { return data_->status; }
  eUTL::Point2D getOrigin() const { return data_->origin; }
  eUTL::PhysOrientation getOrient() const { return data_->orient; }

 private:
  const PhysCellData* data_ = nullptr;
};

// Logical hierarchy handle. Destination UDM exposes instance names through
// HierManager::getLeafCell(), not through the physical placement handle.
class LeafCell
{
 public:
  LeafCell() = default;
  explicit LeafCell(const PhysCellData* data) : data_(data) {}
  const std::string& getName() const
  {
    static const std::string empty;
    return data_ != nullptr ? data_->name : empty;
  }

 private:
  const PhysCellData* data_ = nullptr;
};

class PhysTerm
{
 public:
  eUTL::Point2D getOrigin() const { return origin_; }
  eUTL::Point2D origin_;
};

class PhysPin
{
 public:
  const std::vector<PhysTerm>& getTermIter() const { return terms_; }
  std::vector<PhysTerm> terms_;
};

class PhysRow
{
 public:
  const eLIB::TechSite& getSite() const { return site_; }
  eUTL::Point2D getOrigin() const { return origin_; }
  eUTL::Rect getBbox() const { return bbox_; }
  int getSiteCnt() const { return site_cnt_; }
  eUTL::PhysOrientation getOrient() const { return orient_; }

  eLIB::TechSite site_;
  eUTL::Point2D origin_;
  eUTL::Rect bbox_;
  int site_cnt_ = 0;
  eUTL::PhysOrientation orient_ = eUTL::PhysOrientationE::R0;
};

class PhysBlockage
{
 public:
  bool isSoft() const { return is_soft_; }
  const std::vector<eLIB::TechShape>& getShapes() const { return shapes_; }

  bool is_soft_ = false;
  std::vector<eLIB::TechShape> shapes_;
};

class PhysRegion
{
 public:
  const std::vector<eUTL::Rect>& getRects() const { return rects_; }
  std::vector<eUTL::Rect> rects_;
};

class PhysGroup
{
 public:
  bool hasRegion() const { return !region_.rects_.empty(); }
  const PhysRegion& getRegion() const { return region_; }
  const std::vector<LeafCellID>& getLeafCells() const { return cells_; }

  PhysRegion region_;
  std::vector<LeafCellID> cells_;
};

class PhysDesMgr
{
 public:
  const eLIB::TechLib& getTopTech() const { return *tech_; }
  const std::deque<PhysRow>& getPhysRowIter() const { return rows_; }
  PhysCell getPhysCell(LeafCellID cellId) const
  {
    const auto it = cells_.find(cellId);
    return it != cells_.end() ? PhysCell(&it->second) : PhysCell();
  }
  PhysPin getPhysPin(PhysPinID pinId) const
  {
    const auto it = pins_.find(pinId);
    return it != pins_.end() ? it->second : PhysPin{};
  }
  const std::deque<PhysBlockage>& getPhysBlockageIter() const;
  const std::deque<PhysGroup>& getPhysGroupIter() const { return groups_; }

  template <typename Arena, typename Visitor, typename Usage>
  void iterateAllPhysCells(Arena&, Visitor& visitor, const Usage&) const
  {
    for (const auto& [id, data] : cells_) {
      const PhysCell cell(&data);
      if (visitor.filter(cell, id)) {
        visitor.visit(cell, id);
      }
    }
  }

  template <typename Arena, typename Visitor>
  void iterateAllPhysNets(Arena&, Visitor&, bool, bool) const
  {
    // The fake-UDM fixture has no routed nets.  This is data, not a Grid
    // behavioral substitute: runtime Grid still executes its real scan.
  }

  // --- test-population helpers ---
  PhysRow& addRow(int64_t originX,
                  int64_t originY,
                  int64_t siteWidth,
                  int64_t siteHeight,
                  int64_t rowWidth,
                  bool isPad = false)
  {
    PhysRow row;
    row.site_.is_pad_ = isPad;
    row.site_.width_ = eUTL::UvDist(siteWidth);
    row.site_.height_ = eUTL::UvDist(siteHeight);
    row.origin_ = eUTL::Point2D(eUTL::UvDist(originX), eUTL::UvDist(originY));
    row.bbox_ = eUTL::Rect(eUTL::UvDist(originX), eUTL::UvDist(originY),
                           eUTL::UvDist(originX + rowWidth),
                           eUTL::UvDist(originY + siteHeight));
    row.site_cnt_ = static_cast<int>(rowWidth / siteWidth);
    rows_.push_back(row);
    return rows_.back();
  }
  PhysCellData& addCell(
      LeafCellID id,
      const eLIB::PhysLibCell* master,
      int64_t x,
      int64_t y,
      eUTL::PhysOrientation orient = eUTL::PhysOrientationE::R0,
      PhysObjStatus status = PhysObjStatus::PLACED,
      const std::string& name = {})
  {
    PhysCellData data;
    data.valid = true;
    data.master = master;
    data.status = status;
    data.origin = eUTL::Point2D(eUTL::UvDist(x), eUTL::UvDist(y));
    data.orient = orient;
    data.name = name;
    return cells_[id] = data;
  }
  PhysBlockage& addBlockage(int64_t xl,
                            int64_t yl,
                            int64_t xh,
                            int64_t yh,
                            bool isSoft = false)
  {
    PhysBlockage blockage;
    blockage.is_soft_ = isSoft;
    blockage.shapes_.emplace_back(
        eLIB::TechShape::RECT,
        eUTL::Rect(eUTL::UvDist(xl), eUTL::UvDist(yl), eUTL::UvDist(xh),
                   eUTL::UvDist(yh)));
    blockages_.push_back(std::move(blockage));
    return blockages_.back();
  }
  PhysPin& addPin(PhysPinID id, int64_t x, int64_t y)
  {
    PhysPin pin;
    pin.terms_.push_back(
        PhysTerm{eUTL::Point2D(eUTL::UvDist(x), eUTL::UvDist(y))});
    return pins_[id] = std::move(pin);
  }

  const eLIB::TechLib* tech_ = nullptr;
  std::deque<PhysRow> rows_;
  std::map<LeafCellID, PhysCellData> cells_;
  std::map<PhysPinID, PhysPin> pins_;
  std::deque<PhysBlockage> blockages_;
  std::deque<PhysGroup> groups_;
};

class PhysCellImpl
{
};
class PhysWire
{
};
enum class ShapeUsageE
{
  SIGNAL,
  DRCFILL
};

struct PhysNetID : fake_udm::IdBase
{
  using fake_udm::IdBase::IdBase;
};

enum class UnlIterStatus
{
  CONTINUE,
  STOP
};

template <typename Object, typename Id>
class UnlBaseVisitor
{
 public:
  virtual ~UnlBaseVisitor() = default;
  virtual bool filter(const Object&, const Id&) = 0;
  virtual UnlIterStatus visit(const Object&, const Id&) = 0;
};

class PhysShape
{
 public:
  bool isVia() const { return is_via_; }
  ShapeUsageE getUsage() const { return usage_; }
  const eUTL::Rect& getRect() const { return rect_; }
  eLIB::TechLayerID getLayer() const { return layer_; }

  bool is_via_ = false;
  ShapeUsageE usage_ = ShapeUsageE::SIGNAL;
  eUTL::Rect rect_;
  eLIB::TechLayerID layer_;
};

class PhysSWire
{
 public:
  const std::vector<PhysShape>& getShapes() const { return shapes_; }
  std::vector<PhysShape> shapes_;
};

class PhysNet
{
 public:
  bool hasSWire() const { return has_swire_; }
  const PhysSWire& getSWire() const { return swire_; }

  bool has_swire_ = false;
  PhysSWire swire_;
};

inline const std::deque<PhysBlockage>& PhysDesMgr::getPhysBlockageIter() const
{
  return blockages_;
}
class HierManager
{
 public:
  HierManager() = default;
  explicit HierManager(const PhysDesMgr* desMgr) : des_mgr_(desMgr) {}
  LeafCell getLeafCell(LeafCellID id) const
  {
    if (des_mgr_ == nullptr) {
      return LeafCell{};
    }
    const auto found = des_mgr_->cells_.find(id);
    return found != des_mgr_->cells_.end() ? LeafCell(&found->second)
                                          : LeafCell{};
  }

 private:
  const PhysDesMgr* des_mgr_ = nullptr;
};

class Design;

// Global session with a settable current design (mirrors the real singleton
// the checker constructor consults; tests inject via DesignDb::activate()).
class Session
{
 public:
  static Session& getSession()
  {
    static Session session;
    return session;
  }
  Design* getCurrentDesign() { return design_; }
  void setCurrentDesign(Design* design) { design_ = design; }

 private:
  Design* design_ = nullptr;
};

}  // namespace eUNL

namespace std {
template <>
struct hash<eUNL::LeafCellID>
{
  size_t operator()(const eUNL::LeafCellID& id) const
  {
    return std::hash<long long>()(
        (static_cast<long long>(id.block) << 32) ^ id.index);
  }
};
}  // namespace std

// ---------------------------------------------------------------------------
// Library accessor (fillerSetting: name -> module -> lib cell -> phys cell)
// ---------------------------------------------------------------------------
namespace fake_udm {

class LibAcc
{
 public:
  eFNL::ModuleID findModule(const std::string& name) const
  {
    const auto it = modules_.find(name);
    return it != modules_.end() ? it->second : eFNL::ModuleID();
  }
  const eLIB::LibCell* getLibCell(eFNL::ModuleID id) const
  {
    const auto it = lib_cells_.find(id.getIndexValue());
    return it != lib_cells_.end() ? &it->second : nullptr;
  }
  const eLIB::LibCell& getLibCell(eLIB::LibCellID id) const
  {
    return lib_cells_.at(id.getIndexValue());
  }
  const eLIB::PhysLibCell& getPhysLibCell(int libCellId) const
  {
    return *phys_cells_.at(libCellId);
  }
  const eLIB::PhysLibCell& getPhysLibCell(eLIB::LibCellID id) const
  {
    return *phys_cells_.at(id.getIndexValue());
  }
  std::vector<eLIB::LibCell> getLibCellIter(bool, bool) const
  {
    std::vector<eLIB::LibCell> cells;
    cells.reserve(lib_cells_.size());
    for (const auto& [id, cell] : lib_cells_) {
      (void) id;
      cells.push_back(cell);
    }
    return cells;
  }

  // --- test-population: register a master under a name.
  void addMaster(const std::string& name, const eLIB::PhysLibCell* cell)
  {
    const int id = cell->getLibCellId().getIndexValue();
    modules_[name] = eFNL::ModuleID(0, id);
    eLIB::LibCell libCell;
    libCell.id_ = id;
    libCell.name_ = name;
    lib_cells_[id] = libCell;
    phys_cells_[id] = cell;
  }

  std::map<std::string, eFNL::ModuleID> modules_;
  std::map<int, eLIB::LibCell> lib_cells_;
  std::map<int, const eLIB::PhysLibCell*> phys_cells_;
};

}  // namespace fake_udm

namespace eUNL {

using LibObjAccessor = fake_udm::LibAcc;

class Design
{
 public:
  Design() : hier_mgr_(&des_mgr_) {}
  PhysDesMgr* getPhysDesMgr() { return &des_mgr_; }
  const PhysDesMgr* getPhysDesMgr() const { return &des_mgr_; }
  fake_udm::LibAcc& getLibAcc() { return lib_acc_; }
  const fake_udm::LibAcc& getLibAcc() const { return lib_acc_; }
  HierManager* getHierMgr() { return &hier_mgr_; }
  const HierManager* getHierMgr() const { return &hier_mgr_; }

  PhysDesMgr des_mgr_;
  fake_udm::LibAcc lib_acc_;
  HierManager hier_mgr_;
};

enum class UnlChangePhaseE
{
  PRE_CHANGE,
  POST_CHANGE
};

class NlEditor
{
 public:
  explicit NlEditor(Design* design) : design_(design) {}
  Design* getDesign() const { return design_; }

 private:
  Design* design_ = nullptr;
};

class UnlChange_sizeCell
{
 public:
  UnlChange_sizeCell(NlEditor*,
                     Design& design,
                     UnlChangePhaseE,
                     LeafCellID cellId,
                     eFNL::ModuleID,
                     eFNL::ModuleID newMaster)
      : design_(design), cell_id_(cellId), new_master_(newMaster)
  {
  }

  bool feasible() const
  {
    return cell_id_.isValid() && new_master_.isValid()
           && design_.getPhysDesMgr()->getPhysCell(cell_id_).isValid()
           && design_.getLibAcc().getLibCell(new_master_) != nullptr;
  }

  void commit()
  {
    if (!feasible()) {
      return;
    }
    design_.getPhysDesMgr()->cells_[cell_id_].master
        = &design_.getLibAcc().getPhysLibCell(
            eLIB::LibCellID(new_master_.block, new_master_.index));
  }

 private:
  Design& design_;
  LeafCellID cell_id_;
  eFNL::ModuleID new_master_;
};

class UnlChange_removeCell
{
 public:
  UnlChange_removeCell(NlEditor*,
                       Design& design,
                       UnlChangePhaseE,
                       LeafCellID cellId)
      : design_(design), cell_id_(cellId)
  {
  }

  bool feasible() const
  {
    return cell_id_.isValid()
           && design_.getPhysDesMgr()->getPhysCell(cell_id_).isValid();
  }

  void commit()
  {
    if (feasible()) {
      design_.getPhysDesMgr()->cells_[cell_id_].valid = false;
    }
  }

 private:
  Design& design_;
  LeafCellID cell_id_;
};

}  // namespace eUNL

// ---------------------------------------------------------------------------
// fake_udm::DesignDb -- one-stop container for tests: owns the tech lib, the
// master library (stable addresses) and the design/session wiring.
// ---------------------------------------------------------------------------
namespace fake_udm {

struct DesignDb
{
  eLIB::TechSite coreSite;
  eUNL::Design design;

  DesignDb() { design.des_mgr_.tech_ = &tech_; }

  eLIB::TechLib& tech() { return tech_; }
  eUNL::PhysDesMgr& desMgr() { return design.des_mgr_; }

  eLIB::PhysLibCell& addMaster(const std::string& name,
                               int libCellIndex,
                               int64_t width,
                               int64_t height,
                               bool isCoreFiller)
  {
    masters_.emplace_back();
    eLIB::PhysLibCell& cell = masters_.back();
    cell.width_ = eUTL::UvDist(width);
    cell.height_ = eUTL::UvDist(height);
    cell.type_ = eLIB::PhysMacroType(
        isCoreFiller ? eLIB::PhysMacroType::TypeE::CORE_FILLER
                     : eLIB::PhysMacroType::TypeE::CORE);
    cell.lib_cell_id_ = eLIB::LibCellID(0, libCellIndex);
    cell.site_ = &coreSite;
    cell.lib_cell_ = std::make_shared<eLIB::LibCell>();
    cell.lib_cell_->id_ = libCellIndex;
    cell.lib_cell_->name_ = name;
    design.lib_acc_.addMaster(name, &cell);
    return cell;
  }

  // One full-width implant RECT on `relId` covering [0,width) x [yl,yh) in
  // the master's R0 frame.
  static void addShape(eLIB::PhysLibCell& cell,
                       int relId,
                       int64_t yl,
                       int64_t yh)
  {
    if (cell.obs_.empty()) {
      cell.obs_.emplace_back();
    }
    cell.obs_.front().shapes_[eLIB::TechLayerRelativeID(relId)].emplace_back(
        eLIB::TechShape::RECT,
        eUTL::Rect(eUTL::UvDist(0), eUTL::UvDist(yl), cell.getWidth(),
                   eUTL::UvDist(yh)));
  }

  void activate() { eUNL::Session::getSession().setCurrentDesign(&design); }

 private:
  eLIB::TechLib tech_;
  std::deque<eLIB::PhysLibCell> masters_;
};

}  // namespace fake_udm
