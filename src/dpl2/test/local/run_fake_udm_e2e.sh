#!/usr/bin/env bash
# Local-only runner for the historical engine regression. Both the
# case source and its fake-UDM provider live outside the migration payload.
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
dpl2_root="$(cd "$script_dir/../.." && pwd)"

asan=OFF
build_name=e2e-gtest
if [[ "${SANITIZE:-}" == "address" ]]; then
  asan=ON
  build_name=e2e-gtest-asan
  if [[ "$(uname -s)" == "Darwin" ]]; then
    # Homebrew GoogleTest is an unsanitized static archive. libc++ container
    # annotations otherwise report inside discovery rather than project code.
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_container_overflow=0"
  fi
fi
build_dir="$dpl2_root/test/build/$build_name"

# Every test is its own ctest entry (gtest_discover_tests), and ctest is serial
# by default, so an unparallelised run pays one process launch per case.
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# ALL=1 builds and runs the whole suite (engine + planner + portable checker)
# instead of only the engine regression.
if [[ "${ALL:-}" == "1" ]]; then
  build_target=(--target dpl2_filler_repair_e2e
                --target dpl2_filler_repair_planner_test
                --target dpl2_filler_repair_checker_e2e
                --target dpl2_filler_repair2_compile_check)
  # Bash 3.2 + `set -u` rejects expansion of an empty array.
  test_filter=(-R '.*')
else
  build_target=(--target dpl2_filler_repair_e2e)
  test_filter=(-R '^e2e\.')
fi

cmake -S "$dpl2_root/test" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DDPL2_ENABLE_ASAN="$asan" \
  -DDPL2_TEST_FAKE_UDM_INCLUDE_DIR="$script_dir/fake_udm/include" \
  -DDPL2_TEST_USE_FAKE_UDM=ON
cmake --build "$build_dir" "${build_target[@]}" --parallel "$jobs"
ctest --test-dir "$build_dir" --output-on-failure -j "$jobs" "${test_filter[@]}"
