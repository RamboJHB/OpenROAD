#!/usr/bin/env bash
# GoogleTest E2E over the supplied infrastructure/checker and production
# fillerRepair. Fake UDM is selected only through the test CMake interface.
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"

asan=OFF
build_name=e2e-gtest
if [[ "${SANITIZE:-}" == "address" ]]; then
  asan=ON
  build_name=e2e-gtest-asan
  if [[ "$(uname -s)" == "Darwin" ]]; then
    # Homebrew GoogleTest is an unsanitized static archive. libc++ container
    # annotations otherwise report inside GoogleTest discovery, not our code.
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_container_overflow=0"
  fi
fi
build_dir="$script_dir/build/$build_name"

cmake -S "$script_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DDPL2_ENABLE_ASAN="$asan" \
  -DDPL2_TEST_USE_FAKE_UDM=ON
cmake --build "$build_dir" --target dpl2_filler_repair_e2e --parallel
ctest --test-dir "$build_dir" --output-on-failure -R '^e2e\.'
