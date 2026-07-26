#!/usr/bin/env bash
# Destination simulation: builds and runs the migration payload the way a
# real-UDM destination does (DPL2_TEST_USE_FAKE_UDM=OFF), so a breakage that
# only shows up outside the fake-UDM configuration is caught HERE instead of
# at migration time.
#
# We have no real UDM in this environment, so the fake headers are supplied
# through the real-UDM knob. That is deliberate and still meaningful: this
# mode's value is that it exercises the destination code path -- no fake test
# provider, no fake-only target -- not that the headers are real. What it
# proves is that every supplied/runtime source compiles and that the executable
# LINK CLOSURE is complete; a missing object shows up as an undefined symbol.
# (A static compile-check library cannot prove this: archives do not resolve
# symbols. Only linking an executable does.)
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
dpl2_root="$(cd "$script_dir/../.." && pwd)"

asan=OFF
build_name=migration-gate
if [[ "${SANITIZE:-}" == "address" ]]; then
  asan=ON
  build_name=migration-gate-asan
fi
build_dir="$dpl2_root/test/build/$build_name"

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# DPL2_TEST_UDM_INCLUDE_DIRS may be pointed at a genuine UDM install; it
# defaults to the fake headers so the gate is runnable with no UDM present.
udm_includes="${DPL2_UDM_INCLUDE_DIRS:-$script_dir/fake_udm/include}"

cmake -S "$dpl2_root/test" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DDPL2_ENABLE_ASAN="$asan" \
  -DDPL2_TEST_USE_FAKE_UDM=OFF \
  -DDPL2_TEST_UDM_INCLUDE_DIRS="$udm_includes" \
  -DDPL2_TEST_UDM_LIBRARIES="${DPL2_UDM_LIBRARIES:-}"
cmake --build "$build_dir" --parallel "$jobs"
ctest --test-dir "$build_dir" --output-on-failure -j "$jobs"
