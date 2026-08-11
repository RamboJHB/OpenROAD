#include "drc/ImplantLayerChecker.h"
#include "drc/ImplantLayerCheckerHelper.h"
#include "ImplantLayerCheckerSimpleTestSupport.h"
#include "ImplantLayerDirectTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace dpl2 {
namespace ipl {
namespace simpletest {

PlacedInst place(InstanceId instanceId,
               MasterId masterId,
               RowId rowId,
               Dbu colId,
               int orientation)
{
    return PlacedInst{instanceId,
                      masterId,
                      rowId,
                      colId,
                      static_cast<PhysOrientation>(orientation)};
}

Rule rule(int ruleId,
         RuleSource source,
         LayerId primaryLayer,
         Dbu value)
{
    Rule rule;
    rule.setRuleId(ruleId);
    rule.setSource(source);
    rule.setPrimaryLayer(primaryLayer);
    rule.setMinValue(value);
    rule.setDirection(source == RuleSource::Spacing ||
                      source == RuleSource::Lef58Spacing
                  ? RuleDirection::Horizontal
                  : RuleDirection::Any);
    return rule;
}

Rule interLayerSpacingRule(int ruleId,
                          LayerId primaryLayer,
                          LayerId secondaryLayer,
                          Dbu value)
{
    Rule Rule =
        rule(ruleId, RuleSource::Spacing, primaryLayer, value);
    Rule.setSecondaryLayer(secondaryLayer);
    return Rule;
}

Rule lef58WidthRule(int ruleId, LayerId primaryLayer, Dbu value)
{
    return rule(ruleId, RuleSource::Lef58Width, primaryLayer, value);
}

Rule lef58SpacingRule(int ruleId, LayerId primaryLayer, Dbu value)
{
    return rule(ruleId, RuleSource::Lef58Spacing, primaryLayer, value);
}

MasterShape shape(MasterId masterId,
               ShapeId shapeId,
               LayerId layer,
               Dbu xl,
               Dbu yl,
               Dbu xh,
               Dbu yh)
{
    return MasterShape{masterId, shapeId, layer,
      directtest::makeRect(xl, yl, xh, yh)};
}

std::vector<Rule> rules()
{
    return {rule(0, RuleSource::Width, F1_N, 40),
            rule(1, RuleSource::Spacing, F1_N, 40),
            rule(2, RuleSource::Width, F1_P, 40),
            rule(3, RuleSource::Spacing, F1_P, 40),
            rule(4, RuleSource::Width, F2_N, 40),
            rule(5, RuleSource::Spacing, F2_N, 40),
            rule(6, RuleSource::Width, F2_P, 40),
            rule(7, RuleSource::Spacing, F2_P, 40),
            rule(8, RuleSource::Width, F3_N, 40),
            rule(9, RuleSource::Spacing, F3_N, 40),
            rule(10, RuleSource::Width, F3_P, 40),
            rule(11, RuleSource::Spacing, F3_P, 40)};
}

std::vector<MasterItem> masters()
{
    return {MasterItem{0,
                       20,
                       100,
                       {shape(0, 1, F1_N, 0, 50, 20, 100),
                        shape(0, 2, F1_P, 0, 0, 20, 50)}},
            MasterItem{1,
                       40,
                       100,
                       {shape(1, 3, F1_N, 0, 50, 40, 100),
                        shape(1, 4, F1_P, 0, 0, 40, 50)}},
            MasterItem{2,
                       20,
                       100,
                       {shape(2, 5, F1_P, 0, 50, 20, 100),
                        shape(2, 6, F1_N, 0, 0, 20, 50)}},
            MasterItem{3,
                       40,
                       100,
                       {shape(3, 7, F2_N, 0, 50, 40, 100),
                        shape(3, 8, F2_P, 0, 0, 40, 50)}},
            MasterItem{4,
                       20,
                       100,
                       {shape(4, 9, F2_P, 0, 50, 20, 100),
                        shape(4, 10, F2_N, 0, 0, 20, 50)}},
            MasterItem{5,
                       60,
                       100,
                       {shape(5, 11, F3_N, 0, 50, 60, 100),
                        shape(5, 12, F3_P, 0, 0, 60, 50)}},
            MasterItem{6,
                       40,
                       100,
                       {shape(6, 13, F3_P, 0, 50, 40, 100),
                        shape(6, 14, F3_N, 0, 0, 40, 50)}},
            MasterItem{7,
                       60,
                       200,
                       {shape(7, 21, F1_P, 0, 0, 60, 50),
                        shape(7, 22, F1_N, 0, 50, 60, 100),
                        shape(7, 23, F1_N, 0, 100, 60, 150),
                        shape(7, 24, F1_P, 0, 150, 60, 200)}},
            MasterItem{8,
                       40,
                       200,
                       {shape(8, 25, F3_P, 0, 0, 40, 50),
                        shape(8, 26, F3_N, 0, 50, 40, 100),
                        shape(8, 27, F3_N, 0, 100, 40, 150),
                        shape(8, 28, F3_P, 0, 150, 40, 200)}},
            MasterItem{9,
                       60,
                       300,
                       {shape(9, 31, F2_P, 0, 0, 60, 50),
                        shape(9, 32, F2_N, 0, 50, 60, 100),
                        shape(9, 33, F2_N, 0, 100, 60, 150),
                        shape(9, 34, F2_P, 0, 150, 60, 200),
                        shape(9, 35, F2_P, 0, 200, 60, 250),
                        shape(9, 36, F2_N, 0, 250, 60, 300)}},
            MasterItem{10,
                       80,
                       300,
                       {shape(10, 41, F3_N, 0, 0, 80, 50),
                        shape(10, 42, F3_P, 0, 50, 80, 100),
                        shape(10, 43, F3_P, 0, 100, 80, 150),
                        shape(10, 44, F3_N, 0, 150, 80, 200),
                        shape(10, 45, F3_N, 0, 200, 80, 250),
                        shape(10, 46, F3_P, 0, 250, 80, 300)}},
            MasterItem{11,
                       20,
                       100,
                       {shape(11, 51, F3_N, 0, 50, 20, 100),
                        shape(11, 52, F3_P, 0, 0, 20, 50)}}};
}

ImplantInput inputFor(const SimpleCase& testCase)
{
    ImplantInput input;
    input.layers = {Layer{F1_N, "F1_N", Layer::Vt::UL, Layer::Polar::N},
                    Layer{F1_P, "F1_P", Layer::Vt::UL, Layer::Polar::P},
                    Layer{F2_N, "F2_N", Layer::Vt::L, Layer::Polar::N},
                    Layer{F2_P, "F2_P", Layer::Vt::L, Layer::Polar::P},
                    Layer{F3_N, "F3_N", Layer::Vt::H, Layer::Polar::N},
                    Layer{F3_P, "F3_P", Layer::Vt::H, Layer::Polar::P}};
    input.rules = rules();
    input.rules.insert(input.rules.end(),
                       testCase.extraRules.begin(),
                       testCase.extraRules.end());
    input.groups = testCase.groups;
    input.masters = masters();
    input.placedInsts = testCase.placedInsts;
    input.rowCount = testCase.rowCount;
    input.rowHeight = 100;
    input.siteWidth = SIMPLE_SITE_WIDTH;
    // Compute colCount from placed instance extents
    ColId maxCol = 0;
    for (const PlacedInst& pi : testCase.placedInsts) {
        const Dbu masterWidth = input.masters[pi.masterId].width;
        ColId endCol = pi.colId + masterWidth / SIMPLE_SITE_WIDTH - 1;
        if (endCol > maxCol) {
            maxCol = endCol;
        }
    }
    input.colCount = maxCol + 1;
    return input;
}
std::string familyLabel(LayerId layer)
{
    switch (layer) {
    case F1_N:
    case F1_P:
        return "F1";
    case F2_N:
    case F2_P:
        return "F2";
    case F3_N:
    case F3_P:
        return "F3";
    default:
        return "??";
    }
}

struct MasterSchematicInfo
{
    std::string family;
    Dbu width = 0;
    int rowCount = 1;
};

std::unordered_map<MasterId, MasterSchematicInfo> masterSchematicInfo()
{
    std::unordered_map<MasterId, MasterSchematicInfo> infoByMaster;
    for (const MasterItem& master : masters()) {
        MasterSchematicInfo info;
        info.family = master.shapes.empty() ? "??"
                                            : familyLabel(master.shapes.front().layer);
        info.width = master.width;
        info.rowCount = static_cast<int>(master.height / 100);
        infoByMaster[master.masterId] = info;
    }
    return infoByMaster;
}

std::string cellToken(const PlacedInst& instance,
                    const std::string& family,
                    int siteCount)
{
    const size_t tokenWidth = static_cast<size_t>(std::max(1, siteCount)) * 6;
    std::ostringstream token;
    token << std::setw(2) << std::setfill('0') << instance.instanceId << family;
    const std::string label = token.str();
    if (tokenWidth <= label.size() + 2) {
        return '[' + label.substr(0, tokenWidth - 2) + ']';
    }
    const size_t innerWidth = tokenWidth - 2;
    const size_t leftPadding = (innerWidth - label.size()) / 2;
    const size_t rightPadding = innerWidth - label.size() - leftPadding;
    return '[' + std::string(leftPadding, ' ') + label +
           std::string(rightPadding, ' ') + ']';
}

std::string siteToken(int site)
{
    std::ostringstream token;
    token << std::setw(2) << std::setfill('0') << site;
    std::string value = token.str();
    if (value.size() < 6) {
        value.append(6 - value.size(), '-');
    }
    return value.substr(0, 6);
}

std::string schematicFor(const SimpleCase& testCase)
{
    constexpr int SITE_WIDTH = static_cast<int>(SIMPLE_SITE_WIDTH);
    constexpr const char* EMPTY_SITE = "------";

    const std::unordered_map<MasterId, MasterSchematicInfo> masterInfo =
        masterSchematicInfo();

    int maxSite = 0;
    std::vector<std::vector<std::string>> rows(testCase.rowCount);
    std::vector<std::vector<bool>> occupied(testCase.rowCount);
    for (const PlacedInst& instance : testCase.placedInsts) {
        const auto found = masterInfo.find(instance.masterId);
        const int occupiedRows = found == masterInfo.end() ? 1
                              : found->second.rowCount;
        const std::string family =
            found == masterInfo.end() ? "??" : found->second.family;
        const Dbu masterWidth =
            found == masterInfo.end() ? SITE_WIDTH : found->second.width;
        const int siteCount =
            std::max(1, static_cast<int>(masterWidth / SITE_WIDTH));
        const int site = static_cast<int>(instance.colId);
        maxSite = std::max(maxSite, site + siteCount - 1);
        for (int rowOffset = 0; rowOffset < occupiedRows; ++rowOffset) {
            const RowId row = instance.rowId + rowOffset;
            if (row < 0 || row >= static_cast<RowId>(rows.size())) {
                continue;
            }
            if (static_cast<int>(rows[row].size()) <= maxSite) {
                rows[row].resize(maxSite + 1);
                occupied[row].resize(maxSite + 1, false);
            }
            rows[row][site] = cellToken(instance, family, siteCount);
            for (int offset = 0; offset < siteCount; ++offset) {
                occupied[row][site + offset] = true;
                if (offset > 0) {
                    rows[row][site + offset] = "";
                }
            }
        }
    }
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        rows[row].resize(maxSite + 1);
        occupied[row].resize(maxSite + 1, false);
    }

    std::ostringstream schematic;
    schematic << "Schematic: " << testCase.name << '\n'
              << "  Legend: [  09F1  ] = instance 09, family F1; "
              << EMPTY_SITE << " = empty/spacing/gap.\n"
              << "          P/N bands are merged into one placement row.\n"
              << "          Every site renders as exactly 6 characters; a cell "
              << "token spans master.width / site_width sites.\n"
              << "  Sites:  ";
    for (int site = 0; site <= maxSite; ++site) {
        schematic << siteToken(site);
    }
    schematic << '\n';

    for (int row = static_cast<int>(rows.size()) - 1; row >= 0; --row) {
        schematic << "  r" << row << ":     ";
        for (int site = 0; site <= maxSite; ++site) {
            const std::string& token = rows[row][site];
            if (token.empty() && !occupied[row][site]) {
                schematic << EMPTY_SITE;
            } else if (!token.empty()) {
                schematic << token;
            }
        }
        schematic << '\n';
    }
    return schematic.str();
}

bool hasCellPlacementOverlap(const SimpleCase& testCase)
{
    const std::unordered_map<MasterId, MasterSchematicInfo> masterInfo =
        masterSchematicInfo();
    std::map<std::pair<RowId, int>, InstanceId> occupiedSites;
    for (const PlacedInst& instance : testCase.placedInsts) {
        const auto found = masterInfo.find(instance.masterId);
        const int occupiedRows = found == masterInfo.end() ? 1
            : found->second.rowCount;
        const Dbu masterWidth =
            found == masterInfo.end() ? SIMPLE_SITE_WIDTH : found->second.width;
        const int siteCount = std::max(
            1, static_cast<int>(masterWidth / SIMPLE_SITE_WIDTH));
        for (int rowOffset = 0; rowOffset < occupiedRows; ++rowOffset) {
            const RowId row = instance.rowId + rowOffset;
            for (int siteOffset = 0; siteOffset < siteCount; ++siteOffset) {
                const int site =
                    static_cast<int>(instance.colId) + siteOffset;
                const auto [_, inserted] = occupiedSites.emplace(
                    std::make_pair(row, site), instance.instanceId);
                if (!inserted) {
                    return true;
                }
            }
        }
    }
    return false;
}

UniqueViolationKey uniqueKeyFor(const PlacedInst& target,
                               const Violation& violation)
{
    UniqueViolationKey key;
    key.ruleId = violation.ruleId;
    key.relatedInstances = violation.instances;
    key.relatedInstances.push_back(target.instanceId);
    std::sort(key.relatedInstances.begin(), key.relatedInstances.end());
    key.relatedInstances.erase(
        std::unique(key.relatedInstances.begin(), key.relatedInstances.end()),
        key.relatedInstances.end());
    return key;
}

void printViolation(const SimpleCase& testCase,
                   const PlacedInst& target,
                   const Violation& violation)
{
    std::cout << "violation case=" << testCase.name
              << " target_instance=" << target.instanceId
              << " target_master=" << target.masterId
              << " target_row=" << target.rowId
              << " target_x=" << target.colId * SIMPLE_SITE_WIDTH
              << violation.toString(SIMPLE_SITE_WIDTH) << '\n';
}

SimpleCase withExtraRules(const SimpleCase& base,
                         const char* name,
                         const char* description,
                         size_t illegalChecks,
                         size_t totalViolations,
                         size_t uniqueViolations,
                         std::vector<Rule> extraRules)
{
    SimpleCase testCase = base;
    testCase.name = name;
    testCase.description = description;
    testCase.extraRules = extraRules;
    testCase.illegalChecks = illegalChecks;
    testCase.totalViolations = totalViolations;
    testCase.uniqueViolations = uniqueViolations;
    return testCase;
}

std::vector<SimpleCase> makeSimpleCases()
{
    std::vector<SimpleCase> cases = {
        {"case01_dense_four_rows",
         "Scenario: case01_dense_four_rows, four-row placement grid.\n"
         "Expected: checked_instances=16, illegal_checks=13, total_violations=36, unique_violations=25.",
         4,
         {place(0, 0, 0, 0, 0),
          place(1, 1, 0, 2, 0),
          place(2, 3, 0, 4, 0),
          place(3, 5, 0, 6, 0),
          place(4, 11, 0, 10, 0),
          place(5, 2, 1, 0, 0),
          place(6, 4, 1, 2, 0),
          place(7, 6, 1, 4, 0),
          place(8, 7, 0, 11, 0),
          place(9, 8, 0, 14, 0),
          place(10, 0, 2, 0, 0),
          place(11, 3, 2, 2, 0),
          place(12, 5, 2, 4, 0),
          place(13, 2, 3, 0, 0),
          place(14, 4, 3, 1, 0),
          place(15, 6, 3, 3, 0)},
         16,
         13,
         36,
         25},
        {"case02_corner_abutment_six_rows",
         "Scenario: six rows with exact abutments at x=30/35 and tight near-corner adjacent-row contacts.\n"
         "Expected: checked_instances=18, illegal_checks=10, total_violations=18, unique_violations=13.",
         6,
         {place(0, 0, 0, 0, 0),
          place(1, 0, 0, 1, 0),
          place(2, 1, 0, 4, 0),
          place(3, 3, 0, 7, 0),
          place(4, 5, 0, 9, 0),
          place(5, 2, 1, 0, 0),
          place(6, 2, 1, 1, 0),
          place(7, 4, 1, 3, 0),
          place(8, 6, 1, 5, 0),
          place(9, 9, 0, 12, 0),
          place(10, 7, 2, 0, 0),
          place(11, 8, 2, 4, 0),
          place(12, 0, 4, 0, 0),
          place(13, 3, 4, 1, 0),
          place(14, 5, 4, 3, 0),
          place(15, 2, 5, 0, 0),
          place(16, 4, 5, 1, 0),
          place(17, 6, 5, 2, 0)},
         18,
         10,
         18,
         13},
        {"case03_multifamily_three_height",
         "Scenario: multi-family mix with 3-row, 2-row, and 1-row masters sharing rows.\n"
         "Expected: checked_instances=15, illegal_checks=9, total_violations=16, unique_violations=13.",
         6,
         {place(0, 9, 0, 0, 0),
          place(1, 10, 1, 4, 1),
          place(2, 7, 0, 9, 0),
          place(3, 8, 2, 12, 0),
          place(4, 0, 0, 3, 0),
          place(5, 1, 2, 8, 0),
          place(6, 3, 2, 10, 0),
          place(7, 5, 2, 14, 0),
          place(8, 2, 3, 0, 0),
          place(9, 4, 3, 1, 0),
          place(10, 6, 3, 8, 0),
          place(11, 11, 4, 0, 0),
          place(12, 0, 4, 1, 0),
          place(13, 3, 4, 3, 0),
          place(14, 5, 4, 5, 0)},
         15,
         9,
         16,
         13},
        {"case04_wide_rows_layer_mix",
         "Scenario: five rows with wide x spread, mixed concrete layers, and a flipped 3-row master.\n"
         "Expected: checked_instances=15, illegal_checks=11, total_violations=26, unique_violations=19.",
         5,
         {place(0, 0, 0, 0, 0),
          place(1, 3, 0, 1, 0),
          place(2, 5, 0, 3, 0),
          place(3, 7, 0, 7, 0),
          place(4, 8, 0, 11, 0),
          place(5, 2, 1, 0, 0),
          place(6, 4, 1, 1, 0),
          place(7, 6, 1, 2, 0),
          place(8, 10, 1, 13, 1),
          place(9, 1, 2, 0, 0),
          place(10, 3, 2, 3, 0),
          place(11, 5, 2, 10, 0),
          place(12, 0, 4, 0, 0),
          place(13, 0, 4, 2, 0),
          place(14, 11, 4, 5, 0)},
         15,
         11,
         26,
         19},
        {"case05_boundary_width_spacing",
         "Scenario: four rows emphasizing spacing exactly near rule boundaries and separated high-x groups.\n"
         "Expected: checked_instances=16, illegal_checks=9, total_violations=30, unique_violations=22.",
         4,
         {place(0, 0, 0, 0, 0),
          place(1, 1, 0, 2, 0),
          place(2, 3, 0, 5, 0),
          place(3, 5, 0, 8, 0),
          place(4, 0, 0, 14, 0),
          place(5, 2, 1, 0, 0),
          place(6, 4, 1, 1, 0),
          place(7, 6, 1, 4, 0),
          place(8, 7, 0, 11, 0),
          place(9, 9, 0, 16, 0),
          place(10, 0, 2, 0, 0),
          place(11, 3, 2, 1, 0),
          place(12, 11, 2, 3, 0),
          place(13, 2, 3, 0, 0),
          place(14, 4, 3, 2, 0),
          place(15, 6, 3, 4, 0)},
         16,
         9,
         30,
         22},
        {"case06_many_cells_six_layers",
         "Scenario: six rows, 25 cells, all six layers, repeated dense row pairs plus far multi-height masters.\n"
         "Expected: checked_instances=25, illegal_checks=20, total_violations=50, unique_violations=36.",
         6,
         {place(0, 0, 0, 0, 0),
          place(1, 3, 0, 1, 0),
          place(2, 5, 0, 3, 0),
          place(3, 1, 0, 7, 0),
          place(4, 11, 0, 10, 0),
          place(5, 2, 1, 0, 0),
          place(6, 4, 1, 1, 0),
          place(7, 6, 1, 2, 0),
          place(8, 2, 1, 5, 0),
          place(9, 7, 0, 13, 0),
          place(10, 0, 2, 0, 0),
          place(11, 3, 2, 1, 0),
          place(12, 5, 2, 3, 0),
          place(13, 8, 2, 8, 0),
          place(14, 2, 3, 0, 0),
          place(15, 4, 3, 1, 0),
          place(16, 6, 3, 3, 0),
          place(17, 9, 0, 18, 0),
          place(18, 10, 1, 22, 1),
          place(19, 0, 4, 0, 0),
          place(20, 3, 4, 1, 0),
          place(21, 5, 4, 3, 0),
          place(22, 2, 5, 0, 0),
          place(23, 4, 5, 1, 0),
          place(24, 6, 5, 2, 0)},
         25,
         20,
         50,
         36},
        {"case07_interrow_corner_touch",
         "Scenario: five rows arranged for near-corner inter-row contacts and tight horizontal gaps.\n"
         "Expected: checked_instances=17, illegal_checks=11, total_violations=22, unique_violations=19.",
         5,
         {place(0, 0, 0, 0, 0),
          place(1, 1, 0, 3, 0),
          place(2, 3, 0, 6, 0),
          place(3, 5, 0, 8, 0),
          place(4, 2, 1, 1, 0),
          place(5, 4, 1, 3, 0),
          place(6, 6, 1, 6, 0),
          place(7, 7, 0, 11, 0),
          place(8, 8, 2, 15, 0),
          place(9, 0, 2, 0, 0),
          place(10, 3, 2, 1, 0),
          place(11, 5, 2, 4, 0),
          place(12, 2, 3, 1, 0),
          place(13, 4, 3, 3, 0),
          place(14, 6, 3, 5, 0),
          place(15, 11, 4, 0, 0),
          place(16, 0, 4, 1, 0)},
         17,
         11,
         22,
         19},
        {"case08_three_row_heights_dense",
         "Scenario: seven rows dominated by dense but non-overlapping 3-row height interactions.\n"
         "Expected: checked_instances=15, illegal_checks=11, total_violations=22, unique_violations=16.",
         7,
         {place(0, 9, 0, 0, 0),
          place(1, 10, 1, 3, 1),
          place(2, 9, 2, 8, 0),
          place(3, 7, 4, 0, 0),
          place(4, 8, 4, 3, 0),
          place(5, 0, 0, 3, 0),
          place(6, 3, 0, 5, 0),
          place(7, 5, 0, 7, 0),
          place(8, 2, 1, 7, 0),
          place(9, 4, 1, 8, 0),
          place(10, 6, 1, 9, 0),
          place(11, 0, 6, 0, 0),
          place(12, 3, 6, 1, 0),
          place(13, 5, 6, 3, 0),
          place(14, 11, 6, 7, 0)},
         15,
         11,
         22,
         16},
        {"case09_abutting_runs_and_gaps",
         "Scenario: four rows with long abutting F1 runs, then small explicit gaps to F2/F3 cells.\n"
         "Expected: checked_instances=18, illegal_checks=11, total_violations=26, unique_violations=22.",
         4,
         {place(0, 0, 0, 0, 0),
          place(1, 0, 0, 1, 0),
          place(2, 0, 0, 3, 0),
          place(3, 1, 0, 5, 0),
          place(4, 3, 0, 8, 0),
          place(5, 5, 0, 10, 0),
          place(6, 2, 1, 0, 0),
          place(7, 2, 1, 1, 0),
          place(8, 4, 1, 4, 0),
          place(9, 6, 1, 5, 0),
          place(10, 7, 0, 15, 0),
          place(11, 8, 2, 0, 0),
          place(12, 0, 2, 3, 0),
          place(13, 3, 2, 4, 0),
          place(14, 5, 2, 6, 0),
          place(15, 2, 3, 2, 0),
          place(16, 4, 3, 3, 0),
          place(17, 6, 3, 4, 0)},
         18,
         11,
         26,
         18},
        // Schematic: case10_stress_mixed_rows
        {"case10_stress_mixed_rows",
         "Scenario: eight-row stress case with 26 cells, mixed 1/2/3-row masters, all six layers.\n"
         "Expected: checked_instances=26, illegal_checks=13, total_violations=32, unique_violations=25.",
         8,
         {place(0, 9, 0, 0, 0),
          place(1, 10, 1, 4, 1),
          place(2, 7, 0, 9, 0),
          place(3, 8, 2, 13, 0),
          place(4, 9, 4, 0, 0),
          place(5, 10, 5, 4, 1),
          place(6, 0, 0, 3, 0),
          place(7, 3, 0, 5, 0),
          place(8, 5, 0, 12, 0),
          place(9, 2, 1, 3, 0),
          place(10, 4, 1, 8, 0),
          place(11, 6, 1, 12, 0),
          place(12, 0, 2, 3, 0),
          place(13, 3, 2, 8, 0),
          place(14, 5, 2, 10, 0),
          place(15, 2, 3, 0, 0),
          place(16, 4, 3, 1, 0),
          place(17, 6, 3, 8, 0),
          place(18, 0, 6, 3, 0),
          place(19, 3, 6, 8, 0),
          place(20, 5, 6, 10, 0),
          place(21, 2, 7, 0, 0),
          place(22, 4, 7, 1, 0),
          place(23, 6, 7, 8, 0),
          place(24, 11, 6, 13, 0),
          place(25, 1, 6, 14, 0)},
         26,
         13,
         32,
         25},
    };
    cases.push_back(withExtraRules(
        cases[0],
        "case11_dense_four_rows_f1n_to_f2n_spacing",
        "Scenario: case01_dense_four_rows plus inter-layer F1_N to F2_N spacing.\n"
        "Expected: checked_instances=16, illegal_checks=13, total_violations=44, unique_violations=33.",
        13,
        43,
        32,
        {interLayerSpacingRule(12, F1_N, F2_N, 40)}));
    cases.push_back(withExtraRules(
        cases[1],
        "case12_corner_abutment_f1n_to_f2n_f3n_spacing",
        "Scenario: case02_corner_abutment_six_rows plus inter-layer F1_N to F2_N/F3_N spacing.\n"
        "Expected: checked_instances=18, illegal_checks=13, total_violations=33, unique_violations=25.",
        13,
        32,
        24,
        {interLayerSpacingRule(12, F1_N, F2_N, 40),
         interLayerSpacingRule(13, F1_N, F3_N, 40)}));
    cases.push_back(withExtraRules(
        cases[2],
        "case13_multifamily_f1p_to_f2p_spacing",
        "Scenario: case03_multifamily_three_height plus inter-layer F1_P to F2_P spacing.\n"
        "Expected: checked_instances=15, illegal_checks=9, total_violations=22, unique_violations=19.",
        9,
        20,
        17,
        {interLayerSpacingRule(12, F1_P, F2_P, 40)}));
    cases.push_back(withExtraRules(
        cases[3],
        "case14_wide_rows_f2p_to_f3p_spacing",
        "Scenario: case04_wide_rows_layer_mix plus inter-layer F2_P to F3_P spacing.\n"
        "Expected: checked_instances=15, illegal_checks=11, total_violations=29, unique_violations=22.",
        11,
        28,
        21,
        {interLayerSpacingRule(12, F2_P, F3_P, 40)}));
    cases.push_back(withExtraRules(
        cases[4],
        "case15_boundary_f2n_to_f3n_spacing",
        "Scenario: case05_boundary_width_spacing plus inter-layer F2_N to F3_N spacing.\n"
        "Expected: checked_instances=16, illegal_checks=10, total_violations=36, unique_violations=28.",
        10,
        35,
        27,
        {interLayerSpacingRule(12, F2_N, F3_N, 40)}));

    return cases;
}

const std::vector<SimpleCase>& simpleCases()
{
    static const std::vector<SimpleCase> cases = makeSimpleCases();
    return cases;
}

void replaySimpleCase(const SimpleCase& testCase)
{
    SCOPED_TRACE(testCase.name);
    SCOPED_TRACE(testCase.description);
    SCOPED_TRACE(schematicFor(testCase));

    ASSERT_FALSE(hasCellPlacementOverlap(testCase));

    const ImplantInput input = inputFor(testCase);
    ImplantLayerCheckerHelper helper;
    helper.initialize(input);
    ImplantLayerChecker checker(helper.getGrid(), helper.getDesign(),
        helper.getNetwork());
    helper.initChecker(checker);
    ASSERT_TRUE(checker.getDiags().empty());

    size_t illegalChecks = 0;
    size_t totalViolations = 0;
    std::set<UniqueViolationKey> uniqueViolations;
    for (const PlacedInst& instance : testCase.placedInsts) {
        const CheckRequest request{instance.instanceId,
                                   instance.masterId,
                                   instance.rowId,
                                   instance.colId,
                                   instance.orientation};
        const CheckResult result = checker.checkDirect(request);
        directtest::expectMatches(input, request, result);
        totalViolations += result.violations.size();
        for (const Violation& violation : result.violations) {
            if (uniqueViolations.insert(uniqueKeyFor(instance, violation))
                .second) {
                printViolation(testCase, instance, violation);
            }
        }
        if (!result.isLegal) {
            ++illegalChecks;
        }
    }

    EXPECT_EQ(testCase.placedInsts.size(), testCase.checkedInstances);
    EXPECT_EQ(illegalChecks, testCase.illegalChecks);
    EXPECT_EQ(totalViolations, testCase.totalViolations);
    EXPECT_EQ(uniqueViolations.size(), testCase.uniqueViolations);
}

class ImplantCheckerSimpleTest : public ::testing::TestWithParam<SimpleCase>
{
};

TEST_P(ImplantCheckerSimpleTest,
       ReplaysSimpleCaseFromInput)
{
    replaySimpleCase(GetParam());
}

INSTANTIATE_TEST_SUITE_P(SimpleCases,
                         ImplantCheckerSimpleTest,
                         ::testing::ValuesIn(simpleCases()));

} // namespace simpletest
} // namespace ipl
} // namespace dpl2