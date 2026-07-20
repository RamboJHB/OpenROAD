// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Shared base types for the filler VT overlay repair planner.
//
// The planner is a deterministic, non-mutating component (spec section 3.1): it
// depends only on PlannerDataSource plus the planner-internal oracle protocol in
// OracleGate.h. The only shared checker wire type is ipl::FillerChanges; test
// builds provide the same UDM ID/value types through their test-only UDM shim.
//
// Conventions:
//  - All x coordinates are DBU. Site alignment comes from
//    PlannerDataSource::siteWidth().
//  - All intervals are half-open [xl, xh).
//  - Row ranges in Region are inclusive [rowLo, rowHi].

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "drc/ImplantLayerChecker.h"

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
  bool contains(DbCoord x) const { return x >= xl && x < xh; }
  bool operator==(const XInterval& other) const
  {
    return xl == other.xl && xh == other.xh;
  }
};

// VT family identity. The planner only compares VT ids; it never interprets
// them -- rule semantics stay inside the checker (checker-as-oracle).
using VtId = int32_t;
inline constexpr VtId kUnknownVt = -1;

// Implant band polarity (checker: ipl::Layer::Polar, from the LAST '_' suffix of
// the implant layer name via parseLayerName). Each row is two half-row bands
// with alternating polarity; a master's VT FAMILY is uniform across its bands
// by checker construction (buildMasters: master_implant_family_mismatch), so
// the per-band degree of freedom is polarity only.
enum class BandPolarity : uint8_t
{
  N,
  P
};

// Placement orientation. The checker draft uses eUTL::PhysOrientation (a
// UDM type); the pure planner keeps this minimal enum and the runtime
// engine boundary maps between the two.
enum class Orient : uint8_t
{
  R0,
  R180,
  MX,
  MY
};

// Planner-side guard region. The wire-level checker API uses a geometric
// Rect; converting rows to y coordinates is the runtime boundary's concern, so the
// pure planner keeps the row-based form everywhere.
struct Region
{
  XInterval x;
  RowId rowLo = 0;
  RowId rowHi = -1;  // empty when rowHi < rowLo

  bool containsRow(RowId r) const { return r >= rowLo && r <= rowHi; }
};

enum class Severity
{
  Info,
  Warning,
  Error,
  Fatal
};

// Stable machine-readable code + human-readable message used inside the pure
// planner. Runtime converts these to final-checker ipl::Diagnostic.
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

// --- Infrastructure candidate query ---------------------------------------

struct MasterCandidateRequest
{
  InstanceId fillerInstanceId = 0;
};

struct MasterCandidate
{
  MasterId masterId = 0;
};

struct MasterCandidateResult
{
  std::vector<MasterCandidate> candidates;
  std::vector<Diagnostic> diagnostics;
};

// --- Shared planner model (spec sections 5.1 / 5.2) -------------------------

// Anchor: the std cell changed by upstream opto/ECO. Not a repair window.
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

// Mirrors the FINAL checker's ipl::Relationship exactly (IntraInstance was
// removed from the checker; do not reintroduce it here).
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

// Exact final-checker wire helpers. The record itself is deliberately not
// duplicated in fillerRepair: the planner, oracle and public result all carry
// ipl::FillerCellRecord unchanged.
inline InstanceId fillerRecordInstanceId(const FillerCellRecord& change)
{
  return static_cast<InstanceId>(change.cell_id_.getIndexValue());
}

inline MasterId fillerRecordNewMasterId(const FillerCellRecord& change)
{
  return static_cast<MasterId>(change.new_lib_cell_.getIndexValue());
}

inline bool sameFillerCellRecord(const FillerCellRecord& left,
                                 const FillerCellRecord& right)
{
  return left.op_ == right.op_ && left.cell_id_ == right.cell_id_
         && left.origin_x_ == right.origin_x_
         && left.origin_y_ == right.origin_y_
         && left.orig_lib_cell_ == right.orig_lib_cell_
         && left.new_lib_cell_ == right.new_lib_cell_;
}

// --- Planner entry types (spec section 5.4) --------------------------------

struct FillerRepairRequest
{
  TargetPlace targetPlace;
  std::vector<Violation> violations;  // initial snapshot from the checker
};

struct FillerRepairResult
{
  bool hasSolution = false;
  ipl::FillerChanges changes;
  std::vector<Diagnostic> diagnostics;
};

}  // namespace dpl2::fillerRepair
