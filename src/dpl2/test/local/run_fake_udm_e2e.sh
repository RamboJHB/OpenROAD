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

cmake -S "$dpl2_root/test" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DDPL2_ENABLE_ASAN="$asan" \
  -DDPL2_TEST_FAKE_UDM_INCLUDE_DIR="$script_dir/fake_udm/include" \
  -DDPL2_TEST_USE_FAKE_UDM=ON
cmake --build "$build_dir" --target dpl2_filler_repair_e2e --parallel
ctest --test-dir "$build_dir" --output-on-failure -R '^e2e\.'
