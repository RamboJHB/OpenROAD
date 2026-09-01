if(UV_BUILD_TESTS)
  add_library(
    uv3d_dpl2_filler_repair_test OBJECT
    FillerRepairIntegrationTest.cpp
    FillerRepairInternalTest.cpp
    FillerRetilerTest.cpp)
  target_include_directories(
    uv3d_dpl2_filler_repair_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../.." "${TBB_ROOT}/include")
  target_link_libraries(
    uv3d_dpl2_filler_repair_test PRIVATE dpl2Lib)
  set(UNIT_TESTS
      "${UNIT_TESTS} uv3d_dpl2_filler_repair_test"
      CACHE STRING "Collect unit tests from sub-directories" FORCE)
endif()
