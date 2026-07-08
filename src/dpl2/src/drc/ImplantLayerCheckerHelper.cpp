#include "ImplantLayerCheckerHelper.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>

namespace dpl2 {
namespace ipl {

// -----------------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------------
ImplantLayerCheckerHelper::ImplantLayerCheckerHelper()
{
}

// -----------------------------------------------------------------------------
// Parse layer name to extract family and polarity
// Expected format: "<FAMILY>_<POLARITY>" e.g. "VTUL_N", "VTL_P", "VTH_N"
// -----------------------------------------------------------------------------
void ImplantLayerCheckerHelper::parseLayerName(const std::string& name,
                                               Family& family,
                                               Polarity& polarity)
{
    family = Family::Unknown;
    polarity = Polarity::N;

    auto pos = name.rfind('_');
    if (pos == std::string::npos) {
        return;
    }

    std::string famStr = name.substr(0, pos);
    std::string polStr = name.substr(pos + 1);

    polarity = ((polStr == "P" || polStr == "p") ? Polarity::P : Polarity::N);

    if (famStr == "VTS" || famStr == "vts") {
        family = Family::VTS;
    } else if (famStr == "VTL" || famStr == "vtl") {
        family = Family::VTL;
    } else if (famStr == "VTH" || famStr == "vth") {
        family = Family::VTH;
    } else if (famStr == "VTUL" || famStr == "vtul") {
        family = Family::VTUL;
    } else {
        family = Family::Unknown;
    }
}

LayerId ImplantLayerCheckerHelper::findLayerId(eLIB::TechLayerRelativeID relId) const
{
    auto it = techLayerToCheckerId_.find(relId);
    return (it != techLayerToCheckerId_.end()) ? it->second : -1;
}

void ImplantLayerCheckerHelper::addDiag(const std::string& msg)
{
    diags_.push_back(msg);
}

// -----------------------------------------------------------------------------
// Rebuild master shapes into canonical band-level shapes.
// For each row the master spans, produces 2 shapes (bottom band, top band),
// each spanning the full master width with height = rowHeight / 2.
// The polarity pattern per row is determined by the bottommost raw shape:
//   - If bottom raw shape polarity is N:  Row0 = N-P, Row1 = P-N, Row2 = N-P...
//   - If bottom raw shape polarity is P:  Row0 = P-N, Row1 = N-P, Row2 = P-N...
// Layer is mapped to the family-appropriate N/P layer.
// Raw shapes are preserved in rawShapes.
// -----------------------------------------------------------------------------
void ImplantLayerCheckerHelper::rebuildMasterShapes()
{
    for (auto& master : data_.masters) {
        if (master.rawShapes.empty()) {
            master.shapes.clear();
            continue;
        }

        if (master.siteHeight <= 0) {
            addDiag("skipped rebuild no_site_height: master "
                    + std::to_string(master.masterId));
            master.shapes = master.rawShapes;
            continue;
        }

        // const DbCoord fullRow = master.siteHeight;
        const DbCoord fullRow = data_.rowHeight;
        const DbCoord halfRow = fullRow / 2;

        // Determine the number of rows this master spans
        int numRows = static_cast<int>((master.height + fullRow - 1) / fullRow);
        if (numRows < 1) numRows = 1;

        // Determine the family from raw shapes (all should be the same family)
        Family family = Family::Unknown;
        for (const auto& rs : master.rawShapes) {
            auto layerIt = std::find_if(data_.layers.begin(), data_.layers.end(),
                                        [&](const ImplantLayer& l) { return l.id == rs.layer; });
            if (layerIt != data_.layers.end()) {
                family = layerIt->family;
                break;
            }
        }
        if (family == Family::Unknown) {
            addDiag("skipped rebuild unknown_family: master "
                    + std::to_string(master.masterId));
            master.shapes = master.rawShapes;
            continue;
        }

        // Determine the base band polarity from the bottommost raw shape
        // Find the raw shape with the minimum y1 (bottommost)
        DbCoord minY = std::numeric_limits<DbCoord>::max();
        Polarity bottomPolarity = Polarity::N;
        for (const auto& rs : master.rawShapes) {
            DbCoord y1 = rs.rect._yl.getStorage();
            if (y1 < minY) {
                minY = y1;
                auto layerIt = std::find_if(data_.layers.begin(), data_.layers.end(),
                                            [&](const ImplantLayer& l) { return l.id == rs.layer; });
                if (layerIt != data_.layers.end()) {
                    bottomPolarity = layerIt->polarity;
                }
            }
        }

        // Find N and P layer IDs for this family
        LayerId familyNLayer = -1;
        LayerId familyPLayer = -1;
        for (const auto& l : data_.layers) {
            if (l.family == family) {
                if (l.polarity == Polarity::N) familyNLayer = l.id;
                else familyPLayer = l.id;
            }
        }
        if (familyNLayer < 0 || familyPLayer < 0) {
            addDiag("skipped rebuild missing_layer: master "
                    + std::to_string(master.masterId)
                    + " family missing N or P layer");
            master.shapes = master.rawShapes;
            continue;
        }

        // Build 2*numRows shapes
        // Polarity pattern per row: if bottomPolarity == N, row0 = N-P, row1 = P-N, ...
        //                           if bottomPolarity == P, row0 = P-N, row1 = N-P, ...
        master.shapes.clear();
        ShapeId shapeId = 0;
        for (int row = 0; row < numRows; ++row) {
            Polarity bottomBandPol, topBandPol;
            if (row % 2 == 0) {
                // Even row: base polarity on bottom
                bottomBandPol = bottomPolarity;
                topBandPol = (bottomPolarity == Polarity::N) ? Polarity::P : Polarity::N;
            } else {
                // Odd row: flip
                bottomBandPol = (bottomPolarity == Polarity::N) ? Polarity::P : Polarity::N;
                topBandPol = bottomPolarity;
            }

            // Bottom band shape
            {
                MasterShape ms;
                ms.shapeId = shapeId++;
                ms.layer = (bottomBandPol == Polarity::N) ? familyNLayer : familyPLayer;
                DbCoord yBase = static_cast<DbCoord>(row) * fullRow;
                ms.rect = eUTL::Rect(
                    eUTL::UvDist(static_cast<int64_t>(0)),
                    eUTL::UvDist(yBase),
                    eUTL::UvDist(master.width),
                    eUTL::UvDist(yBase + halfRow));
                master.shapes.push_back(ms);
            }

            // Top band shape
            {
                MasterShape ms;
                ms.shapeId = shapeId++;
                ms.layer = (topBandPol == Polarity::N) ? familyNLayer : familyPLayer;
                DbCoord yBase = static_cast<DbCoord>(row) * fullRow + halfRow;
                ms.rect = eUTL::Rect(
                    eUTL::UvDist(static_cast<int64_t>(0)),
                    eUTL::UvDist(yBase),
                    eUTL::UvDist(master.width),
                    eUTL::UvDist(yBase + halfRow));
                master.shapes.push_back(ms);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Build row track pattern
// -----------------------------------------------------------------------------
void ImplantLayerCheckerHelper::buildTrackPattern()
{
    std::vector<LayerId> nLayers, pLayers;
    for (const auto& layer : data_.layers) {
        if (layer.polarity == Polarity::N) {
            nLayers.push_back(layer.id);
        } else {
            pLayers.push_back(layer.id);
        }
    }

    auto& pattern = data_.tracks;

    for (const auto& row : data_.rows) {
        RowId r = row.rowId;
        bool isEven = ((r % 2) == 0);

        if (isEven) {
            if (!pLayers.empty()) pattern.layerBySlot[{r, BandSlot::Bottom}] = pLayers[0];
            if (!nLayers.empty()) pattern.layerBySlot[{r, BandSlot::Top}] = nLayers[0];
        } else {
            if (!nLayers.empty()) pattern.layerBySlot[{r, BandSlot::Bottom}] = nLayers[0];
            if (!pLayers.empty()) pattern.layerBySlot[{r, BandSlot::Top}] = pLayers[0];
        }

        if (r + 1 < static_cast<RowId>(data_.rows.size())) {
            pattern.activeKindByBoundary[{r, r + 1}] = isEven ? Polarity::N : Polarity::P;
        }
    }
}

// -----------------------------------------------------------------------------
// Main initialization: extract all data from UDM PhysDesMgr
// -----------------------------------------------------------------------------
void ImplantLayerCheckerHelper::init(const PhysDesMgr& desMgr)
{
    data_ = ImplantInput{};
    techLayerToCheckerId_.clear();

    const eLIB::TechLib& tech = desMgr.getTopTech();

    // =========================================================================
    // Step 1: Extract implant layers from TechLib
    // =========================================================================
    LayerId nextLayerId = 0;
    for (const eLIB::TechLayer& layer : tech.getLayerIter()) {
        if (!layer.isImplant()) {
            continue;
        }

        ImplantLayer il;
        il.id = nextLayerId;
        il.name = layer.getName();
        parseLayerName(il.name, il.family, il.polarity);

        techLayerToCheckerId_[layer.getId().getLocalId()] = nextLayerId;
        data_.layers.push_back(il);
        nextLayerId++;
    }

    // =========================================================================
    // Step 2: Extract simple rules from implant layers
    // =========================================================================
    int nextRuleId = 0;
    for (const auto& il : data_.layers) {
        eLIB::TechLayerRelativeID relId(0);
        for (const auto& [techRel, checkerId] : techLayerToCheckerId_) {
            if (checkerId == il.id) { relId = techRel; break; }
        }

        const eLIB::TechLayer& techLayer = tech.getTechLayer(relId);
        bool hasWidth = false, hasSpacing = false;

        // For implant layers, LEF WIDTH is stored in Width attribute
        // (setWidth is called in lefIn for layer->width()).
        // MinWidth is for LEF MINWIDTH statement (unrelated).
        DbCoord implantWidthVal = techLayer.getWidth().getStorage();
        if (implantWidthVal > 0) {
            Rule wRule;
            wRule.ruleId = nextRuleId++;
            wRule.source = RuleSource::Width;
            wRule.primaryLayer = il.id;
            wRule.minValue = implantWidthVal;
            data_.rules.push_back(wRule);
            hasWidth = true;
        } else {
            addDiag("layer " + il.name + " has no WIDTH value");
        }

        // For implant layers, SPACING is stored in MinSpacing or may need
        // to be extracted via TechRuleCheck. Try MinSpacing first.
        DbCoord minSpacingVal = techLayer.getMinSpacing().getStorage();
        if (minSpacingVal > 0) {
            Rule sRule;
            sRule.ruleId = nextRuleId++;
            sRule.source = RuleSource::Spacing;
            sRule.primaryLayer = il.id;
            sRule.minValue = minSpacingVal;
            data_.rules.push_back(sRule);
            hasSpacing = true;
        } else {
            addDiag("layer " + il.name + " has no SPACING value via MinSpacing");
        }

        if (!hasWidth && !hasSpacing) {
            addDiag("skipped missing_rule_parameter: layer " + il.name
                    + " has no WIDTH or SPACING rule");
        }
    }

    // =========================================================================
    // Step 3: Extract rows
    // =========================================================================
    {
        RowId nextRowId = 0;
        for (const PhysRow& row : desMgr.getPhysRowIter()) {
            RowInput ri;
            ri.rowId = nextRowId++;
            data_.rows.push_back(ri);
            if (!row.getSite().getSiteSpan()) {
                auto w = row.getSite().getWidth().getStorage();
                if (data_.siteWidth == 0 || w < data_.siteWidth) {
                    data_.siteWidth = w;
                }
                auto h = row.getSite().getHeight().getStorage();
                if (data_.rowHeight == 0 || h < data_.rowHeight) {
                    data_.rowHeight = h;
                }
            }
        }
    }

    // =========================================================================
    // Step 4: Build row track pattern (must precede master shape rebuild)
    // =========================================================================
    buildTrackPattern();

    // =========================================================================
    // Step 5a: Collect unique masters with implant shapes from placed instances
    // =========================================================================
    // key: PhysLibCell*  value: hasImplantShapes
    std::map<const eLIB::PhysLibCell*, bool> mastersWithImplant;
    // key: PhysLibCell*  value: checker MasterId
    std::map<const eLIB::PhysLibCell*, MasterId> masterToId;

    const eUNL::HierManager& hierMgr = desMgr.getHierMgr();
    for (eUNL::LeafCellId lCId : hierMgr.getAllLeafCellIter()) {
        PhysCell physCell = desMgr.getPhysCell(lCId);
        if (!physCell.isValid()) {
            continue;
        }

        const eLIB::PhysLibCell& master = physCell.getPhysMaster();
        if (!master.getType().isCore()) {
            continue;
        }

        if (mastersWithImplant.find(&master) != mastersWithImplant.end()) {
            continue;  // already checked
        }

        // Check if this master has any implant shapes
        bool hasImplant = false;
        const auto& obsVec = master.getObstruction();
        for (const auto& obs : obsVec) {
            const auto& shapes = obs.getShapes(eUTL::PhysOrientationE::R0);
            for (const auto& [layerRelId, shapeVec] : shapes) {
                if (findLayerId(layerRelId) >= 0) {
                    hasImplant = true;
                    break;
                }
            }
            if (hasImplant) break;
        }

        mastersWithImplant[&master] = hasImplant;
    }

    // =========================================================================
    // Step 5b: Build MasterInput from masters with implant shapes
    // =========================================================================
    MasterId nextMasterId = 0;
    for (const auto& [masterPtr, hasImplant] : mastersWithImplant) {
        if (!hasImplant) {
            continue;
        }

        const eLIB::PhysLibCell& master = *masterPtr;
        MasterInput mi;
        mi.width = master.getWidth().getStorage();
        mi.height = master.getHeight().getStorage();
        mi.siteHeight = master.getTechSite()->getHeight().getStorage();
        mi.isFiller = master.getType().isCoreFiller() || master.getType().isPadFiller();

        ShapeId shapeId = 0;
        const auto& obsVec = master.getObstruction();
        for (const auto& obs : obsVec) {
            const auto& shapes = obs.getShapes(eUTL::PhysOrientationE::R0);
            for (const auto& [layerRelId, shapeVec] : shapes) {
                LayerId checkerLayerId = findLayerId(layerRelId);
                if (checkerLayerId < 0) {
                    continue;
                }
                for (const auto& techShape : shapeVec) {
                    if (techShape.getType() != eLIB::TechShape::RECT) {
                        addDiag("skipped unsupported_geometry: master "
                                + std::to_string(nextMasterId) + " non-RECT shape");
                        continue;
                    }
                    const eUTL::Rect& mRect = techShape.getRect();
                    MasterShape mis;
                    mis.shapeId = shapeId++;
                    mis.layer = checkerLayerId;
                    mis.rect = mRect;
                    mi.shapes.push_back(mis);
                }
            }
        }

        if (!mi.shapes.empty()) {
            mi.masterId = nextMasterId;
            // Preserve raw shapes
            mi.rawShapes = mi.shapes;
            masterToId[masterPtr] = nextMasterId;
            data_.masters.push_back(mi);
            nextMasterId++;
        }
    }

    // =========================================================================
    // Step 5c: Rebuild master shapes into canonical band-level shapes
    // =========================================================================
    rebuildMasterShapes();

    // =========================================================================
    // Step 6: Extract placed instances from design
    // =========================================================================
    {
        std::vector<std::pair<eUTL::UvDist, eUTL::UvDist>> rowYBounds;
        std::vector<eUTL::UvDist> rowOriginsX;
        for (const PhysRow& row : desMgr.getPhysRowIter()) {
            eUTL::UvDist yLo = row.getOrigin().getY();
            rowYBounds.emplace_back(yLo, yLo + row.getSite().getHeight());
            rowOriginsX.push_back(row.getOrigin().getX());
        }

        InstanceId nextInstId = 0;
        for (eUNL::LeafCellId lCId : hierMgr.getAllLeafCellIter()) {
            PhysCell physCell = desMgr.getPhysCell(lCId);
            if (!physCell.isValid()) {
                continue;
            }

            const eLIB::PhysLibCell& master = physCell.getPhysMaster();

            auto mIt = masterToId.find(&master);
            if (mIt == masterToId.end()) {
                continue;
            }

            eUNL::PhysObjStatus status = physCell.getStatus();
            if (status != eUNL::PhysObjStatus::PLACED &&
                status != eUNL::PhysObjStatus::LOC_FIXED) {
                continue;
            }

            eUTL::Point2D origin = physCell.getOrigin();
            eUTL::PhysOrientation orient = physCell.getOrient();

            if (orient != eUTL::PhysOrientationE::R0 &&
                orient != eUTL::PhysOrientationE::MX &&
                orient != eUTL::PhysOrientationE::MY &&
                orient != eUTL::PhysOrientationE::R180) {
                addDiag("skipped unsupported_geometry: instance "
                        + std::to_string(lCId.getIndexValue())
                        + " unsupported orientation");
                continue;
            }

            // Find row from Y coordinate
            eUTL::UvDist y = origin.getY();
            RowId rowId = -1;
            for (size_t ri = 0; ri < rowYBounds.size(); ++ri) {
                if (y >= rowYBounds[ri].first && y < rowYBounds[ri].second) {
                    rowId = static_cast<RowId>(ri);
                    break;
                }
            }

            if (rowId < 0) {
                addDiag("placement not site aligned: instance "
                        + std::to_string(lCId.getIndexValue())
                        + " not in any row");
                continue;
            }

            if (data_.siteWidth <= 0) {
                addDiag("missing site width");
                continue;
            }

            eUTL::UvDist x = origin.getX();
            eUTL::UvDist rowOriginX = rowOriginsX[rowId];
            DbCoord xOffset = (x - rowOriginX).getStorage();

            if (xOffset < 0) {
                addDiag("placement not site aligned: instance "
                        + std::to_string(lCId.getIndexValue())
                        + " x before row origin");
                continue;
            }

            if (xOffset % data_.siteWidth != 0) {
                addDiag("placement not site aligned: instance "
                        + std::to_string(lCId.getIndexValue())
                        + " x not site-aligned");
                continue;
            }

            PlacedInst pi;
            pi.instanceId = nextInstId++;
            pi.masterId = mIt->second;
            pi.rowId = rowId;
            pi.columnId = data_.siteWidth > 0 ? (xOffset / data_.siteWidth) : 0;
            pi.orientation = orient;
            pi.isFiller = mIt->first->getType().isCoreFiller()
                          || mIt->first->getType().isPadFiller();
            data_.placedInsts.push_back(pi);
        }
    }
}

// -----------------------------------------------------------------------------
// Generate statistics string
// -----------------------------------------------------------------------------
std::string ImplantLayerCheckerHelper::toString() const
{
    std::ostringstream oss;
    oss << "========================================\n";
    oss << "Implant Layer Data Extraction Summary\n";
    oss << "========================================\n";

    oss << "Implant Layers: " << data_.layers.size() << "\n";
    for (const auto& il : data_.layers) {
        oss << "  LayerId=" << il.id
            << " name=\"" << il.name << "\""
            << " family=";
        switch (il.family) {
            case Family::VTS:  oss << "VTS"; break;
            case Family::VTL:  oss << "VTL"; break;
            case Family::VTH:  oss << "VTH"; break;
            case Family::VTUL: oss << "VTUL"; break;
            default:           oss << "Unknown"; break;
        }
        oss << " polarity=" << (il.polarity == Polarity::N ? "N" : "P") << "\n";
    }

    oss << "Rules: " << data_.rules.size() << "\n";
    for (const auto& rule : data_.rules) {
        oss << "  RuleId=" << rule.ruleId << " source=";
        switch (rule.source) {
            case RuleSource::Width:       oss << "WIDTH"; break;
            case RuleSource::Spacing:     oss << "SPACING"; break;
            case RuleSource::Lef58Width:  oss << "LEF58_WIDTH"; break;
            case RuleSource::Lef58Spacing: oss << "LEF58_SPACING"; break;
        }
        oss << " primaryLayer=" << rule.primaryLayer
            << " minValue=" << rule.minValue;
        if (rule.secondaryLayer) oss << " secondaryLayer=" << *rule.secondaryLayer;
        oss << "\n";
    }

    oss << "Implant Groups: " << data_.groups.size() << "\n";
    oss << "Masters with Implant Shapes: " << data_.masters.size() << "\n";
    for (const auto& m : data_.masters) {
        oss << "  MasterId=" << m.masterId
            << " width=" << m.width << " height=" << m.height
            << " siteHeight=" << m.siteHeight
            << " rawShapes=" << m.rawShapes.size()
            << " rebuiltShapes=" << m.shapes.size();

        for (const auto& shape : m.shapes) {
            oss << " [" << shape.shapeId << ":L" << shape.layer << " "
                << shape.rect.toString() << "]";
        }
        oss << "\n";
        if (!m.rawShapes.empty()) {
            oss << "    raw: ";
            for (const auto& rs : m.rawShapes) {
                oss << "(L" << rs.layer << " " << rs.rect.toString() << ") ";
            }
            oss << "\n";
        }
    }

    int fillerNum = std::count_if(data_.placedInsts.begin(), data_.placedInsts.end(), [](const auto& inst) { return inst.isFiller; });
    oss << "Placed Instances: " << data_.placedInsts.size() << " filler: " << fillerNum << "\n";
    for (const auto& inst : data_.placedInsts) {
        oss << "  InstanceId=" << inst.instanceId
            << " masterId=" << inst.masterId
            << " coord=<" << inst.rowId << ", " << inst.columnId << ">"
            << " orient=";
        if (inst.orientation == PhysOrientationE::R0) {
            oss << "R0";
        } else if (inst.orientation == PhysOrientationE::MX) {
            oss << "MX";
        } else if (inst.orientation == PhysOrientationE::MY) {
            oss << "MY";
        } else if (inst.orientation == PhysOrientationE::R180) {
            oss << "R180";
        } else {
            oss << "?";
        }
        oss << "\n";
    }

    oss << "Rows: " << data_.rows.size() << "\n";
    oss << "Row Height: " << data_.rowHeight << "\n";
    oss << "Site Width: " << data_.siteWidth << "\n";

    oss << "Row Track Pattern layerBySlot: "
        << data_.tracks.layerBySlot.size() << "\n";
    for (const auto& [key, layerId] : data_.tracks.layerBySlot) {
        auto [rowId, slot] = key;
        oss << "  Row=" << rowId
            << " Slot=" << (slot == BandSlot::Bottom ? "Bottom" : "Top")
            << " -> LayerId=" << layerId << "\n";
    }

    oss << "Row Track Pattern activeKindByBoundary: "
        << data_.tracks.activeKindByBoundary.size() << "\n";
    for (const auto& [key, pol] : data_.tracks.activeKindByBoundary) {
        auto [rowA, rowB] = key;
        oss << "  Boundary Row(" << rowA << "," << rowB
            << ") -> " << (pol == Polarity::N ? "N" : "P") << "\n";
    }

    oss << "Diagnostics: " << diags_.size() << "\n";
    for (const auto& d : diags_) {
        oss << "  " << d << "\n";
    }

    oss << "========================================\n";
    return oss.str();
}

} // namespace ipl
} // namespace dpl2
