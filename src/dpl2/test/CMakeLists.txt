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
# headers model eUNL/eLIB/ePHY/eUTL queries and deterministic test mutations.
# The local Add/placement change contracts need destination UDM ABI validation.
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
    "${CMAKE_CURRENT_LIST_DIR}/../src/dbToOpendp.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/PlacementDRC.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/Place.cpp")
target_link_libraries(dpl2_fr_deplace_compile_check PRIVATE
    dpl2_fr_chain_common dpl2::fillerRepair)
dpl2_configure_chain_target(dpl2_fr_deplace_compile_check delivered)

# Keep the command on the same public DePlace contract. The local shim supplies
# its option framework; the commit executable also links and runs this object
# without the destination's Tcl registration layer.
add_library(dpl2_fr_command_compile_check OBJECT
    "${CMAKE_CURRENT_LIST_DIR}/../dpl2ui/testFillerRepairCmd.cc")
target_include_directories(
  dpl2_fr_command_compile_check
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../dpl2ui"
          "${CMAKE_CURRENT_LIST_DIR}/../local/command_shim/include")
target_link_libraries(
  dpl2_fr_command_compile_check PRIVATE dpl2_fr_chain_common)
dpl2_configure_chain_target(dpl2_fr_command_compile_check strict)

# The payload builds itself and its migration tests from its own CMakeLists;
# we never list its files. Handing it our already-compiled infra/checker
# objects is exactly what a destination does with DPL2_RUNTIME_LIBRARIES, so
# the executables are defined once and used by both.
set(DPL2_FILLER_REPAIR_BUILD_TESTS ON)
set(DPL2_RUNTIME_LIBRARIES dpl2_fr_chain_common)
add_subdirectory("${DPL2_SRC}/fillerRepair" fillerRepair)

# Compile the copy-only migration payload under its destination directory
# name. This prevents fillerRepair2 from drifting behind the exercised source
# while keeping it out of runtime linkage and test behavior.
foreach(shared_file IN ITEMS FillerRepairEngine.cpp FillerRepairEngine.h
                             RepairPlanner.cpp RepairPlanner.h PlacementView.h
                             FillerRetiler.cpp FillerRetiler.h)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${DPL2_SRC}/fillerRepair/${shared_file}"
            "${DPL2_SRC}/fillerRepair2/${shared_file}"
    RESULT_VARIABLE differs)
  if(NOT differs EQUAL 0)
    message(FATAL_ERROR "fillerRepair2 is out of sync: ${shared_file}")
  endif()
endforeach()
set(DPL2_FR2_STAGE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/fillerRepair2-stage")
file(MAKE_DIRECTORY "${DPL2_FR2_STAGE_ROOT}/fillerRepair")
file(GLOB_RECURSE fr2_files CONFIGURE_DEPENDS
     RELATIVE "${DPL2_SRC}/fillerRepair2" "${DPL2_SRC}/fillerRepair2/*")
foreach(fr2_file IN LISTS fr2_files)
  configure_file("${DPL2_SRC}/fillerRepair2/${fr2_file}"
                 "${DPL2_FR2_STAGE_ROOT}/fillerRepair/${fr2_file}" COPYONLY)
endforeach()
add_library(dpl2_filler_repair2_compile_check OBJECT
    "${DPL2_FR2_STAGE_ROOT}/fillerRepair/RepairPlanner.cpp"
    "${DPL2_FR2_STAGE_ROOT}/fillerRepair/FillerRetiler.cpp"
    "${DPL2_FR2_STAGE_ROOT}/fillerRepair/FillerRepairEngine.cpp")
target_include_directories(
  dpl2_filler_repair2_compile_check
  BEFORE PRIVATE "${DPL2_FR2_STAGE_ROOT}")
target_link_libraries(
  dpl2_filler_repair2_compile_check PRIVATE dpl2_filler_repair_deps)
dpl2_configure_chain_target(dpl2_filler_repair2_compile_check strict)

add_executable(
  dpl2_filler_repair2_integration_test
  "${DPL2_FR2_STAGE_ROOT}/fillerRepair/test/FillerRepairIntegrationTest.cpp"
  "${DPL2_SRC}/drc/ImplantLayerCheckerHelper.cpp")
target_include_directories(
  dpl2_filler_repair2_integration_test
  BEFORE PRIVATE "${DPL2_FR2_STAGE_ROOT}")
target_link_libraries(
  dpl2_filler_repair2_integration_test
  PRIVATE GTest::gtest_main ZLIB::ZLIB dpl2_fr_chain_common
          dpl2_filler_repair2_compile_check)
dpl2_configure_chain_target(dpl2_filler_repair2_integration_test strict)
gtest_discover_tests(
  dpl2_filler_repair2_integration_test
  TEST_PREFIX "fr2-integration."
  DISCOVERY_TIMEOUT 30
  PROPERTIES ENVIRONMENT "FR_VERBOSE=1")

# Internal-only migration test: compile the planner separately and exercise it
# through PlacementView/RepairOracle doubles, without checker/engine objects.
add_library(
  dpl2_filler_repair2_internal_impl OBJECT
  "${DPL2_FR2_STAGE_ROOT}/fillerRepair/RepairPlanner.cpp"
  "${DPL2_FR2_STAGE_ROOT}/fillerRepair/FillerRetiler.cpp")
target_include_directories(
  dpl2_filler_repair2_internal_impl
  BEFORE PRIVATE "${DPL2_FR2_STAGE_ROOT}")
target_link_libraries(
  dpl2_filler_repair2_internal_impl PRIVATE dpl2_filler_repair_deps)
dpl2_configure_chain_target(dpl2_filler_repair2_internal_impl strict)

add_executable(
  dpl2_filler_repair2_internal_test
  "${DPL2_FR2_STAGE_ROOT}/fillerRepair/test/FillerRepairInternalTest.cpp")
target_include_directories(
  dpl2_filler_repair2_internal_test
  BEFORE PRIVATE "${DPL2_FR2_STAGE_ROOT}")
target_link_libraries(
  dpl2_filler_repair2_internal_test
  PRIVATE GTest::gtest_main dpl2_filler_repair2_internal_impl)
dpl2_configure_chain_target(dpl2_filler_repair2_internal_test strict)
gtest_discover_tests(
  dpl2_filler_repair2_internal_test
  TEST_PREFIX "fr2-internal."
  DISCOVERY_TIMEOUT 30
  PROPERTIES ENVIRONMENT "FR_VERBOSE=1")

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
  DISCOVERY_TIMEOUT 30
  PROPERTIES ENVIRONMENT "FR_VERBOSE=1")

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
  DISCOVERY_TIMEOUT 30
  PROPERTIES ENVIRONMENT "FR_VERBOSE=1")

# --- Fake-UDM only ---------------------------------------------------------
# The historical engine regression is the one target that cannot be built
# against a real UDM: its data comes from FakeUdmE2ETestProvider, which
# constructs fake-UDM objects directly. Everything it links is otherwise the
# same runtime chain.
if(DPL2_TEST_USE_FAKE_UDM)
  add_executable(dpl2_commit_test
    "${CMAKE_CURRENT_LIST_DIR}/local/commit_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/local/FakeUdmE2ETestProvider.cpp")
  target_include_directories(dpl2_commit_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../dpl2ui"
    "${CMAKE_CURRENT_LIST_DIR}/../local/command_shim/include")
  target_link_libraries(dpl2_commit_test PRIVATE
    GTest::gtest_main dpl2_fr_deplace_compile_check
    dpl2_fr_command_compile_check dpl2_filler_repair_compile_check
    dpl2_fr_chain_common dpl2::fillerRepair)
  dpl2_configure_chain_target(dpl2_commit_test strict)
  gtest_discover_tests(dpl2_commit_test
    TEST_PREFIX "commit." DISCOVERY_TIMEOUT 30
    PROPERTIES ENVIRONMENT "FR_VERBOSE=1")

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
    DISCOVERY_TIMEOUT 30
    PROPERTIES ENVIRONMENT "FR_VERBOSE=1")
endif()
