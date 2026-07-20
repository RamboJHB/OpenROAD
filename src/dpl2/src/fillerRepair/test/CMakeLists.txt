cmake_minimum_required(VERSION 3.20)

project(dpl2_filler_repair_tests LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

include(CTest)
include(GoogleTest)

option(DPL2_ENABLE_ASAN "Build tests with AddressSanitizer" OFF)
set(DPL2_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Directory containing fillerRepair/, infrastructure/ and drc/")
set(DPL2_DPL2_INCLUDE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../include" CACHE PATH
    "Directory containing dpl2/DePlace.h")
set(DPL2_UDM_INCLUDE_DIRS "" CACHE STRING
    "Destination UDM include directories")
set(DPL2_UDM_LIBRARIES "" CACHE STRING
    "Destination UDM libraries or CMake targets")
set(DPL2_RUNTIME_LIBRARIES "" CACHE STRING
    "Destination targets owning the existing dpl2 infrastructure/checker")

find_package(GTest REQUIRED)
find_package(Boost REQUIRED)
find_package(TBB REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/../sources.cmake")

# Database-free planner tests. The test doubles consume the same final wire
# types as the checker but do not include a fake UDM fixture or provider.
add_executable(
  dpl2_filler_repair_planner_test
  ${DPL2_FILLER_REPAIR_PLANNER_SOURCES}
  ${DPL2_FILLER_REPAIR_PORTABLE_PLANNER_TEST_SOURCES})
target_compile_features(dpl2_filler_repair_planner_test PRIVATE cxx_std_17)
target_compile_options(
  dpl2_filler_repair_planner_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  dpl2_filler_repair_planner_test
  PRIVATE
    "${DPL2_SOURCE_ROOT}"
    "${DPL2_DPL2_INCLUDE_ROOT}"
    ${DPL2_UDM_INCLUDE_DIRS})
target_include_directories(
  dpl2_filler_repair_planner_test SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(
  dpl2_filler_repair_planner_test
  PRIVATE GTest::gtest_main ${DPL2_UDM_LIBRARIES})

if(DPL2_ENABLE_ASAN)
  target_compile_options(
    dpl2_filler_repair_planner_test
    PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(
    dpl2_filler_repair_planner_test
    PRIVATE -fsanitize=address -fno-omit-frame-pointer)
endif()

gtest_discover_tests(
  dpl2_filler_repair_planner_test
  TEST_PREFIX "planner."
  DISCOVERY_TIMEOUT 30)

# Prefer the destination's existing runtime targets so its exact infrastructure
# and checker implementation are linked once. With no target supplied, compile
# the sibling sources as a standalone fallback.
set(DPL2_CHECKER_HELPER_SOURCES
    "${DPL2_SOURCE_ROOT}/drc/ImplantLayerCheckerHelper.cpp")
if(NOT DPL2_RUNTIME_LIBRARIES)
  list(PREPEND DPL2_CHECKER_HELPER_SOURCES
      "${DPL2_SOURCE_ROOT}/infrastructure/Grid.cpp"
      "${DPL2_SOURCE_ROOT}/infrastructure/Object.cpp"
      "${DPL2_SOURCE_ROOT}/infrastructure/network.cpp"
      "${DPL2_SOURCE_ROOT}/infrastructure/architecture.cpp"
      "${DPL2_SOURCE_ROOT}/infrastructure/Padding.cpp"
      "${DPL2_SOURCE_ROOT}/infrastructure/fillerSetting.cpp"
      "${DPL2_SOURCE_ROOT}/drc/ImplantLayerChecker.cpp")
endif()

add_executable(
  dpl2_filler_repair_checker_e2e
  ${DPL2_CHECKER_HELPER_SOURCES}
  ${DPL2_FILLER_REPAIR_SOURCES}
  ${DPL2_FILLER_REPAIR_PORTABLE_E2E_SOURCE})

target_compile_features(dpl2_filler_repair_checker_e2e PRIVATE cxx_std_20)
target_compile_options(
  dpl2_filler_repair_checker_e2e PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  dpl2_filler_repair_checker_e2e
  PRIVATE
    "${DPL2_SOURCE_ROOT}"
    "${DPL2_DPL2_INCLUDE_ROOT}"
    ${DPL2_UDM_INCLUDE_DIRS})
target_include_directories(
  dpl2_filler_repair_checker_e2e SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(
  dpl2_filler_repair_checker_e2e
  PRIVATE GTest::gtest_main TBB::tbb ${DPL2_RUNTIME_LIBRARIES}
          ${DPL2_UDM_LIBRARIES})

if(DPL2_ENABLE_ASAN)
  target_compile_options(
    dpl2_filler_repair_checker_e2e
    PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(
    dpl2_filler_repair_checker_e2e
    PRIVATE -fsanitize=address -fno-omit-frame-pointer)
endif()

gtest_discover_tests(
  dpl2_filler_repair_checker_e2e
  TEST_PREFIX "filler-repair-checker."
  DISCOVERY_TIMEOUT 30)
