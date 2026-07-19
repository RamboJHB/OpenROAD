# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, The OpenROAD Authors
#
# Single source of truth for the fillerRepair source lists. The test harness
# and the destination production build must both include this file instead of
# spelling out the files, so the two can never drift.
#
#   DPL2_FILLER_REPAIR_PLANNER_SOURCES     pure planner, no UDM dependency
#   DPL2_FILLER_REPAIR_PRODUCTION_SOURCES  the complete compile list a
#                                          destination build adds: planner +
#                                          UDM-facing facade with its private
#                                          checker/view
#   DPL2_FILLER_REPAIR_PORTABLE_E2E_SOURCE planner + final-checker E2E that
#                                           uses ImplantLayerCheckerHelper
#   DPL2_FILLER_REPAIR_PRECHECK_SOURCE      UDM-free coverage sweep shared by
#                                           production and portable tests

set(DPL2_FILLER_REPAIR_PLANNER_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/PlacementView.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Signature.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Swap.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Window.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Ranker.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/SubsetSearch.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/OracleGate.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/FillerRepairPlanner.cpp")

set(DPL2_FILLER_REPAIR_PRECHECK_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/PlacementPrecheck.cpp")

set(DPL2_FILLER_REPAIR_PRODUCTION_SOURCES
    ${DPL2_FILLER_REPAIR_PLANNER_SOURCES}
    ${DPL2_FILLER_REPAIR_PRECHECK_SOURCE}
    "${CMAKE_CURRENT_LIST_DIR}/FillerRepairEngine.cpp")

set(DPL2_FILLER_REPAIR_PORTABLE_E2E_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/test/FillerRepairCheckerE2ETest.cpp")
