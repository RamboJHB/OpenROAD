// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Shared base types for the filler VT overlay repair planner.
//
// The planner is a pure, deterministic component (spec section 3.1): it
// depends only on PlacementView plus the planner-internal oracle protocol in
// OracleGate.h, never on UDM or the real checker headers. Test fakes implement
// that protocol; the production FillerRepairEngine translates it privately.
//
// Conventions:
//  - All x coordinates are DBU. Site alignment comes from
//    PlacementView::siteWidth().
//  - All intervals are half-open [xl, xh).
//  - Row ranges in Region are inclusive [rowLo, rowHi].

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// Implant band polarity (checker: ipl::Polarity, from the LAST '_' suffix of
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
// UDM type); the pure planner keeps this minimal enum and the production
// engine boundary maps between the two.
enum class Orient : uint8_t
{
  R0,
  R180,
  MX,
  MY
};

// Planner-side guard region. The wire-level checker API uses a geometric
// Rect; converting rows to y coordinates is the production boundary's concern, so the
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
// planner. Production converts these to final-checker ipl::Diagnostic.
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

// --- Wire types shared with the checker (spec section 5.1 / 5.2) -----------

// Anchor: the std cell changed by upstream opto/ECO. Not a repair window.
struct TargetPlace
{
  InstanceId instanceId = 0;
  MasterId masterId = 0;  // new/candidate std-cell master
  RowId rowId = 0;
  DbCoord x = 0;
  Orient orientation = Orient::R0;
};

// V1 wire format: same-size master swap on one filler instance.
struct FillerChange
{
  InstanceId instanceId = 0;
  MasterId newMasterId = 0;
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

using OverlayRequestId = int32_t;

struct OverlayCheckRequest
{
  OverlayRequestId requestId = -1;  // planner-generated, unique per batch
  TargetPlace targetPlace;
  Region guardRegion;  // repair window expanded by a two-cell guard halo
  std::vector<FillerChange> fillerChanges;  // one atomic overlay candidate
};

enum class CheckStatus
{
  Checked,
  InvalidOverlay,
  CheckerError
};

struct CheckResult
{
  OverlayRequestId requestId = -1;  // must echo OverlayCheckRequest.requestId
  CheckStatus status = CheckStatus::CheckerError;

  bool isLegal = false;  // meaningful only when status == Checked

  std::vector<Violation> violations;
  std::vector<Diagnostic> diagnostics;
};

// --- Planner entry types (spec section 5.4) --------------------------------

struct FillerRepairRequest
{
  TargetPlace targetPlace;
  std::vector<Violation> violations;  // initial snapshot from the checker
};

struct FillerRepairResult
{
  bool hasSolution = false;
  std::vector<FillerChange> changes;
  std::vector<Diagnostic> diagnostics;
};

}  // namespace dpl2::fillerRepair
