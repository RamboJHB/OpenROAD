# Local-only OpenROAD validation wire. None of this target is part of the
# fillerRepair/fillerRepair2 copy payload.

find_package(Boost REQUIRED)
find_package(TBB REQUIRED)
find_package(ZLIB REQUIRED)

set(DPL2_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(DPL2_SRC "${DPL2_ROOT}/src")
set(DPL2_FAKE_UDM "${DPL2_ROOT}/test/local/fake_udm/include")

add_library(dpl2_local_runtime STATIC
  "${DPL2_SRC}/DePlace.cpp"
  "${DPL2_SRC}/PlacementDRC.cpp"
  "${DPL2_SRC}/infrastructure/Grid.cpp"
  "${DPL2_SRC}/infrastructure/Object.cpp"
  "${DPL2_SRC}/infrastructure/network.cpp"
  "${DPL2_SRC}/infrastructure/Padding.cpp"
  "${DPL2_SRC}/infrastructure/fillerSetting.cpp"
  "${DPL2_SRC}/infrastructure/architecture.cpp"
  "${DPL2_SRC}/drc/util.cpp"
  "${DPL2_SRC}/drc/ImplantLayerChecker.cpp"
  "${DPL2_SRC}/drc/ImplantLayerCheckerHelper.cpp"
  ImportDb.cpp)

target_compile_features(dpl2_local_runtime PUBLIC cxx_std_20)
target_include_directories(dpl2_local_runtime
  PUBLIC
    "${DPL2_ROOT}/include"
    "${DPL2_SRC}"
    "${DPL2_SRC}/infrastructure"
  SYSTEM PUBLIC
    "${DPL2_FAKE_UDM}"
    ${Boost_INCLUDE_DIRS})
target_link_libraries(dpl2_local_runtime PUBLIC odb TBB::tbb ZLIB::ZLIB)

add_library(dpl2_filler_repair_deps INTERFACE)
target_link_libraries(dpl2_filler_repair_deps INTERFACE dpl2_local_runtime)
set(DPL2_FILLER_REPAIR_BUILD_TESTS OFF)
add_subdirectory("${DPL2_SRC}/fillerRepair" fillerRepair)

add_library(dpl2_openroad_test STATIC
  OpenRoadTestWire.cpp
  "${DPL2_ROOT}/dpl2ui/FillerRepairDumpReplay.cc"
  "${DPL2_ROOT}/dpl2ui/testFillerRepairCmd.cc")
target_compile_features(dpl2_openroad_test PUBLIC cxx_std_20)
target_include_directories(dpl2_openroad_test
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/command_shim/include"
    "${DPL2_ROOT}/dpl2ui")
target_link_libraries(dpl2_openroad_test
  PUBLIC dpl2_local_runtime dpl2::fillerRepair odb
  PRIVATE ${TCL_LIBRARY})

target_compile_options(dpl2_local_runtime PRIVATE -Wall -Wextra)
target_compile_options(dpl2_openroad_test PRIVATE -Wall -Wextra)
