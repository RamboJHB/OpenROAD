// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <fillerRepair/test/SyntheticMasterCatalog.h>

#include <algorithm>
#include <utility>

namespace dpl2::fillerRepair {

void parseSyntheticLayerName(const std::string& name,
                           int& familyIndex,
                           bool& polarityP)
{
  familyIndex = -1;
  polarityP = false;

  const auto pos = name.rfind('_');
  if (pos == std::string::npos) {
    return;
  }
  const std::string famStr = name.substr(0, pos);
  const std::string polStr = name.substr(pos + 1);

  polarityP = (polStr == "P" || polStr == "p");

  if (famStr == "VTS" || famStr == "vts") {
    familyIndex = 0;
  } else if (famStr == "VTL" || famStr == "vtl") {
    familyIndex = 1;
  } else if (famStr == "VTH" || famStr == "vth") {
    familyIndex = 2;
  } else if (famStr == "VTUL" || famStr == "vtul") {
    familyIndex = 3;
  }
}

void SyntheticMasterCatalog::addLayer(LayerId id, const std::string& name)
{
  LayerInfo info;
  info.id = id;
  info.name = name;
  parseSyntheticLayerName(name, info.familyIndex, info.polarityP);
  layers_[id] = info;
}

const SyntheticMasterCatalog::LayerInfo* SyntheticMasterCatalog::layer(
    LayerId id) const
{
  const auto it = layers_.find(id);
  return it != layers_.end() ? &it->second : nullptr;
}

// Derivation mirror of ImplantLayerChecker::buildMasters: width alignment,
// per-shape layer lookup, single-family requirement, full-width span. The
// first failed rule is recorded as the checker-style reason.
MasterDescription SyntheticMasterCatalog::derive(
    const SyntheticMaster& master) const
{
  MasterDescription d;
  d.masterId = master.masterId;
  d.name = master.name;
  d.width = master.width;
  d.isFiller = master.isFiller;
  d.heightRows = row_height_ > 0
                     ? static_cast<int>((master.height + row_height_ - 1)
                                        / row_height_)
                     : 0;

  if (site_width_ <= 0 || master.width <= 0
      || master.width % site_width_ != 0) {
    d.reason = "master_width_not_site_aligned";
    return d;
  }
  if (master.shapes.empty()) {
    d.reason = "no_implant_shape";
    return d;
  }

  int familyIndex = -1;
  const SyntheticMasterShape* bottom = nullptr;
  for (const SyntheticMasterShape& shape : master.shapes) {
    const LayerInfo* info = layer(shape.layer);
    if (info == nullptr || info->familyIndex < 0) {
      d.reason = "skipped_missing_rule_parameter";  // unknown implant layer
      return d;
    }
    if (familyIndex < 0) {
      familyIndex = info->familyIndex;
    } else if (familyIndex != info->familyIndex) {
      d.reason = "master_implant_family_mismatch";
      return d;
    }
    if (shape.xl != 0 || shape.xh != master.width) {
      d.reason = "implant_shape_width_mismatch";
      return d;
    }
    if (bottom == nullptr || shape.yl < bottom->yl) {
      bottom = &shape;
    }
  }

  d.vt = familyIndex;
  // Band anchor exactly like rebuildMasterShapes: the bottommost shape's
  // layer polarity is the master's R0-frame bottom band.
  if (bottom != nullptr) {
    d.bottomBandPolarity =
        layer(bottom->layer)->polarityP ? BandPolarity::P : BandPolarity::N;
  }
  d.usable = true;
  return d;
}

void SyntheticMasterCatalog::addMaster(const SyntheticMaster& master)
{
  masters_[master.masterId] = master;
  described_[master.masterId] = derive(master);
}

void SyntheticMasterCatalog::addBandMaster(MasterId id,
                                             const std::string& name,
                                             DbCoord width,
                                             bool isFiller,
                                             const std::string& family)
{
  // Find the family's N and P layers (rebuildMasterShapes needs both).
  int familyIndex = -1;
  bool polarityP = false;
  parseSyntheticLayerName(family + "_N", familyIndex, polarityP);
  LayerId nLayer = -1;
  LayerId pLayer = -1;
  for (const auto& [layerId, info] : layers_) {
    if (familyIndex >= 0 && info.familyIndex == familyIndex) {
      (info.polarityP ? pLayer : nLayer) = layerId;
    }
  }

  SyntheticMaster master;
  master.masterId = id;
  master.name = name;
  master.width = width;
  master.height = row_height_;
  master.isFiller = isFiller;
  const DbCoord halfRow = row_height_ / 2;
  // Canonical single-row band pair: bottom band on the N layer, top band on
  // the P layer, both spanning the full width (rebuildMasterShapes).
  master.shapes.push_back(SyntheticMasterShape{0, nLayer, 0, 0, width, halfRow});
  master.shapes.push_back(
      SyntheticMasterShape{1, pLayer, 0, halfRow, width, row_height_});
  addMaster(master);
}

const MasterDescription* SyntheticMasterCatalog::describeMaster(
    MasterId id) const
{
  const auto it = described_.find(id);
  return it != described_.end() ? &it->second : nullptr;
}

std::vector<MasterDescription> SyntheticMasterCatalog::describeMasters(
    const std::vector<MasterId>& ids) const
{
  std::vector<MasterDescription> result;
  result.reserve(ids.size());
  for (const MasterId id : ids) {
    if (const MasterDescription* d = describeMaster(id)) {
      result.push_back(*d);
    } else {
      MasterDescription missing;
      missing.masterId = id;
      missing.reason = "unknown_master";
      result.push_back(missing);
    }
  }
  return result;
}

void SyntheticMasterCatalog::registerInto(TestPlacementView& design) const
{
  std::vector<MasterId> fillerIds;
  for (const auto& [id, d] : described_) {
    if (d.usable) {
      design.addMaster(id, d.width, d.heightRows, d.isFiller, d.vt,
                       d.bottomBandPolarity);
      if (d.isFiller) fillerIds.push_back(id);
    }
  }
  design.setFillerMasterIds(std::move(fillerIds));
}

void SyntheticMasterCatalog::addAppendixALibrary()
{
  // Layers first: {VTS, VTL, VTUL} x {N, P} with deterministic ids.
  addLayer(1, "VTS_N");
  addLayer(2, "VTS_P");
  addLayer(3, "VTL_N");
  addLayer(4, "VTL_P");
  addLayer(5, "VTUL_N");
  addLayer(6, "VTUL_P");

  // F_FILL{8,4,3,2}_63S6T9{R,L,UL}_1; master id = width-in-sites * 10 +
  // family index (VTS=0, VTL=1, VTUL=3) -- deterministic and readable.
  const std::pair<const char*, const char*> suffixToFamily[] = {
      {"R", "VTS"}, {"L", "VTL"}, {"UL", "VTUL"}};
  for (const int widthSites : {2, 3, 4, 8}) {
    for (const auto& [suffix, family] : suffixToFamily) {
      int familyIndex = -1;
      bool polarityP = false;
      parseSyntheticLayerName(std::string(family) + "_N", familyIndex,
                            polarityP);
      const MasterId id = widthSites * 10 + familyIndex;
      addBandMaster(id,
                    cat("F_FILL", widthSites, "_63S6T9", suffix, "_1"),
                    widthSites * site_width_,
                    /*isFiller=*/true,
                    family);
    }
  }
}

}  // namespace dpl2::fillerRepair
