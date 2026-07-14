// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <ostream>
#include <tuple>

#include "boost/operators.hpp"
#include "dpl2/DePlace.h"

// UDM
#include <phys/fpManager.hh>
#include <phys/physDesMgr.hh>
#include <phys/physHier.hh>
#include <physHierImpl.hh>
#include <util/iter.hh>

namespace dpl2 {

// Strongly type the difference between pixel and DBU locations.
//
// Multiplication and division are intentionally not defined as they
// are often, but not always, used to convert between DBUs and pixels
// and such operations must be explicit about the resulting type.

template <typename T>
struct TypedCoordinate : public boost::totally_ordered<TypedCoordinate<T>>,
                         public boost::totally_ordered<TypedCoordinate<T>, int>,
                         public boost::additive<TypedCoordinate<T>>,
                         public boost::additive<TypedCoordinate<T>, int>,
                         public boost::modable<TypedCoordinate<T>>,
                         public boost::modable<TypedCoordinate<T>, int>,
                         public boost::incrementable<TypedCoordinate<T>>,
                         public boost::decrementable<TypedCoordinate<T>>,
                         public boost::dividable<TypedCoordinate<T>>,
                         public boost::dividable<TypedCoordinate<T>, int>,
                         public boost::multipliable<TypedCoordinate<T>>,
                         public boost::multipliable<TypedCoordinate<T>, int>
{
    explicit TypedCoordinate(const int v = 0) : v(v) {}

    // totally ordered
    bool operator<(const TypedCoordinate& rhs) const { return v < rhs.v; }
    bool operator<(const int rhs) const { return v < rhs; }
    bool operator>(const int rhs) const { return v > rhs; }
    bool operator==(const TypedCoordinate& rhs) const { return v == rhs.v; }
    bool operator==(const int rhs) const { return v == rhs; }
    // additive
    TypedCoordinate& operator+=(const TypedCoordinate& rhs)
    {
        v += rhs.v;
        return *this;
    }
    TypedCoordinate& operator+=(const int rhs)
    {
        v += rhs;
        return *this;
    }
    TypedCoordinate& operator-=(const TypedCoordinate& rhs)
    {
        v -= rhs.v;
        return *this;
    }
    TypedCoordinate& operator-=(const int rhs)
    {
        v -= rhs;
        return *this;
    }
    // modable
    TypedCoordinate& operator%=(const TypedCoordinate& rhs)
    {
        v %= rhs.v;
        return *this;
    }

    // incrementable
    TypedCoordinate& operator++()
    {
        ++v;
        return *this;
    }

    // decrementable
    TypedCoordinate& operator--()
    {
        --v;
        return *this;
    }
    // dividable
    TypedCoordinate& operator/=(const TypedCoordinate& rhs)
    {
        v /= rhs.v;
        return *this;
    }
    TypedCoordinate& operator/=(const int rhs)
    {
        v /= rhs;
        return *this;
    }
    // multipliable
    TypedCoordinate& operator*=(const TypedCoordinate& rhs)
    {
        v *= rhs.v;
        return *this;
    }
    TypedCoordinate& operator*=(const int rhs)
    {
        v *= rhs;
        return *this;
    }

    int v;
};

template <typename T>
inline std::ostream& operator<<(std::ostream& o, const TypedCoordinate<T>& tc)
{
    return o << tc.v;
}

template <typename T>
TypedCoordinate<T> abs(const TypedCoordinate<T>& val)
{
    return TypedCoordinate<T>{std::abs(val.v)};
}

struct GridPt
{
    GridPt() = default;
    GridPt(GridX x, GridY y) : x(x), y(y) {}
    bool operator==(const GridPt& p) const { return (x == p.x) && (y == p.y); }
    GridX x{0};
    GridY y{0};
};

struct GridRect
{
    GridX xlo;
    GridY ylo;
    GridX xhi;
    GridY yhi;

    GridRect intersect(const GridRect& r) const;
    GridPt closestPtInside(GridPt pt) const;
};

inline GridPt GridRect::closestPtInside(GridPt pt) const
{
    int closest_x = std::min(std::max(pt.x.v, this->xlo.v), this->xhi.v);
    int closest_y = std::min(std::max(pt.y.v, this->ylo.v), this->yhi.v);

    return {GridX{closest_x}, GridY{closest_y}};
}

inline GridRect GridRect::intersect(const GridRect& r) const
{
    GridRect result;
    result.xlo = std::max(xlo, r.xlo);
    result.ylo = std::max(ylo, r.ylo);
    result.xhi = std::min(xhi, r.xhi);
    result.yhi = std::min(yhi, r.yhi);
    return result;
}

struct DbuPt
{
    DbuPt() = default;
    DbuPt(DbuX x, DbuY y) : x(x), y(y) {}
    DbuX x{0};
    DbuY y{0};
};

struct DbuRect
{
    DbuRect(const eUTL::Rect& rect)
        : xl(rect.getXL().getStorage()), yl(rect.getYL().getStorage()),
          xh(rect.getXH().getStorage()), yh(rect.getYH().getStorage())
    {}
    DbuX dx() const { return xh - xl; }
    DbuY dy() const { return yh - yl; }
    eUTL::Rect getRect()
    {
        return eUTL::Rect(UvDist(xl.v), UvDist(yl.v), UvDist(xh.v), UvDist(yh.v));
    }

    DbuRect &expand(const DbuRect& rect)
    {
        if (xl > rect.xl) {
            xl = rect.xl;
        }
        if (xh < rect.xh) {
            xh = rect.xh;
        }
        if (yl > rect.yl) {
            yl = rect.yl;
        }
        if (yh < rect.yh) {
            yh = rect.yh;
        }
        return (*this);
    }

    DbuX xl{0};
    DbuY yl{0};
    DbuX xh{0};
    DbuY yh{0};
};

inline bool operator==(const DbuPt& p1, const DbuPt& p2)
{
    return std::tie(p1.x, p1.y) == std::tie(p2.x, p2.y);
}

inline DbuX gridToDbu(GridX x, DbuX scale)
{
    return DbuX{x.v * scale.v};
}

inline DbuY gridToDbu(GridY y, DbuY scale)
{
    return DbuY{y.v * scale.v};
}

static int divRound(const int dividend, const int divisor)
{
    return round(static_cast<double>(dividend) / divisor);
}

static int divCeil(const int dividend, const int divisor)
{
    return ceil(static_cast<double>(dividend) / divisor);
}

static int divFloor(const int dividend, const int divisor)
{
    return dividend / divisor;
}

inline GridX dbuToGridCeil(DbuX x, DbuX divisor)
{
    return GridX{divCeil(x.v, divisor.v)};
}

inline GridX dbuToGridFloor(DbuX x, DbuX divisor)
{
    return GridX{divFloor(x.v, divisor.v)};
}

inline GridY dbuToGridCeil(DbuY y, DbuY divisor)
{
    return GridY{divCeil(y.v, divisor.v)};
}

inline GridY dbuToGridFloor(DbuY y, DbuY divisor)
{
    return GridY{divFloor(y.v, divisor.v)};
}

inline int sumXY(DbuX x, DbuY y)
{
    return x.v + y.v;
}
} // namespace dpl2

// Enable use with unordered map/set
namespace std {

template <typename T>
struct hash<dpl2::TypedCoordinate<T>>
{
    std::size_t operator()(const dpl2::TypedCoordinate<T>& tc) const noexcept
    {
        return std::hash<int>()(tc.v);
    }
};

template <>
struct hash<dpl2::GridPt>
{
    std::size_t operator()(const dpl2::GridPt& p) const
    {
        size_t hashX = std::hash<dpl2::GridX>{}(p.x);
        size_t hashY = std::hash<dpl2::GridY>{}(p.y);
        return hashX ^ (hashY + 0x9e3779b9 + (hashX << 6) + (hashX >> 2));
    }
};

// Partial specialization for all TypedCoordinate<T>
template <typename T>
struct numeric_limits<dpl2::TypedCoordinate<T>>
{
    static constexpr bool is_specialized = true;

    static constexpr dpl2::TypedCoordinate<T> min() noexcept
    {
        return dpl2::TypedCoordinate<T>{numeric_limits<int>::min()};
    }

    static constexpr dpl2::TypedCoordinate<T> max() noexcept
    {
        return dpl2::TypedCoordinate<T>{numeric_limits<int>::max()};
    }

    static constexpr dpl2::TypedCoordinate<T> lowest() noexcept
    {
        return dpl2::TypedCoordinate<T>{numeric_limits<int>::lowest()};
    }

    // Mirror numeric_limits<int> properties
    static constexpr bool is_signed = numeric_limits<int>::is_signed;
    static constexpr bool is_integer = numeric_limits<int>::is_integer;
    static constexpr bool is_exact = numeric_limits<int>::is_exact;
    static constexpr int digits = numeric_limits<int>::digits;
    // Add other members as needed...
};

} // namespace std
