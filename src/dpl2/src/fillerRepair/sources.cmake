# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, The OpenROAD Authors
#
# Single source of truth for the fillerRepair source lists. The test harness
# and the destination runtime build must both include this file instead of
# spelling out the files, so the two can never drift.
#
#   DPL2_FILLER_REPAIR_PLANNER_SOURCES     pure search pipeline; no database
#                                          access (headers: RepairTypes,
#                                          Debug, PlacementView, RepairOracle)
#   DPL2_FILLER_REPAIR_SOURCES             complete destination compile list:
#                                          planner + runtime engine/checker boundary
#   DPL2_FILLER_REPAIR_PORTABLE_E2E_SOURCE planner + final-checker E2E that
#                                           uses ImplantLayerCheckerHelper
#   DPL2_FILLER_REPAIR_PORTABLE_PLANNER_TEST_SOURCES
#                                          database-free planner unit tests
#                                          and synthetic test doubles

set(DPL2_FILLER_REPAIR_PLANNER_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/RepairPlanner.cpp")

set(DPL2_FILLER_REPAIR_SOURCES
    ${DPL2_FILLER_REPAIR_PLANNER_SOURCES}
    "${CMAKE_CURRENT_LIST_DIR}/FillerRepairEngine.cpp")

set(DPL2_FILLER_REPAIR_PORTABLE_E2E_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/test/FillerRepairCheckerE2ETest.cpp")

set(DPL2_FILLER_REPAIR_PORTABLE_PLANNER_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/test/TestRepairOracle.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/test/SyntheticMasterCatalog.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/test/RepairPlannerTest.cpp")
