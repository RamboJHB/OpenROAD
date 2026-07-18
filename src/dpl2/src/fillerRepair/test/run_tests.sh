#!/usr/bin/env bash
# Builds and runs the 87 GoogleTest planner cases.
# Usage: test/run_tests.sh
#        FR_VERBOSE=1 test/run_tests.sh
#        test/run_tests.sh --gtest_filter='FillerRepairPlanner.*adaptive*'
#        SANITIZE=address test/run_tests.sh
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
dpl2_root="$(cd "$script_dir/../../.." && pwd)"

asan=OFF
build_name=planner-gtest
if [[ "${SANITIZE:-}" == "address" ]]; then
  asan=ON
  build_name=planner-gtest-asan
  if [[ "$(uname -s)" == "Darwin" ]]; then
    # Homebrew GoogleTest is an unsanitized static archive. libc++ container
    # annotations otherwise report inside GoogleTest discovery, not our code.
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_container_overflow=0"
  fi
fi
build_dir="$dpl2_root/test/build/$build_name"

cmake -S "$dpl2_root/test" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DDPL2_ENABLE_ASAN="$asan" \
  -DDPL2_TEST_USE_FAKE_UDM=ON
cmake --build "$build_dir" --target dpl2_filler_repair_planner_test --parallel
"$build_dir/dpl2_filler_repair_planner_test" "$@"
