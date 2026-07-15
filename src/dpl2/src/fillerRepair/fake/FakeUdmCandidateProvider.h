// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Fake-UDM master catalog + candidate provider.
//
// Purpose: rehearse the REAL adapter's data path before the real checker/infra
// APIs land. The real provider will sit on the checker's master tables, which
// are built from UDM by ImplantLayerChecker::buildMasters, parseLayerName and
// rebuildMasterShapes (formerly in the helper, folded into the checker by the
// 2026-07-12 update). This fake replicates that derivation UDM-free, with the
// same rules:
//
//   - implant layers are named "<FAMILY>_<POLARITY>"; family is one of
//     VTS/VTL/VTH/VTUL (case-insensitive), polarity P/p -> P, anything else N
//     (ImplantLayerCheckerHelper::parseLayerName);
//   - a master's VT is the FAMILY of the implant layers its shapes sit on --
//     NEVER parsed from the master's name (spec appendix A note). All shapes
//     of one master must share one family, or the master is unusable
//     (buildMasters: master_implant_family_mismatch);
//   - width is DBU and must be site-aligned (master_width_not_site_aligned);
//     implant shapes must span the full master width
//     (implant_shape_width_mismatch);
//   - height spans whole rows; each row carries two half-row band shapes with
//     alternating polarity (rebuildMasterShapes). Polarity does not affect the
//     derived VT, so describeMasters is polarity-agnostic.
//
// VtId mapping (pinned, documented): family enum index -- VTS=0, VTL=1,
// VTH=2, VTUL=3; underivable -> kUnknownVt.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "../Log.h"
#include "../PlacementView.h"
#include "FakeDesign.h"

namespace dpl2::fillerRepair {

// Structural mirrors of ipl::MasterShape / ipl::MasterInput, UDM-free (the
// eUTL::Rect is reduced to plain extents).
struct FakeUdmShape
{
  ShapeId shapeId = 0;
  LayerId layer = 0;
  DbCoord xl = 0;
  DbCoord yl = 0;
  DbCoord xh = 0;
  DbCoord yh = 0;
};

struct FakeUdmMaster
{
  MasterId masterId = 0;
  std::string name;  // human-readable only; never used for derivation
  DbCoord width = 0;
  DbCoord height = 0;
  bool isFiller = false;
  std::vector<FakeUdmShape> shapes;
};

// Result of the derivation for one master id: the "given master ids, return
// every id's width and VT type" query.
struct MasterDescription
{
  MasterId masterId = 0;
  std::string name;
  DbCoord width = 0;
  int heightRows = 0;
  VtId vt = kUnknownVt;  // family index (see header comment)
  bool isFiller = false;
  bool usable = false;   // derivation succeeded; unusable masters are never
                         // offered as swap candidates
  std::string reason;    // checker-style code when unusable, empty otherwise
};

class FakeUdmCandidateProvider
{
 public:
  // `view` resolves instances for the candidate query (spec 5.3 is keyed by
  // filler INSTANCE); the catalog itself is master-only.
  FakeUdmCandidateProvider(DbCoord siteWidth, DbCoord rowHeight)
      : site_width_(siteWidth), row_height_(rowHeight)
  {
  }

  // --- catalog building (mirrors ImplantInput.layers / .masters) -----------
  void addLayer(LayerId id, const std::string& name);
  void addMaster(const FakeUdmMaster& master);
  // Convenience: a single-row master carrying the two canonical band shapes
  // (bottom on the family's N layer, top on its P layer) exactly as
  // rebuildMasterShapes would emit them. Layers of `family` must exist.
  void addBandMaster(MasterId id,
                     const std::string& name,
                     DbCoord width,
                     bool isFiller,
                     const std::string& family);

  // --- the requested query --------------------------------------------------
  // One description per input id, input order preserved; unknown ids yield
  // usable=false with reason "unknown_master".
  std::vector<MasterDescription> describeMasters(
      const std::vector<MasterId>& ids) const;
  // nullptr when the id is not in the catalog.
  const MasterDescription* describeMaster(MasterId id) const;

  // --- engine contract (spec 5.3) -------------------------------------------
  // Same width + height, usable filler masters, current master excluded,
  // ascending master id. Non-filler input / no replacement -> diagnostics,
  // never an error.

  // Sync every usable master into a FakeDesign so the engine's PlacementView
  // and this provider agree on width/height/vt (the engine validates each
  // candidate against view.masterInfo when constructing Swaps).
  void registerInto(FakeDesign& design) const;

  // Appendix-A library: F_FILL{8,4,3,2}_63S6T9{R,L,UL}_1 on layers
  // {VTS,VTL,VTUL}_{N,P}, widths in sites * siteWidth. Suffix mapping
  // R->VTS, L->VTL, UL->VTUL is an assumption pending library-team
  // confirmation (spec appendix A); derivation still goes through the
  // implant layers, the names are decoration.
  void addAppendixALibrary();

 private:
  struct LayerInfo
  {
    LayerId id = 0;
    std::string name;
    int familyIndex = -1;  // -1 = Unknown
    bool polarityP = false;
  };

  const LayerInfo* layer(LayerId id) const;
  MasterDescription derive(const FakeUdmMaster& master) const;

  DbCoord site_width_ = 1;
  DbCoord row_height_ = 1;
  std::map<LayerId, LayerInfo> layers_;              // ordered: deterministic
  std::map<MasterId, FakeUdmMaster> masters_;        // ordered: deterministic
  std::map<MasterId, MasterDescription> described_;  // derived on addMaster
};

// parseLayerName replica (ImplantLayerCheckerHelper.cpp): split at the LAST
// '_'; family index VTS=0 VTL=1 VTH=2 VTUL=3, unknown -> -1; polarity "P"/"p"
// -> P, anything else N. Exposed for tests.
void parseFakeUdmLayerName(const std::string& name,
                           int& familyIndex,
                           bool& polarityP);

}  // namespace dpl2::fillerRepair
