#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

build_dir="build"
mkdir -p "$build_dir"

cxx="${CXX:-c++}"
sanitize_flags=(-O0)
if [[ "${SANITIZE:-}" == "address" ]]; then
  sanitize_flags+=(-fsanitize=address -fno-omit-frame-pointer)
fi

"$cxx" -std=c++17 -Wall -Wextra -Werror -g \
  "${sanitize_flags[@]}" \
  -DDPL2_FAKE_UDM \
  -Wno-unused-function \
  -Wno-switch \
  -I support/include \
  -I ../.. \
  ../ImplantLayerChecker.cpp \
  ImplantLayerCheckerOverlayTest.cpp \
  support/mini_gtest_main.cpp \
  -o "$build_dir/ImplantLayerCheckerOverlayTest"

"$build_dir/ImplantLayerCheckerOverlayTest"
