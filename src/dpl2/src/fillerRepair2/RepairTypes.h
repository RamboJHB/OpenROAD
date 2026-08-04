// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The vocabulary: ids, geometry, what a violation is, what a diagnostic is,

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <infrastructure/Objects.h>

#include <drc/ImplantLayerChecker.h>

namespace dpl2::fillerRepair {

using DbCoord = int64_t;
using LayerId = int32_t;
using MasterId = int32_t;
using InstanceId = int32_t;
using ShapeId = int32_t;
using RowId = int32_t;

// Half-open interval [xl, xh).
struct XInterval
{
  DbCoord xl = 0;
  DbCoord xh = 0;

  DbCoord length() const { return xh - xl; }
  bool empty() const { return xh <= xl; }
  bool overlaps(const XInterval& other) const
  {
    return xl < other.xh && other.xl < xh;
  }
  bool operator==(const XInterval& other) const
  {
    return xl == other.xl && xh == other.xh;
  }
};

// Which VT family a master belongs to. The search only ever asks "same or
using VtId = int32_t;
inline constexpr VtId kUnknownVt = -1;

// N or P. Every row is two half-height bands, and the polarity alternates
enum class BandPolarity : uint8_t
{
  N,
  P
};

// Placement orientation. UDM has its own richer type; the search keeps this
enum class Orient : uint8_t
{
  R0,
  R180,
  MX,
  MY
};

// A rectangle of design, counted in ROWS rather than y coordinates -- rows
struct Region
{
  XInterval x;
  RowId rowLo = 0;
  RowId rowHi = -1;  // empty when rowHi < rowLo

  bool containsRow(RowId r) const { return r >= rowLo && r <= rowHi; }
  bool operator==(const Region& other) const
  {
    return x.xl == other.x.xl && x.xh == other.x.xh && rowLo == other.rowLo
           && rowHi == other.rowHi;
  }
};

enum class Severity
{
  Info,
  Warning,
  Error,
  Fatal
};

// Why something happened, in two parts: a `code` that is stable enough for a
struct Diagnostic
{
  Severity severity = Severity::Info;
  std::string code;
  std::string message;
};

inline Diagnostic makeDiag(Severity severity, std::string code, std::string message)
{
  return Diagnostic{severity, std::move(code), std::move(message)};
}

// --- what the search works on -----------------------------------------------

// The cell opto retargeted: same instance, same site, new master. Everything
struct TargetPlace
{
  InstanceId instanceId = 0;
  MasterId masterId = 0;  // new/candidate std-cell master
  RowId rowId = 0;
  DbCoord x = 0;
  Orient orientation = Orient::R0;
};

enum class ViolationKind
{
  MinWidth,
  MinSpacing
};

// Whether the two things in conflict sit in the same row or across a row
enum class ViolationRelation
{
  IntraRow,
  InterRow
};

struct ViolationParticipant
{
  InstanceId instanceId = 0;
  MasterId masterId = 0;
  RowId rowId = 0;
  XInterval xRange;
  bool isFiller = false;
  bool isTarget = false;
};

struct Violation
{
  int ruleId = 0;
  ViolationKind kind = ViolationKind::MinWidth;
  ViolationRelation relation = ViolationRelation::IntraRow;

  LayerId primaryLayer = 0;
  std::optional<LayerId> secondaryLayer;

  std::vector<RowId> rowIds;  // sorted unique; inter-row lists all touched rows
  XInterval xWindow;
  DbCoord measuredValue = 0;
  DbCoord requiredValue = 0;

  std::vector<ViolationParticipant> participants;
};

// Readers for the shared change record. Repair only ever emits Replace with a
inline const eUNL::LeafCellID* cellChangeRecordLeafCellId(
    const CellChangeRecord& change)
{
  return std::get_if<eUNL::LeafCellID>(&change.cell_data_);
}

inline InstanceId cellChangeRecordInstanceId(const CellChangeRecord& change)
{
  const eUNL::LeafCellID* cellId = cellChangeRecordLeafCellId(change);
  return cellId != nullptr
             ? static_cast<InstanceId>(cellId->getIndexValue())
             : static_cast<InstanceId>(-1);
}

inline MasterId cellChangeRecordNewMasterId(const CellChangeRecord& change)
{
  return static_cast<MasterId>(change.new_lib_cell_.getIndexValue());
}

inline bool sameCellChangeRecord(const CellChangeRecord& left,
                                 const CellChangeRecord& right)
{
  return left.op_ == right.op_ && left.cell_data_ == right.cell_data_
         && left.origin_x_ == right.origin_x_
         && left.origin_y_ == right.origin_y_
         && left.orig_lib_cell_ == right.orig_lib_cell_
         && left.new_lib_cell_ == right.new_lib_cell_
         && left.orientation_.getValue() == right.orientation_.getValue();
}

// --- Planner entry types ----------------------------------------------------

// In: the retargeted cell, and what the checker said about it.
struct FillerRepairRequest
{
  TargetPlace targetPlace;
  std::vector<Violation> violations;
};

// Out: the filler swaps that make it legal -- checker-verified, or empty.
struct FillerRepairResult
{
  bool hasSolution = false;
  ipl::FillerChanges changes;
  std::vector<Diagnostic> diagnostics;
};

}  // namespace dpl2::fillerRepair
