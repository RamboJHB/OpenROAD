#pragma once

#include "drc/ImplantLayerChecker.h"
#include "drc/ImplantLayerCheckerHelper.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <ostream>
#include <set>
#include <tuple>

namespace dpl2 {
namespace ipl {
namespace directtest {

struct ViolationSignature
{
    int ruleId = 0;
    RuleSource ruleSource = RuleSource::Width;
    LayerId primaryLayer = 0;
    std::optional<LayerId> secondaryLayer;
    Relationship relationship = Relationship::IntraRow;
    Dbu measuredValue = 0;
    Dbu requiredValue = 0;
    XInterval xWindow;

    bool operator<(const ViolationSignature& other) const
    {
        return std::tie(ruleId,
                        ruleSource,
                        primaryLayer,
                        secondaryLayer,
                        relationship,
                        measuredValue,
                        requiredValue,
                        xWindow.xl,
                        xWindow.xh) <
               std::tie(other.ruleId,
                        other.ruleSource,
                        other.primaryLayer,
                        other.secondaryLayer,
                        other.relationship,
                        other.measuredValue,
                        other.requiredValue,
                        other.xWindow.xl,
                        other.xWindow.xh);
    }

    bool operator==(const ViolationSignature& other) const
    {
        return ruleId == other.ruleId &&
               ruleSource == other.ruleSource &&
               primaryLayer == other.primaryLayer &&
               secondaryLayer == other.secondaryLayer &&
               relationship == other.relationship &&
               measuredValue == other.measuredValue &&
               requiredValue == other.requiredValue &&
               xWindow.xl == other.xWindow.xl &&
               xWindow.xh == other.xWindow.xh;
    }
};

inline std::ostream& operator<<(std::ostream& out,
                                const ViolationSignature& signature)
{
    out << "{rule=" << signature.ruleId
        << ",source=" << static_cast<int>(signature.ruleSource)
        << ",primary=" << signature.primaryLayer;
    if (signature.secondaryLayer) {
        out << ",secondary=" << *signature.secondaryLayer;
    }
    out << ",relationship=" << static_cast<int>(signature.relationship)
        << ",measured=" << signature.measuredValue
        << ",required=" << signature.requiredValue
        << ",x=[" << signature.xWindow.xl << ',' << signature.xWindow.xh
        << "]}";
    return out;
}

inline std::set<ViolationSignature> signatures(const CheckResult& result)
{
    std::set<ViolationSignature> signatures;
    for (const Violation& violation : result.violations) {
        signatures.insert(ViolationSignature{violation.ruleId,
                                              violation.ruleSource,
                                              violation.primaryLayer,
                                              violation.secondaryLayer,
                                              violation.relationship,
                                              violation.measuredValue,
                                              violation.requiredValue,
                                              violation.xWindow});
    }
    return signatures;
}

inline CheckRequest requestFor(Network& network,
                                      const PlacedInst& placed)
{
    Node* const node = network.getNode(placed.instanceId);
    EXPECT_NE(node, nullptr);
    if (node == nullptr || node->getMaster() == nullptr) {
        return {};
    }
    const LibCellID master = node->getMaster()->getDbMaster();
    return CheckRequest{
        node,
        GridX(placed.colId),
        GridY(placed.rowId),
        placed.orientation,
        {{OpType::Delete,
          node->getDbInst(),
          UvDist(node->getLeft().v),
          UvDist(node->getBottom().v),
          master,
          master,
          node->getOrient()}}};
}

inline void expectMatches(const ImplantInput& input,
                          const PlacedInst& placed,
                          const CheckResult& fastResult)
{
    SCOPED_TRACE(::testing::Message()
                 << "direct cross-validation request: instance="
                 << placed.instanceId << " master=" << placed.masterId
                 << " row=" << placed.rowId << " col=" << placed.colId
                 << " orientation=" << static_cast<int>(placed.orientation));

    ImplantLayerCheckerHelper helper;
    helper.initialize(input);
    ImplantLayerChecker direct(helper.getGrid(), helper.getDesign(),
        helper.getNetwork());
    helper.initChecker(direct);
    ASSERT_TRUE(direct.getDiags().empty());

    const CheckResult directResult =
        direct.checkDirect(requestFor(*helper.getNetwork(), placed));
    EXPECT_EQ(fastResult.isLegal, directResult.isLegal);
    const std::set<ViolationSignature> fastSignatures = signatures(fastResult);
    const std::set<ViolationSignature> directSignatures =
        signatures(directResult);
    EXPECT_EQ(fastSignatures, directSignatures);
}

// Helper to construct Rect from Dbu values, avoiding the need for
// explicit UvDist/DbuValueInt32 casting in test code.
inline ::Rect makeRect(Dbu xl, Dbu yl, Dbu xh, Dbu yh)
{
    return ::Rect(UvDist(xl), UvDist(yl), UvDist(xh), UvDist(yh));
}

} // namespace directtest
} // namespace ipl
} // namespace dpl2
