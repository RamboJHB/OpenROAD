#!/usr/bin/env bash
# Builds and runs the 82 GoogleTest planner cases.
# Convenience runner for the portable cases owned by fillerRepair/test.
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
dpl2_root="$(cd "$script_dir/../.." && pwd)"

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
  -DDPL2_TEST_FAKE_UDM_INCLUDE_DIR="$script_dir/fake_udm/include" \
  -DDPL2_TEST_USE_FAKE_UDM=ON
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
cmake --build "$build_dir" --target dpl2_filler_repair_planner_test \
  --parallel "$jobs"
"$build_dir/dpl2_filler_repair_planner_test" "$@"
