# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, The OpenROAD Authors
#
# The fillerRepair migration payload, as CMake targets.
#
# A destination adds this directory and links a target -- it never spells out
# source files, so its build and ours cannot drift:
#
#   add_subdirectory(<...>/src/fillerRepair fillerRepair)
#   target_link_libraries(my_placer PRIVATE dpl2::fillerRepair)
#
# Everything external (UDM, dpl2 infrastructure, the implant checker, and any
# global flags such as sanitizers) arrives through ONE interface target,
# dpl2_filler_repair_deps. Define it before adding this directory to point the
# payload at your headers and libraries; otherwise the fallback below assumes
# the in-tree layout.
#
# Targets:
#   dpl2::fillerRepair         complete payload: search pipeline + runtime
#                              engine/checker boundary (C++20)
#   dpl2::fillerRepairPlanner  the pure search pipeline alone, no database
#                              access (C++17) -- the portability gate

cmake_minimum_required(VERSION 3.20)

# Only when this directory is the build's top level -- a parent that adds it
# has its own project() and must keep it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(dpl2_filler_repair LANGUAGES CXX)
  if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
  endif()
endif()

# Plain variables, not option(): a parent that adds this directory sets them
# directly, with no policy subtleties about option() vs normal variables.
if(NOT DEFINED DPL2_FILLER_REPAIR_BUILD_TESTS)
  set(DPL2_FILLER_REPAIR_BUILD_TESTS OFF)
endif()
if(NOT DEFINED DPL2_FILLER_REPAIR_WERROR)
  set(DPL2_FILLER_REPAIR_WERROR ON)
endif()

set(DPL2_UDM_INCLUDE_DIRS "" CACHE STRING
    "UDM include directories (fallback deps target only)")
set(DPL2_UDM_LIBRARIES "" CACHE STRING
    "UDM libraries or CMake targets (fallback deps target only)")

if(NOT TARGET dpl2_filler_repair_deps)
  # Fallback for standalone use: <fillerRepair/...>, <drc/...> and
  # <infrastructure/...> resolve from the src/ root, <Grid.h> and friends from
  # infrastructure/, and <dpl2/DePlace.h> from include/. UDM comes from the
  # cache variables above. A destination that already has targets for these
  # should define dpl2_filler_repair_deps itself and skip all of this.
  find_package(Boost REQUIRED)
  find_package(TBB REQUIRED)
  add_library(dpl2_filler_repair_deps INTERFACE)
  target_include_directories(
    dpl2_filler_repair_deps
    INTERFACE "${CMAKE_CURRENT_LIST_DIR}/.."
              "${CMAKE_CURRENT_LIST_DIR}/../infrastructure"
              "${CMAKE_CURRENT_LIST_DIR}/../../include")
  target_include_directories(
    dpl2_filler_repair_deps
    SYSTEM INTERFACE ${DPL2_UDM_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})
  target_link_libraries(
    dpl2_filler_repair_deps INTERFACE TBB::tbb ${DPL2_UDM_LIBRARIES})
endif()

# Warnings are the module's own business, not the consumer's: these sources
# are ours and must stay clean wherever they are built.
function(dpl2_filler_repair_configure target standard)
  target_compile_features(${target} PUBLIC "cxx_std_${standard}")
  target_compile_options(${target} PRIVATE -Wall -Wextra)
  if(DPL2_FILLER_REPAIR_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
  target_link_libraries(${target} PUBLIC dpl2_filler_repair_deps)
endfunction()

# The pure pipeline. Built at C++17 on purpose: it must not acquire a
# dependency on the runtime boundary or on a newer standard than a
# destination's own placer may use.
add_library(dpl2_filler_repair_planner OBJECT RepairPlanner.cpp)
dpl2_filler_repair_configure(dpl2_filler_repair_planner 17)
add_library(dpl2::fillerRepairPlanner ALIAS dpl2_filler_repair_planner)

# The complete payload. RepairPlanner.cpp is compiled again here rather than
# reused from the target above: that one is pinned to C++17 for the
# portability gate, and mixing standards across one link is not something to
# rely on.
add_library(dpl2_filler_repair OBJECT RepairPlanner.cpp FillerRepairEngine.cpp)
dpl2_filler_repair_configure(dpl2_filler_repair 20)
add_library(dpl2::fillerRepair ALIAS dpl2_filler_repair)

if(DPL2_FILLER_REPAIR_BUILD_TESTS)
  # enable_testing() has to run in the top-level directory of whichever build
  # owns it, so it lives here rather than in test/: standalone, this file IS
  # the top level; added by a parent, the parent has already called it and
  # this is a no-op repeat.
  include(CTest)
  enable_testing()
  add_subdirectory(test)
endif()
