// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The vocabulary: ids, geometry, what a violation is, what a diagnostic is,
// and what goes in and out of a repair. Pure data -- no logic beyond trivial
// accessors, and it depends on nothing else in the module, which is why
// everything else can include it.
//
// The one type we do NOT define here is the change record itself. That is
// dpl2::CellChangeRecord, owned by infrastructure, and it travels unchanged
// from the search through the checker to the caller -- one representation, so
// there is no second copy of it to drift.
//
// Conventions used everywhere below:
//  - x is in DBU. Sites come from PlacementView::siteWidth().
//  - x intervals are half-open: [xl, xh).
//  - row ranges are inclusive: [rowLo, rowHi].
//
// ---------------------------------------------------------------------------
// PORTING TAGS
//
// Three tags mark everything a destination has to make a decision about.
// One grep finds all of them:
//
//     grep -rn "\[PORT-" <srcroot>/fillerRepair
//
//   [PORT-DROP]   You do not need this. Each one says what it costs to keep
//                 and what breaks if you delete it -- which is nothing in
//                 runtime path; these exist for the repository-local
//                 regression, which does not travel.
//
//   [PORT-ADAPT]  This will not compile or will behave wrongly until you
//                 change it. Each one names the destination-side thing it
//                 depends on. Work through every ADAPT before the first run.
//
//   [PORT-TUNE]   A number or a strategy chosen from measurements taken
//                 HERE, on a synthetic oracle. Each one says what to measure
//                 on your hardware before changing it. Safe to ship as-is;
//                 revisit if repair shows up in a profile.
//
// DROP and TUNE are optional. ADAPT is not.
// ---------------------------------------------------------------------------

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// dpl2::CellChangeRecord is used by value below, so include its owning header
// directly instead of relying on the checker header to carry it transitively:
// drc/DRCChecker.h reaches dpl2/DePlace.h, which only forward-declares it.
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
// different?" -- what a VT means, and which rules it triggers, is the
// checker's business, not ours.
using VtId = int32_t;
inline constexpr VtId kUnknownVt = -1;

// N or P. Every row is two half-height bands, and the polarity alternates
// from band to band up the design -- which is why a cell's orientation
// matters: flipping it swaps which band its N implant lands on.
//
// A master's VT family is the same on both of its bands (the checker enforces
// that), so polarity is the only thing that varies within one master.
enum class BandPolarity : uint8_t
{
  N,
  P
};

// Placement orientation. UDM has its own richer type; the search keeps this
// four-value enum so it stays database-free, and the engine maps between them
// at the boundary.
enum class Orient : uint8_t
{
  R0,
  R180,
  MX,
  MY
};

// A rectangle of design, counted in ROWS rather than y coordinates -- rows
// are what the search reasons about. The checker API wants a geometric Rect;
// turning rows into y is the engine's job at the boundary.
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
// test or a log filter to match on, and a `message` for a human. The engine
// converts these to the checker's own diagnostic type on the way out.
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
// the search does is anchored to this, hence the name used for it throughout.
// Note this is a single cell, not a region.
struct TargetPlace
{
  InstanceId instanceId = 0;
  MasterId masterId = 0;  // new/candidate std-cell master
  RowId rowId = 0;
  DbCoord x = 0;
  Orient orientation = Orient::R0;
  OpType operation = OpType::Replace;
};

enum class ViolationKind
{
  MinWidth,
  MinSpacing
};

// Whether the two things in conflict sit in the same row or across a row
// boundary. Mirrors the checker's own enum exactly -- keep it that way.
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

// Readers for the shared change record. Replace/Delete use a real LeafCellID;
// Add uses a request-local name, so the id accessor intentionally returns -1
// for Add rather than manufacturing a plausible instance id.
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
         && left.x_ == right.x_
         && left.y_ == right.y_
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

// Out: the atomic filler edits that make the target transaction legal --
// checker-verified, or empty. Existing filler VT changes are Replace records;
// target Delete produces filler Adds and target Add produces filler Deletes
// plus any Adds needed to refill collateral area.
// `hasSolution` with no changes means there was nothing to fix.
struct FillerRepairResult
{
  bool hasSolution = false;
  ipl::FillerChanges changes;
  std::vector<Diagnostic> diagnostics;
};

}  // namespace dpl2::fillerRepair
