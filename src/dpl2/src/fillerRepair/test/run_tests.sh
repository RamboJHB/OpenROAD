#!/usr/bin/env bash
# Builds and runs the fillerRepair unit tests. Standalone (STL only) until
# dpl2 joins the CMake build (spec section 11 TODO 12).
# Usage: test/run_tests.sh          quiet
#        FR_VERBOSE=1 test/run_tests.sh   with the [fr] debug transcript
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR=test/build
mkdir -p "$BUILD_DIR"

g++ -std=c++17 -Wall -Wextra -Werror -g \
  Move.cpp PreCheck.cpp Signature.cpp Window.cpp SwapGenerator.cpp \
  FillerRepairEngine.cpp \
  fake/FakeImplantChecker.cpp \
  test/test_main.cpp \
  -o "$BUILD_DIR/fillerRepair_tests"

"$BUILD_DIR/fillerRepair_tests"
