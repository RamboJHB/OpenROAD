// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "dpl/Opendp.h"
#include "gtest/gtest.h"
#include "odb/db.h"
#include "odb/defin.h"
#include "odb/lefin.h"
#include "utl/Logger.h"

namespace dpl {
namespace {

template <class T>
using OdbUniquePtr = std::unique_ptr<T, void (*)(T*)>;

struct TestDatabase
{
  TestDatabase(const char* def_file)
      : db(odb::dbDatabase::create(), &odb::dbDatabase::destroy)
  {
    odb::lefin lef_reader(db.get(), &logger, false);
    lib = lef_reader.createTechAndLib("multi_height_fillers",
                                      "multi_height_fillers.lef");
    EXPECT_NE(lib, nullptr);
    if (lib == nullptr) {
      return;
    }

    odb::defin def_reader(db.get(), &logger);
    std::vector<odb::dbLib*> search_libs{lib};
    chip = def_reader.createChip(search_libs, def_file);
    EXPECT_NE(chip, nullptr);
  }

  utl::Logger logger;
  OdbUniquePtr<odb::dbDatabase> db;
  odb::dbLib* lib = nullptr;
  odb::dbChip* chip = nullptr;
};

TEST(FillerPlacementOpenDbTest, ConfiguredMultiHeightInsertionCommitsMetadata)
{
  TestDatabase database("fillers_multi_height.def");
  ASSERT_NE(database.lib, nullptr);
  ASSERT_NE(database.chip, nullptr);

  odb::dbMaster* filler_h2 = database.lib->findMaster("FILLER_X2_H2");
  odb::dbMaster* filler_x1 = database.lib->findMaster("FILLER_X1");
  ASSERT_NE(filler_h2, nullptr);
  ASSERT_NE(filler_x1, nullptr);

  Opendp opendp;
  opendp.init(database.db.get(), &database.logger);
  dbMasterSeq fillers{filler_h2, filler_x1};
  opendp.setFillerPlacementOptions(
      &fillers, "MH_", /* follow_order */ true, /* fit_space */ true);
  opendp.fillerPlacement();

  odb::dbBlock* block = database.chip->getBlock();
  ASSERT_NE(block, nullptr);
  std::vector<std::tuple<std::string, int, int, odb::dbOrientType>> placed;
  for (odb::dbInst* inst : block->getInsts()) {
    int x = 0;
    int y = 0;
    inst->getLocation(x, y);
    EXPECT_EQ(inst->getMaster(), filler_h2);
    EXPECT_EQ(inst->getPlacementStatus(), odb::dbPlacementStatus::PLACED);
    EXPECT_EQ(inst->getSourceType(), odb::dbSourceType::DIST);
    placed.emplace_back(inst->getName(), x, y, inst->getOrient());
  }
  std::sort(placed.begin(), placed.end());

  EXPECT_EQ(placed,
            (std::vector<std::tuple<std::string, int, int, odb::dbOrientType>>{
                {"MH_0_0", 0, 0, odb::dbOrientType::MX},
                {"MH_0_2", 760, 0, odb::dbOrientType::MX},
                {"MH_2_0", 0, 5600, odb::dbOrientType::MX},
                {"MH_2_2", 760, 5600, odb::dbOrientType::MX}}));

  // A second call must refresh occupancy and must not overlap or duplicate the
  // instances created by the first request.
  opendp.fillerPlacement();
  EXPECT_EQ(block->getInsts().size(), 4);
}

TEST(FillerPlacementOpenDbTest, ExactFailureLeavesDatabaseUnchanged)
{
  TestDatabase database("fillers_multi_height_exact_failure.def");
  ASSERT_NE(database.lib, nullptr);
  ASSERT_NE(database.chip, nullptr);

  odb::dbMaster* filler_h2 = database.lib->findMaster("FILLER_X2_H2");
  ASSERT_NE(filler_h2, nullptr);

  Opendp opendp;
  opendp.init(database.db.get(), &database.logger);
  dbMasterSeq fillers{filler_h2};
  opendp.setFillerPlacementOptions(
      &fillers, "FAIL_", /* follow_order */ true, /* fit_space */ true);

  EXPECT_THROW(opendp.fillerPlacement(), std::runtime_error);
  EXPECT_TRUE(database.chip->getBlock()->getInsts().empty());
}

}  // namespace
}  // namespace dpl
