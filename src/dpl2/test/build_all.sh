#!/usr/bin/env bash
# Tier-1 local build: real infrastructure + final checker + repair chain
# compiled against the fake UDM headers (src/drc/test/support/include).
# DePlace.cpp is excluded: it needs PaddingChecker.h / EdgeSpacingChecker.h /
# PlacementDRC.h implementations that are not delivered yet.
set -euo pipefail
cd "$(dirname "$0")"
build_dir="build"
mkdir -p "$build_dir"
cxx="${CXX:-c++}"
sanitize_flags=(-O0)
if [[ "${SANITIZE:-}" == "address" ]]; then
  sanitize_flags+=(-fsanitize=address -fno-omit-frame-pointer)
fi

# Grid.cpp and DePlace.cpp are excluded: they need tbb / PhysNet visitors /
# PaddingChecker headers that are not part of the repair chain. The Grid
# member functions the chain links against are provided functionally by
# support/grid_link_stubs.cpp over the Helper-populated row maps.
srcs=(
  support/grid_link_stubs.cpp
  ../src/infrastructure/Object.cpp
  ../src/infrastructure/network.cpp
  ../src/infrastructure/architecture.cpp
  ../src/infrastructure/Padding.cpp
  ../src/infrastructure/fillerSetting.cpp
  ../src/drc/ImplantLayerChecker.cpp
  ../src/drc/ImplantLayerCheckerHelper.cpp
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

"$cxx" -std=c++20 -Wall -g "${sanitize_flags[@]}" \
  -I shim \
  -I ../src \
  -I ../src/drc/test/support/include \
  "${srcs[@]}" \
  -o "$build_dir/smoke"

"$build_dir/smoke"
