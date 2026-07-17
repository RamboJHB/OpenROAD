#!/usr/bin/env bash
# Fake-UDM-only E2E: production infrastructure + final checker + repair chain.
# The fake UDM supplies test data only.  Grid, Network, infrastructure import,
# checker, adapter and planner are the real implementations.
set -euo pipefail
cd "$(dirname "$0")"
build_dir="build"
mkdir -p "$build_dir"
cxx="${CXX:-c++}"
sanitize_flags=(-O0)
if [[ "${SANITIZE:-}" == "address" ]]; then
  sanitize_flags+=(-fsanitize=address -fno-omit-frame-pointer)
fi

srcs=(
  ../src/infrastructure/Grid.cpp
  ../src/infrastructure/Object.cpp
  ../src/infrastructure/network.cpp
  ../src/infrastructure/architecture.cpp
  ../src/infrastructure/Padding.cpp
  ../src/infrastructure/fillerSetting.cpp
  ../src/infrastructure/RepairInfrastructure.cpp
  ../src/drc/ImplantLayerChecker.cpp
  ../src/fillerRepair/PlacementView.cpp
  ../src/fillerRepair/Signature.cpp
  ../src/fillerRepair/Swap.cpp
  ../src/fillerRepair/Window.cpp
  ../src/fillerRepair/Ranker.cpp
  ../src/fillerRepair/SubsetSearch.cpp
  ../src/fillerRepair/OracleGate.cpp
  ../src/fillerRepair/FillerRepairEngine.cpp
  ../src/fillerRepair/adapter/PlacementView.cpp
  smoke_main.cpp
)

extra_cxxflags=()
extra_ldflags=(-ltbb)
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists tbb; then
  # shellcheck disable=SC2207
  extra_cxxflags+=($(pkg-config --cflags tbb))
  # shellcheck disable=SC2207
  extra_ldflags=($(pkg-config --libs tbb))
elif [[ -d /opt/homebrew/include ]]; then
  extra_cxxflags+=(-isystem /opt/homebrew/include)
  extra_ldflags=(-L/opt/homebrew/lib -ltbb)
fi
# Homebrew's dynamically linked oneTBB 2023.0 tears down after Apple's ASan
# runtime and crashes in __TBB_InitOnce::~__TBB_InitOnce.  The static archive
# has the same production implementation without that dylib finalizer order.
if [[ "${SANITIZE:-}" == "address" && "$(uname -s)" == "Darwin"
      && -f /opt/homebrew/opt/tbb/lib/libtbb.a ]]; then
  extra_ldflags=(/opt/homebrew/opt/tbb/lib/libtbb.a)
fi

"$cxx" -std=c++20 -Wall -Wextra -Werror -g "${sanitize_flags[@]}" \
  -I ../src \
  -I ../include \
  -I ../src/drc/test/support/include \
  "${extra_cxxflags[@]}" \
  "${srcs[@]}" \
  "${extra_ldflags[@]}" \
  -o "$build_dir/smoke"

"$build_dir/smoke"
