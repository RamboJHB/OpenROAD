cmake_minimum_required(VERSION 3.20)

project(dpl2_filler_repair_tests LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

include(CTest)
include(GoogleTest)

option(DPL2_ENABLE_ASAN "Build fillerRepair tests with AddressSanitizer" OFF)
option(DPL2_TEST_USE_FAKE_UDM "Use the test-only UDM-compatible headers" ON)
set(DPL2_TEST_FAKE_UDM_INCLUDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/local/fake_udm/include" CACHE PATH
    "Test-only UDM-compatible include root")
set(DPL2_TEST_UDM_INCLUDE_DIRS "" CACHE STRING
    "Real UDM include directories used when DPL2_TEST_USE_FAKE_UDM=OFF")
set(DPL2_TEST_UDM_LIBRARIES "" CACHE STRING
    "Real UDM libraries/targets used when DPL2_TEST_USE_FAKE_UDM=OFF")

find_package(GTest REQUIRED)
find_package(Boost REQUIRED)
find_package(TBB REQUIRED)

set(DPL2_SRC "${CMAKE_CURRENT_LIST_DIR}/../src")
include("${DPL2_SRC}/fillerRepair/sources.cmake")

set(DPL2_LOCAL_PLANNER_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/local/planner/FakeImplantChecker.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/local/planner/FakeUdmCandidateProvider.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/local/planner/planner_cases.cpp")

# This interface target is the only fake/real UDM switch. Production sources
# contain no fake include, compile definition, or conditional code. The fake
# headers mirror the real eUNL/eLIB/ePHY/eUTL names and signatures and are used
# solely to construct deterministic test data.
add_library(dpl2_test_udm INTERFACE)
if(DPL2_TEST_USE_FAKE_UDM)
  target_include_directories(
    dpl2_test_udm
    SYSTEM INTERFACE "${DPL2_TEST_FAKE_UDM_INCLUDE_DIR}")
else()
  if(NOT DPL2_TEST_UDM_INCLUDE_DIRS AND NOT DPL2_TEST_UDM_LIBRARIES)
    message(FATAL_ERROR
      "Set DPL2_TEST_UDM_INCLUDE_DIRS and/or DPL2_TEST_UDM_LIBRARIES when "
      "DPL2_TEST_USE_FAKE_UDM=OFF")
  endif()
  target_include_directories(
    dpl2_test_udm INTERFACE ${DPL2_TEST_UDM_INCLUDE_DIRS})
  target_link_libraries(
    dpl2_test_udm INTERFACE ${DPL2_TEST_UDM_LIBRARIES})
endif()

function(dpl2_configure_test target standard)
  target_compile_features(${target} PRIVATE "cxx_std_${standard}")
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  if(DPL2_ENABLE_ASAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(
        ${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(
        ${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    else()
      message(FATAL_ERROR "DPL2_ENABLE_ASAN requires a Clang or GNU compiler")
    endif()
  endif()
endfunction()

add_executable(
  dpl2_filler_repair_planner_test
  ${DPL2_FILLER_REPAIR_PLANNER_SOURCES}
  ${DPL2_LOCAL_PLANNER_TEST_SOURCES})
target_include_directories(dpl2_filler_repair_planner_test PRIVATE "${DPL2_SRC}")
target_link_libraries(dpl2_filler_repair_planner_test PRIVATE GTest::gtest_main)
dpl2_configure_test(dpl2_filler_repair_planner_test 17)
gtest_discover_tests(
  dpl2_filler_repair_planner_test
  TEST_PREFIX "planner."
  DISCOVERY_TIMEOUT 30)

# Supplied infrastructure/checker sources, compiled exactly as delivered by
# dpl2; only UDM headers/linkage come through dpl2_test_udm.
set(DPL2_SUPPLIED_CHAIN_SOURCES
    "${DPL2_SRC}/infrastructure/Grid.cpp"
    "${DPL2_SRC}/infrastructure/Object.cpp"
    "${DPL2_SRC}/infrastructure/network.cpp"
    "${DPL2_SRC}/infrastructure/architecture.cpp"
    "${DPL2_SRC}/infrastructure/Padding.cpp"
    "${DPL2_SRC}/infrastructure/fillerSetting.cpp"
    "${DPL2_SRC}/drc/ImplantLayerChecker.cpp")

function(dpl2_configure_chain_target target)
  target_include_directories(
    ${target}
    PRIVATE "${DPL2_SRC}" "${CMAKE_CURRENT_LIST_DIR}/../include")
  target_include_directories(${target} SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
  target_link_libraries(${target} PRIVATE dpl2_test_udm)
  if(DPL2_ENABLE_ASAN AND APPLE AND EXISTS "/opt/homebrew/opt/tbb/lib/libtbb.a")
    target_link_libraries(${target} PRIVATE "/opt/homebrew/opt/tbb/lib/libtbb.a")
  else()
    target_link_libraries(${target} PRIVATE TBB::tbb)
  endif()
  dpl2_configure_test(${target} 20)
endfunction()

if(DPL2_TEST_USE_FAKE_UDM)
  # Standalone E2E: fake UDM provides the test DATA, everything else is the
  # production chain.
  add_executable(
    dpl2_filler_repair_e2e
    ${DPL2_SUPPLIED_CHAIN_SOURCES}
    ${DPL2_FILLER_REPAIR_PRODUCTION_SOURCES}
    ${DPL2_FILLER_REPAIR_E2E_CASE_SOURCE}
    "${CMAKE_CURRENT_LIST_DIR}/local/FakeUdmE2ETestProvider.cpp")
  target_link_libraries(dpl2_filler_repair_e2e PRIVATE GTest::gtest_main)
  dpl2_configure_chain_target(dpl2_filler_repair_e2e)
  gtest_discover_tests(
    dpl2_filler_repair_e2e
    TEST_PREFIX "e2e."
    NO_PRETTY_VALUES
    DISCOVERY_TIMEOUT 30)
else()
  # Real-UDM mode: the E2E's data provider IS the fake UDM, so there is no
  # runnable E2E here. Instead verify that every supplied and production
  # source compiles and links against the real UDM headers -- the migration
  # gate for the destination environment.
  add_library(
    dpl2_filler_repair_compile_check STATIC
    ${DPL2_SUPPLIED_CHAIN_SOURCES}
    ${DPL2_FILLER_REPAIR_PRODUCTION_SOURCES})
  dpl2_configure_chain_target(dpl2_filler_repair_compile_check)

  # Compile every provider-neutral E2E assertion against real UDM as an
  # object library. The destination links these objects with its concrete
  # RealUdmE2ETestProvider and production owning target to run them.
  add_library(
    dpl2_filler_repair_real_udm_e2e_cases OBJECT
    ${DPL2_FILLER_REPAIR_E2E_CASE_SOURCE})
  target_link_libraries(
    dpl2_filler_repair_real_udm_e2e_cases PRIVATE GTest::gtest)
  dpl2_configure_chain_target(dpl2_filler_repair_real_udm_e2e_cases)
endif()
