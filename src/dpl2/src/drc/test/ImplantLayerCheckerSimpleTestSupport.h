#pragma once

#include "drc/ImplantLayerChecker.h"
#include "drc/ImplantLayerCheckerHelper.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace dpl2 {
namespace ipl {
namespace simpletest {

constexpr LayerId F1_N = 0;
constexpr LayerId F1_P = 1;
constexpr LayerId F2_N = 2;
constexpr LayerId F2_P = 3;
constexpr LayerId F3_N = 4;
constexpr LayerId F3_P = 5;
constexpr Dbu SIMPLE_SITE_WIDTH = 20;

struct SimpleCase
{
    const char* name;
    const char* description;
    int rowCount;
    std::vector<PlacedInst> placedInsts;
    size_t checkedInstances;
    size_t illegalChecks;
    size_t totalViolations;
    size_t uniqueViolations;
    std::vector<Rule> extraRules;
    std::unordered_map<std::string, std::vector<LayerId>> groups;
};

struct UniqueViolationKey
{
    int ruleId = 0;
    std::vector<InstanceId> relatedInstances;

    bool operator<(const UniqueViolationKey& other) const
    {
        if (ruleId != other.ruleId) {
            return ruleId < other.ruleId;
        }
        return relatedInstances < other.relatedInstances;
    }
};

PlacedInst place(InstanceId instanceId,
               MasterId masterId,
               RowId rowId,
               Dbu colId,
               int orientation);
Rule rule(int ruleId,
         RuleSource source,
         LayerId primaryLayer,
         Dbu value);
Rule interLayerSpacingRule(int ruleId,
                          LayerId primaryLayer,
                          LayerId secondaryLayer,
                          Dbu value);
Rule lef58WidthRule(int ruleId, LayerId primaryLayer, Dbu value);
Rule lef58SpacingRule(int ruleId, LayerId primaryLayer, Dbu value);
std::vector<Rule> rules();
std::vector<MasterItem> masters();
ImplantInput inputFor(const SimpleCase& testCase);
std::string schematicFor(const SimpleCase& testCase);
bool hasCellPlacementOverlap(const SimpleCase& testCase);
UniqueViolationKey uniqueKeyFor(const PlacedInst& target,
                               const Violation& violation);
void printViolation(const SimpleCase& testCase,
                   const PlacedInst& target,
                   const Violation& violation);
void replaySimpleCase(const SimpleCase& testCase);
const std::vector<SimpleCase>& simpleCases();

} // namespace simpletest
} // namespace ipl
} // namespace dpl2