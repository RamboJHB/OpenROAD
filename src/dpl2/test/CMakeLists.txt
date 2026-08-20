cmake_minimum_required(VERSION 3.20)

project(dpl2_filler_repair_tests LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

include(CTest)
include(GoogleTest)

option(DPL2_ENABLE_ASAN "Build fillerRepair tests with AddressSanitizer" OFF)
option(DPL2_TEST_USE_FAKE_UDM "Use the test-only UDM-compatible headers" ON)
if(DPL2_ENABLE_ASAN AND APPLE)
  # Applies to the test executables created by the fillerRepair subdirectory
  # as well as those declared in this harness.
  set(CMAKE_TEST_LAUNCHER
      "${CMAKE_COMMAND};-E;env;ASAN_OPTIONS=detect_container_overflow=0")
endif()
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
find_package(ZLIB REQUIRED)

set(DPL2_SRC "${CMAKE_CURRENT_LIST_DIR}/../src")

# This interface target is the only fake/real UDM switch. Runtime sources
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

# `strictness` is "strict" (-Werror; the fillerRepair payload and test code we
# own) or "delivered" (warnings visible but not fatal; infra/checker sources
# arrive from their own owners and their warning noise must not block us).
function(dpl2_configure_test target standard strictness)
  target_compile_features(${target} PRIVATE "cxx_std_${standard}")
  if(strictness STREQUAL "strict")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
  if(DPL2_ENABLE_ASAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(
        ${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(
        ${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      # Homebrew's prebuilt GoogleTest is not ASan-instrumented. On macOS its
      # libc++ vector annotations otherwise report a container-overflow inside
      # GoogleTest itself while CMake is only listing tests. Keep every other
      # ASan check enabled and wrap both discovery and execution consistently.
      if(APPLE)
        set_property(TARGET ${target} PROPERTY TEST_LAUNCHER
          "${CMAKE_COMMAND};-E;env;ASAN_OPTIONS=detect_container_overflow=0")
      endif()
    else()
      message(FATAL_ERROR "DPL2_ENABLE_ASAN requires a Clang or GNU compiler")
    endif()
  endif()
endfunction()

# Supplied infrastructure/checker sources are compiled exactly as delivered by
# dpl2; only UDM headers/linkage come through dpl2_test_udm. The shared ones
# live in the dpl2_fr_chain_common object library below; the two sources that
# belong to only one consumer (architecture.cpp, ImplantLayerCheckerHelper.cpp)
# stay on that consumer.

function(dpl2_configure_chain_target target strictness)
  # infrastructure/ is its own include root: the delivered sources there
  # include each other bare (<Grid.h>, <Coordinates.h>), as in the
  # destination build.
  target_include_directories(
    ${target}
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/local/include"
            "${DPL2_SRC}" "${DPL2_SRC}/infrastructure"
            "${CMAKE_CURRENT_LIST_DIR}/../include")
  target_include_directories(${target} SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
  target_link_libraries(${target} PRIVATE dpl2_test_udm)
  if(DPL2_ENABLE_ASAN AND APPLE AND EXISTS "/opt/homebrew/opt/tbb/lib/libtbb.a")
    target_link_libraries(${target} PRIVATE "/opt/homebrew/opt/tbb/lib/libtbb.a")
  else()
    target_link_libraries(${target} PRIVATE TBB::tbb)
  endif()
  dpl2_configure_test(${target} 20 ${strictness})
endfunction()

# Everything the fillerRepair payload needs from outside itself: headers for
# UDM/infrastructure/checker, the libraries behind them, and the sanitizer
# flags. src/fillerRepair/CMakeLists.txt picks this up and needs nothing else
# from us, which is the same handshake a destination gets.
add_library(dpl2_filler_repair_deps INTERFACE)
target_include_directories(
  dpl2_filler_repair_deps
  INTERFACE "${CMAKE_CURRENT_LIST_DIR}/local/include"
            "${DPL2_SRC}" "${DPL2_SRC}/infrastructure"
            "${CMAKE_CURRENT_LIST_DIR}/../include")
target_include_directories(
  dpl2_filler_repair_deps SYSTEM INTERFACE ${Boost_INCLUDE_DIRS})
target_link_libraries(dpl2_filler_repair_deps INTERFACE dpl2_test_udm TBB::tbb)
if(DPL2_ENABLE_ASAN)
  target_compile_options(
    dpl2_filler_repair_deps INTERFACE -fsanitize=address
                                      -fno-omit-frame-pointer)
  target_link_options(
    dpl2_filler_repair_deps INTERFACE -fsanitize=address
                                      -fno-omit-frame-pointer)
endif()

# Delivered infrastructure/checker sources, compiled once and shared by the
# executables below.
add_library(dpl2_fr_chain_common OBJECT
    "${DPL2_SRC}/infrastructure/Grid.cpp"
    "${DPL2_SRC}/infrastructure/Object.cpp"
    "${DPL2_SRC}/infrastructure/network.cpp"
    "${DPL2_SRC}/infrastructure/Padding.cpp"
    "${DPL2_SRC}/infrastructure/fillerSetting.cpp"
    "${DPL2_SRC}/drc/util.cpp"
    "${DPL2_SRC}/drc/ImplantLayerChecker.cpp")
dpl2_configure_chain_target(dpl2_fr_chain_common delivered)

# Compile the destination ownership/call chain against the same UDM switch.
# The destination dbToOpendp.cpp calls the install helper compiled through
# DePlace.cpp; its other checker headers are intentionally not faked here.
add_library(dpl2_fr_deplace_compile_check OBJECT
    "${CMAKE_CURRENT_LIST_DIR}/../src/DePlace.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/PlacementDRC.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/Place.cpp")
target_link_libraries(dpl2_fr_deplace_compile_check PRIVATE
    dpl2_fr_chain_common dpl2::fillerRepair)
dpl2_configure_chain_target(dpl2_fr_deplace_compile_check delivered)

# Keep the local diagnostic command on the same public DePlace contract. It is
# object-only because the destination command framework supplies registration
# and Tcl linkage; compiling it here catches stale repair APIs immediately.
add_library(dpl2_fr_command_compile_check OBJECT
    "${CMAKE_CURRENT_LIST_DIR}/../dpl2ui/testFillerRepairCmd.cc")
target_include_directories(
  dpl2_fr_command_compile_check
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../dpl2ui"
          "${CMAKE_CURRENT_LIST_DIR}/../local/command_shim/include")
target_link_libraries(
  dpl2_fr_command_compile_check PRIVATE dpl2_fr_chain_common)
dpl2_configure_chain_target(dpl2_fr_command_compile_check strict)

# The payload builds itself and its portable tests from its own CMakeLists;
# we never list its files. Handing it our already-compiled infra/checker
# objects is exactly what a destination does with DPL2_RUNTIME_LIBRARIES, so
# the portable executables are defined once and used by both.
set(DPL2_FILLER_REPAIR_BUILD_TESTS ON)
set(DPL2_RUNTIME_LIBRARIES dpl2_fr_chain_common)
add_subdirectory("${DPL2_SRC}/fillerRepair" fillerRepair)

# Compile the copy-only migration payload under its destination directory
# name. This prevents fillerRepair2 from drifting behind the exercised source
# while keeping it out of runtime linkage and test behavior.
set(DPL2_FR2_STAGE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/fillerRepair2-stage")
file(MAKE_DIRECTORY "${DPL2_FR2_STAGE_ROOT}/fillerRepair")
file(COPY "${DPL2_SRC}/fillerRepair2/"
     DESTINATION "${DPL2_FR2_STAGE_ROOT}/fillerRepair")
add_library(dpl2_filler_repair2_compile_check OBJECT
    "${DPL2_FR2_STAGE_ROOT}/fillerRepair/RepairPlanner.cpp"
    "${DPL2_FR2_STAGE_ROOT}/fillerRepair/FillerRepairEngine.cpp")
target_include_directories(
  dpl2_filler_repair2_compile_check
  BEFORE PRIVATE "${DPL2_FR2_STAGE_ROOT}")
target_link_libraries(
  dpl2_filler_repair2_compile_check PRIVATE dpl2_filler_repair_deps)
dpl2_configure_chain_target(dpl2_filler_repair2_compile_check strict)

# --- Targets defined ONCE for both UDM modes -------------------------------
#
# dpl2_test_udm is the only fake/real switch, so every target below is built
# from the same sources with the same link closure in both modes. This is what
# makes the destination migration gate testable here: the fake-UDM run we do
# locally exercises the exact target graph a destination builds, and only the
# include dirs / libraries behind dpl2_test_udm differ.
#
# Keeping two divergent branches is how this rotted before: the real-UDM branch
# linked a planner-only source list, so when ImplantLayerChecker began calling
# the externally owned FillerRepairEngine, its
# ctor/dtor/init/update/precheck/repair, that branch stopped linking
# (undefined FillerRepairEngine symbols) and nobody could see it without a
# real UDM. Note a STATIC compile-check library would NOT have caught it --
# archives do not resolve symbols; only linking an executable does.

# architecture.cpp is supplied infrastructure that no test entry point pulls
# in; this keeps it in the compile gate for both modes.
add_library(
  dpl2_filler_repair_compile_check STATIC
  "${DPL2_SRC}/infrastructure/architecture.cpp")
target_link_libraries(
  dpl2_filler_repair_compile_check PRIVATE
  dpl2_fr_chain_common dpl2::fillerRepair)
dpl2_configure_chain_target(dpl2_filler_repair_compile_check delivered)

# Destination checker GoogleTests, built against the same delivered checker
# and test-only UDM switch as the rest of this harness.
add_executable(
  dpl2_implant_checker_simple_test
  "${DPL2_SRC}/drc/ImplantLayerCheckerHelper.cpp"
  "${DPL2_SRC}/drc/test/ImplantLayerCheckerSimpleTests.cpp")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(
    "${DPL2_SRC}/drc/test/ImplantLayerCheckerSimpleTests.cpp"
    PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers")
endif()
target_include_directories(
  dpl2_implant_checker_simple_test
  PRIVATE "${DPL2_SRC}/drc/test")
target_link_libraries(
  dpl2_implant_checker_simple_test
  PRIVATE GTest::gtest_main ZLIB::ZLIB
          dpl2_fr_chain_common dpl2::fillerRepair)
dpl2_configure_chain_target(dpl2_implant_checker_simple_test strict)
gtest_discover_tests(
  dpl2_implant_checker_simple_test
  TEST_PREFIX "checker-simple."
  NO_PRETTY_VALUES
  DISCOVERY_TIMEOUT 30)

add_executable(
  dpl2_placement_drc_lifecycle_test
  "${CMAKE_CURRENT_LIST_DIR}/local/placement_drc_lifecycle_test.cpp"
  "${DPL2_SRC}/PlacementDRC.cpp")
target_link_libraries(
  dpl2_placement_drc_lifecycle_test PRIVATE
  GTest::gtest_main dpl2_fr_chain_common dpl2::fillerRepair)
dpl2_configure_chain_target(dpl2_placement_drc_lifecycle_test strict)
gtest_discover_tests(
  dpl2_placement_drc_lifecycle_test
  TEST_PREFIX "placement-drc."
  DISCOVERY_TIMEOUT 30)

# --- Fake-UDM only ---------------------------------------------------------
# The historical engine regression is the one target that cannot be built
# against a real UDM: its data comes from FakeUdmE2ETestProvider, which
# constructs fake-UDM objects directly. Everything it links is otherwise the
# same runtime chain.
if(DPL2_TEST_USE_FAKE_UDM)
  add_executable(
    dpl2_filler_repair_e2e
    "${CMAKE_CURRENT_LIST_DIR}/local/e2e_cases.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/local/FakeUdmE2ETestProvider.cpp")
  target_link_libraries(dpl2_filler_repair_e2e PRIVATE
    GTest::gtest_main
    dpl2_fr_chain_common dpl2::fillerRepair)
  dpl2_configure_chain_target(dpl2_filler_repair_e2e strict)
  gtest_discover_tests(
    dpl2_filler_repair_e2e
    TEST_PREFIX "e2e."
    NO_PRETTY_VALUES
    DISCOVERY_TIMEOUT 30)
endif()
