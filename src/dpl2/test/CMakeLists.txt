cmake_minimum_required(VERSION 3.20)

project(dpl2_filler_repair_e2e LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

option(DPL2_ENABLE_ASAN "Build the fake-UDM-only E2E with AddressSanitizer" OFF)

find_package(Boost REQUIRED)
find_package(TBB REQUIRED)

set(DPL2_SRC "${CMAKE_CURRENT_LIST_DIR}/../src")

add_executable(
  dpl2_filler_repair_e2e
  "${DPL2_SRC}/infrastructure/Grid.cpp"
  "${DPL2_SRC}/infrastructure/Object.cpp"
  "${DPL2_SRC}/infrastructure/network.cpp"
  "${DPL2_SRC}/infrastructure/architecture.cpp"
  "${DPL2_SRC}/infrastructure/Padding.cpp"
  "${DPL2_SRC}/infrastructure/fillerSetting.cpp"
  "${DPL2_SRC}/infrastructure/RepairInfrastructure.cpp"
  "${DPL2_SRC}/drc/ImplantLayerChecker.cpp"
  "${DPL2_SRC}/fillerRepair/PlacementView.cpp"
  "${DPL2_SRC}/fillerRepair/Signature.cpp"
  "${DPL2_SRC}/fillerRepair/Swap.cpp"
  "${DPL2_SRC}/fillerRepair/Window.cpp"
  "${DPL2_SRC}/fillerRepair/Ranker.cpp"
  "${DPL2_SRC}/fillerRepair/SubsetSearch.cpp"
  "${DPL2_SRC}/fillerRepair/OracleGate.cpp"
  "${DPL2_SRC}/fillerRepair/FillerRepairEngine.cpp"
  "${DPL2_SRC}/fillerRepair/adapter/PlacementView.cpp"
  smoke_main.cpp)

target_compile_features(dpl2_filler_repair_e2e PRIVATE cxx_std_20)
target_compile_options(
  dpl2_filler_repair_e2e
  PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  dpl2_filler_repair_e2e
  PRIVATE
    "${DPL2_SRC}"
    "${CMAKE_CURRENT_LIST_DIR}/../include"
    "${DPL2_SRC}/drc/test/support/include")
target_include_directories(
  dpl2_filler_repair_e2e SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
if(DPL2_ENABLE_ASAN AND APPLE AND EXISTS "/opt/homebrew/opt/tbb/lib/libtbb.a")
  target_link_libraries(
    dpl2_filler_repair_e2e PRIVATE "/opt/homebrew/opt/tbb/lib/libtbb.a")
else()
  target_link_libraries(dpl2_filler_repair_e2e PRIVATE TBB::tbb)
endif()

if(DPL2_ENABLE_ASAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      dpl2_filler_repair_e2e PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(
      dpl2_filler_repair_e2e PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR "DPL2_ENABLE_ASAN requires a Clang or GNU compiler")
  endif()
endif()

enable_testing()
add_test(NAME dpl2_filler_repair_e2e COMMAND dpl2_filler_repair_e2e)
