// Checker transaction geometry and vacated-site DRC, without database mutation.
#include <drc/ImplantLayerChecker.h>
#include <drc/ImplantLayerCheckerHelper.h>
#include <gtest/gtest.h>
#include <infrastructure/Grid.h>
#include <infrastructure/network.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace dpl2::ipl {
namespace {

Rect box(int xl, int yl, int xh, int yh)
{
    return Rect(UvDist(xl), UvDist(yl), UvDist(xh), UvDist(yh));
}

MasterItem master(int id, int width, bool filler, int family = 0)
{
    MasterItem item;
    item.masterId = id;
    item.width = width;
    item.height = 8;
    item.siteHeight = 8;
    item.isFiller = filler;
    item.shapes = {{id, 2 * id, family, box(0, 0, width, 4)},
                   {id, 2 * id + 1, family + 2, box(0, 4, width, 8)}};
    item.rawShapes = item.shapes;
    return item;
}

struct Fixture
{
    ImplantLayerCheckerHelper helper;
    std::unique_ptr<ImplantLayerChecker> checker;

    explicit Fixture(bool checkWidths = false)
    {
        ImplantInput input;
        input.layers = {{0, "F1_N", Layer::Vt::L, Layer::Polar::N},
                        {1, "F2_N", Layer::Vt::H, Layer::Polar::N},
                        {2, "F1_P", Layer::Vt::L, Layer::Polar::P},
                        {3, "F2_P", Layer::Vt::H, Layer::Polar::P}};
        input.masters = {master(0, 4, false), master(1, 2, false),
                         master(2, 6, false), master(3, 1, true),
                         master(4, 1, true, 1)};
        input.rowCount = 5;
        input.colCount = 20;
        input.siteWidth = 1;
        input.rowHeight = 8;
        input.basePolar = Layer::Polar::N;
        input.fillerSetting.present = true;
        input.fillerSetting.fillerMasterIds = {3, 4};
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 20; ++col) {
                if (row == 2 && col > 4 && col < 8) {
                    continue;
                }
                input.placedInsts.push_back({row * 100 + col,
                    row == 2 && col == 4 ? 0 : 3, row, col,
                    row % 2 == 0 ? PhysOrientationE::R0 : PhysOrientationE::MX,
                    row != 2 || col != 4});
            }
        }
        if (checkWidths) {
            for (int layer = 0; layer < 4; ++layer) {
                input.rules.emplace_back(layer, RuleSource::Width, layer, 4);
            }
        }
        helper.initialize(input);
        checker = std::make_unique<ImplantLayerChecker>(helper.getGrid(), nullptr, helper.getNetwork());
        helper.initChecker(*checker);
    }

    Node* node(int id) const { return helper.getNetwork()->getNode(id); }
    CellChangeRecord remove(int id) const
    {
        const Node* old = node(id);
        return {OpType::Delete, old->getDbInst(), UvDist(old->getLeft().v),
            UvDist(old->getBottom().v), old->getMaster()->getDbMaster(),
            old->getMaster()->getDbMaster(), old->getOrient()};
    }
    CellChangeRecord add(int col, int row, int index, int masterId = 3) const
    {
        return {OpType::Add, std::string("GAP_") + std::to_string(index),
            UvDist(col), UvDist(row * 8), LibCellID(),
            helper.getNetwork()->getMaster(masterId)->getDbMaster(),
            row % 2 == 0 ? PhysOrientationE::R0 : PhysOrientationE::MX};
    }
    Node target(int masterId) const
    {
        Node target;
        target.setId(node(204)->getId());
        target.setDbInst(node(204)->getDbInst());
        target.setMaster(helper.getNetwork()->getMaster(masterId));
        target.setType(Node::CELL);
        target.setWidth(DbuX{masterId == 1 ? 2 : 6});
        target.setHeight(DbuY{8});
        return target;
    }
};

TEST(CheckerResize, ShrinkRequiresTheEntireReleasedStandardCellArea)
{
    Fixture f;
    Node target = f.target(1);
    const CheckRequest request{&target, GridX{4}, GridY{2}, PhysOrientationE::R0, {f.remove(204)}};
    const FillerChanges fill{f.add(6, 2, 0), f.add(7, 2, 1)};
    const auto results = f.checker->checkPlaceWithOverlays(request, box(0, 0, 20, 40), {{}, fill});
    ASSERT_EQ(results.size(), 2u);
    EXPECT_FALSE(results[0].isLegal);
    EXPECT_TRUE(results[1].isLegal);
    EXPECT_EQ(f.node(204)->getMaster()->getId(), 0);
    EXPECT_EQ(f.helper.getGrid()->gridPixel(GridX{7}, GridY{2})->cell, f.node(204));
}

TEST(CheckerResize, GrowthConsumesOnlyExplicitlySelectedIntersectingFillers)
{
    Fixture f;
    Node target = f.target(2);
    CheckRequest request{&target, GridX{4}, GridY{2}, PhysOrientationE::R0,
        {f.remove(204), f.remove(208), f.remove(209)}};
    EXPECT_TRUE(f.checker->checkDirect(request).isLegal);
    auto results = f.checker->checkPlaceWithOverlays(request, box(0, 0, 20, 40), {{}});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].isLegal);
    request.overlayChanges.push_back(f.remove(219));
    EXPECT_FALSE(f.checker->checkDirect(request).isLegal);
}

TEST(CheckerResize, DisjointMoveChecksAndFillsTheOldLocation)
{
    Fixture f;
    Node target = f.target(1);
    const CheckRequest request{&target, GridX{12}, GridY{0}, PhysOrientationE::R0,
        {f.remove(204), f.remove(12), f.remove(13)}};
    FillerChanges fill;
    for (int col = 4; col < 8; ++col) {
        fill.push_back(f.add(col, 2, col));
    }
    const auto results = f.checker->checkPlaceWithOverlays(request, box(12, 0, 14, 8), {fill});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].isLegal);
    EXPECT_EQ(f.node(204)->getLeft().v, 4);
    EXPECT_EQ(f.node(204)->getBottom().v, 16);
    EXPECT_EQ(f.helper.getGrid()->gridPixel(GridX{12}, GridY{0})->cell, f.node(12));
}

TEST(CheckerResize, RemovalOnlyHasNoTemporaryTargetAndRequiresExactRefill)
{
    Fixture f;
    const CheckRequest request{nullptr, GridX{0}, GridY{0}, PhysOrientationE::R0, {f.remove(204)}};
    FillerChanges fill;
    for (int col = 4; col < 8; ++col) {
        fill.push_back(f.add(col, 2, col));
    }
    const auto results = f.checker->checkPlaceWithOverlays(request, box(4, 16, 8, 24), {{}, fill});
    ASSERT_EQ(results.size(), 2u);
    EXPECT_FALSE(results[0].isLegal);
    EXPECT_TRUE(results[1].isLegal);
    EXPECT_FALSE(f.checker->checkDirect(request).isLegal);
}

TEST(CheckerResize, VacatedSiteViolationsCannotHideOutsideTheNewTargetGuard)
{
    Fixture f(true);
    Node target = f.target(1);
    const CheckRequest request{&target, GridX{12}, GridY{0}, PhysOrientationE::R0,
        {f.remove(204), f.remove(12), f.remove(13)}};
    FillerChanges fill;
    for (int col = 4; col < 8; ++col) {
        fill.push_back(f.add(col, 2, col, col == 5 ? 4 : 3));
    }
    const auto results = f.checker->checkPlaceWithOverlays(request, box(12, 0, 14, 8), {fill});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].isLegal);
    EXPECT_TRUE(std::any_of(results[0].violations.begin(), results[0].violations.end(),
        [](const Violation& violation) {
            return std::find(violation.rowIds.begin(), violation.rowIds.end(), 2) != violation.rowIds.end();
        }));
}

TEST(CheckerResize, CandidateBaselineDoesNotDependOnOtherBatchMembers)
{
    Fixture f(true);
    for (const int id : {108, 208, 209, 210, 211}) {
        f.node(id)->setMaster(f.helper.getNetwork()->getMaster(4));
    }
    const CheckRequest request{nullptr, GridX{4}, GridY{2}, PhysOrientationE::R0, {f.remove(204)}};
    FillerChanges first;
    FillerChanges second;
    for (int offset = 0; offset < 4; ++offset) {
        first.push_back(f.add(4 + offset, 2, offset));
        second.push_back(f.add(4 + offset, 2, offset, 4));
    }
    auto firstSwap = f.remove(208);
    firstSwap.op_ = OpType::Replace;
    firstSwap.new_lib_cell_ = f.helper.getNetwork()->getMaster(4)->getDbMaster();
    first.push_back(firstSwap);
    auto secondSwap = f.remove(211);
    secondSwap.op_ = OpType::Replace;
    secondSwap.new_lib_cell_ = f.helper.getNetwork()->getMaster(3)->getDbMaster();
    second.push_back(secondSwap);

    const auto firstOnly = f.checker->checkPlaceWithOverlays(request, box(0, 0, 20, 40), {first});
    const auto secondOnly = f.checker->checkPlaceWithOverlays(request, box(0, 0, 20, 40), {second});
    ASSERT_TRUE(firstOnly.front().isLegal);
    for (bool reverse : {false, true}) {
        const auto batch = f.checker->checkPlaceWithOverlays(request, box(0, 0, 20, 40),
            reverse ? std::vector<FillerChanges>{second, first} : std::vector<FillerChanges>{first, second});
        ASSERT_EQ(batch.size(), 2u);
        const auto& checkedFirst = batch[reverse ? 1 : 0];
        const auto& checkedSecond = batch[reverse ? 0 : 1];
        EXPECT_EQ(checkedFirst.isLegal, firstOnly.front().isLegal);
        EXPECT_EQ(checkedFirst.violations.size(), firstOnly.front().violations.size());
        EXPECT_EQ(checkedSecond.isLegal, secondOnly.front().isLegal);
        EXPECT_EQ(checkedSecond.violations.size(), secondOnly.front().violations.size());
    }
}

} // namespace
} // namespace dpl2::ipl
