# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, The OpenROAD Authors

add_library(dpl2_filler_repair OBJECT
  RepairPlanner.cpp
  FillerRepairEngine.cpp)
add_library(dpl2::fillerRepair ALIAS dpl2_filler_repair)

target_compile_features(dpl2_filler_repair PUBLIC cxx_std_20)
target_include_directories(
  dpl2_filler_repair PUBLIC "${CMAKE_CURRENT_LIST_DIR}/..")

# Optional parent-owned interface target for UDM/infra/checker dependencies.
if(TARGET dpl2_filler_repair_deps)
  target_link_libraries(dpl2_filler_repair PUBLIC dpl2_filler_repair_deps)
endif()
