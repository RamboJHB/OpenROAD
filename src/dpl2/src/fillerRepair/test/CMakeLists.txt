# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, The OpenROAD Authors
#
# Portable fillerRepair tests. Added by ../CMakeLists.txt when
# DPL2_FILLER_REPAIR_BUILD_TESTS is ON, so it links the module's targets
# instead of re-listing its sources.
#
# Standalone (destination) use:
#   cmake -S <...>/src/fillerRepair -B build \
#         -DDPL2_FILLER_REPAIR_BUILD_TESTS=ON
#   cmake --build build && ctest --test-dir build
#
# Point the payload at a destination's headers/libraries by defining
# dpl2_filler_repair_deps first; supply the infrastructure/checker
# implementation through DPL2_RUNTIME_LIBRARIES so it is linked once instead
# of recompiled here.

include(GoogleTest)

if(NOT DEFINED DPL2_RUNTIME_LIBRARIES)
  set(DPL2_RUNTIME_LIBRARIES "" CACHE STRING
      "Destination targets owning the existing dpl2 infrastructure/checker")
endif()

if(NOT TARGET GTest::gtest_main)
  find_package(GTest REQUIRED)
endif()

set(DPL2_SRC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

# Database-free planner tests. The doubles implement the two seams
# (PlacementView, RepairOracle) without any UDM fixture or provider.
# One self-contained source: the seam doubles and the synthetic catalog are
# folded into it, so the database-free suite is a single file to carry.
add_executable(dpl2_filler_repair_planner_test RepairPlannerTest.cpp)
target_link_libraries(
  dpl2_filler_repair_planner_test
  PRIVATE GTest::gtest_main dpl2::fillerRepairPlanner)
target_compile_options(
  dpl2_filler_repair_planner_test PRIVATE -Wall -Wextra)
gtest_discover_tests(
  dpl2_filler_repair_planner_test
  TEST_PREFIX "planner."
  DISCOVERY_TIMEOUT 30)

# Real-checker E2E. Prefer the destination's own infrastructure/checker
# targets; with none supplied, compile the sibling sources as a fallback so
# the suite still runs standalone.
set(DPL2_CHECKER_HELPER_SOURCES
    "${DPL2_SRC_ROOT}/drc/ImplantLayerCheckerHelper.cpp")
if(NOT DPL2_RUNTIME_LIBRARIES)
  list(PREPEND DPL2_CHECKER_HELPER_SOURCES
      "${DPL2_SRC_ROOT}/infrastructure/Grid.cpp"
      "${DPL2_SRC_ROOT}/infrastructure/Object.cpp"
      "${DPL2_SRC_ROOT}/infrastructure/network.cpp"
      "${DPL2_SRC_ROOT}/infrastructure/architecture.cpp"
      "${DPL2_SRC_ROOT}/infrastructure/Padding.cpp"
      "${DPL2_SRC_ROOT}/infrastructure/fillerSetting.cpp"
      "${DPL2_SRC_ROOT}/drc/util.cpp"
      "${DPL2_SRC_ROOT}/drc/ImplantLayerChecker.cpp")
endif()

add_executable(
  dpl2_filler_repair_checker_e2e
  ${DPL2_CHECKER_HELPER_SOURCES}
  FillerRepairCheckerE2ETest.cpp)
target_link_libraries(
  dpl2_filler_repair_checker_e2e
  PRIVATE GTest::gtest_main dpl2::fillerRepair ${DPL2_RUNTIME_LIBRARIES})
# Delivered infrastructure/checker sources compile here as-is; their warnings
# belong to their owners and must not fail this build.
target_compile_options(
  dpl2_filler_repair_checker_e2e PRIVATE -Wall -Wextra)
target_include_directories(
  dpl2_filler_repair_checker_e2e PRIVATE "${DPL2_SRC_ROOT}/infrastructure")
gtest_discover_tests(
  dpl2_filler_repair_checker_e2e
  TEST_PREFIX "portable-e2e."
  DISCOVERY_TIMEOUT 30)
