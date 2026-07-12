#pragma once

#include <cstdint>
#include <sstream>
#include <string>

namespace eUTL {

struct DbuValueInt32
{
  explicit DbuValueInt32(int32_t value) : value(value) {}
  int32_t value;
};

class UvDist
{
 public:
  UvDist() = default;
  explicit UvDist(int64_t value) : value_(static_cast<int32_t>(value)) {}
  explicit UvDist(DbuValueInt32 value) : value_(value.value) {}

  int32_t getStorage() const { return value_; }

  friend bool operator<(UvDist left, UvDist right)
  {
    return left.value_ < right.value_;
  }

 private:
  int32_t value_ = 0;
};

class Rect
{
 public:
  Rect() = default;
  Rect(UvDist xl, UvDist yl, UvDist xh, UvDist yh)
      : _xl(xl), _yl(yl), _xh(xh), _yh(yh)
  {
  }

  std::string toString() const
  {
    std::ostringstream out;
    out << '[' << _xl.getStorage() << ',' << _yl.getStorage() << ','
        << _xh.getStorage() << ',' << _yh.getStorage() << ']';
    return out.str();
  }

  UvDist getXL() const { return _xl; }
  UvDist getYL() const { return _yl; }
  UvDist getXH() const { return _xh; }
  UvDist getYH() const { return _yh; }

  UvDist _xl;
  UvDist _yl;
  UvDist _xh;
  UvDist _yh;
};

struct Point2D
{
};

enum class PhysOrientationE { R0, R90, R180, R270, MX, MY, MXR90, MYR90 };
using PhysOrientation = PhysOrientationE;

template <typename T>
class uvRIter
{
};

}  // namespace eUTL

using Rect = eUTL::Rect;

namespace eUNL {
class PhysBlockage;
class PhysRow;
class PhysCell;
class PhysCellImpl;
class PhysPin;
class PhysSWire;
class PhysShape;
class PhysDesMgr;
class Design;
using LeafCellID = int;
using LibCellID = int;
enum class PhysObjStatus { Unknown };
}  // namespace eUNL

namespace eLIB {
class TechLayer;
class TechShape;
class PhysLib;
class PhysLibObs;
class PhysLibCell;
class TechSite;
using TechLayerID = int;
using TechLayerRelativeID = int;
enum class ShapeUsageE { Unknown };
}  // namespace eLIB
