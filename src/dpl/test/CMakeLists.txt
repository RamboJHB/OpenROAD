# Copyright 2023 Google LLC
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

include("openroad")

add_executable(dpl_test
  dpl_test.cc
  FillerPlacementOpenDbTest.cpp
  FillerPlacementInternalTest.cpp
)

target_include_directories(dpl_test PRIVATE ../src)

target_link_libraries(dpl_test 
    gtest 
    gtest_main
    dpl_lib
)

if(BUILD_DPL2_LOCAL_TEST)
  target_compile_definitions(dpl_test PRIVATE
    DPL2_FILLER_SETTING_AVAILABLE)
  target_link_libraries(dpl_test dpl2_local_runtime)
endif()

gtest_discover_tests(dpl_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(build_and_test dpl_test)

