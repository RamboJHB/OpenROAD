cmake_minimum_required(VERSION 3.20)

project(dpl2_filler_repair_real_udm_tests LANGUAGES CXX)

include(CTest)
include(GoogleTest)

option(DPL2_ENABLE_ASAN "Build real-UDM tests with AddressSanitizer" OFF)
set(DPL2_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Directory containing fillerRepair/, infrastructure/ and drc/")
set(DPL2_DPL2_INCLUDE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../include" CACHE PATH
    "Directory containing dpl2/DePlace.h")
set(DPL2_REAL_UDM_INCLUDE_DIRS "" CACHE STRING
    "Real UDM include directories")
set(DPL2_REAL_UDM_LIBRARIES "" CACHE STRING
    "Real UDM libraries or CMake targets")
set(DPL2_REAL_UDM_PROVIDER_SOURCE "" CACHE FILEPATH
    "Destination implementation of E2ETestProvider; enables runnable E2E")

find_package(GTest REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/../sources.cmake")

function(dpl2_real_udm_test_options target standard)
  target_compile_features(${target} PRIVATE "cxx_std_${standard}")
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  if(DPL2_ENABLE_ASAN)
    target_compile_options(
      ${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(
      ${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  endif()
endfunction()

if(NOT DPL2_REAL_UDM_INCLUDE_DIRS AND NOT DPL2_REAL_UDM_LIBRARIES)
    message(FATAL_ERROR
      "Real-UDM mode requires DPL2_REAL_UDM_INCLUDE_DIRS and/or "
      "DPL2_REAL_UDM_LIBRARIES")
endif()

  find_package(Boost REQUIRED)
  find_package(TBB REQUIRED)

  add_library(dpl2_real_udm INTERFACE)
  target_include_directories(
    dpl2_real_udm INTERFACE ${DPL2_REAL_UDM_INCLUDE_DIRS})
  target_link_libraries(
    dpl2_real_udm INTERFACE ${DPL2_REAL_UDM_LIBRARIES})

  set(DPL2_SUPPLIED_CHAIN_SOURCES
      "${DPL2_SOURCE_ROOT}/infrastructure/Grid.cpp"
      "${DPL2_SOURCE_ROOT}/infrastructure/Object.cpp"
      "${DPL2_SOURCE_ROOT}/infrastructure/network.cpp"
      "${DPL2_SOURCE_ROOT}/infrastructure/architecture.cpp"
      "${DPL2_SOURCE_ROOT}/infrastructure/Padding.cpp"
      "${DPL2_SOURCE_ROOT}/infrastructure/fillerSetting.cpp"
      "${DPL2_SOURCE_ROOT}/drc/ImplantLayerChecker.cpp")

  function(dpl2_real_udm_target target)
    target_include_directories(
      ${target} PRIVATE "${DPL2_SOURCE_ROOT}" "${DPL2_DPL2_INCLUDE_ROOT}")
    target_include_directories(${target} SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE dpl2_real_udm TBB::tbb)
    dpl2_real_udm_test_options(${target} 20)
  endfunction()

  # Compile every production and assertion source against REAL UDM even when
  # the destination has not yet connected its fixture builder.
  add_library(
    dpl2_filler_repair_real_udm_compile_check STATIC
    ${DPL2_SUPPLIED_CHAIN_SOURCES}
    ${DPL2_FILLER_REPAIR_PRODUCTION_SOURCES})
  dpl2_real_udm_target(dpl2_filler_repair_real_udm_compile_check)

  add_library(
    dpl2_filler_repair_real_udm_e2e_cases OBJECT
    ${DPL2_FILLER_REPAIR_E2E_CASE_SOURCE})
  target_link_libraries(
    dpl2_filler_repair_real_udm_e2e_cases PRIVATE GTest::gtest)
  dpl2_real_udm_target(dpl2_filler_repair_real_udm_e2e_cases)

  # A real UDM fixture implementation is necessarily owned by the destination
  # (design creation/loading APIs are not part of fillerRepair). Once supplied,
  # the exact same 52 case objects become a discovered runnable executable.
  if(DPL2_REAL_UDM_PROVIDER_SOURCE)
    add_executable(
      dpl2_filler_repair_real_udm_e2e
      ${DPL2_SUPPLIED_CHAIN_SOURCES}
      ${DPL2_FILLER_REPAIR_PRODUCTION_SOURCES}
      ${DPL2_FILLER_REPAIR_E2E_CASE_SOURCE}
      "${DPL2_REAL_UDM_PROVIDER_SOURCE}")
    target_link_libraries(
      dpl2_filler_repair_real_udm_e2e PRIVATE GTest::gtest_main)
    dpl2_real_udm_target(dpl2_filler_repair_real_udm_e2e)
    gtest_discover_tests(
      dpl2_filler_repair_real_udm_e2e
      TEST_PREFIX "real-udm."
      NO_PRETTY_VALUES
      DISCOVERY_TIMEOUT 30)
  endif()
