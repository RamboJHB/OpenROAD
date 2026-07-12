#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

build_dir="build"
generated_source="$build_dir/ImplantLayerCheckerCore.cpp"
generated_include="$build_dir/include"
mkdir -p "$build_dir"
mkdir -p "$generated_include/drc"

# The imported checker header and implementation currently have a few naming
# drifts. Normalize those in the generated test header only; production files
# remain untouched. The implementation is the source of truth for this test.
awk '
  /struct ScanOutcome/ { in_scan_outcome = 1 }
  in_scan_outcome && /std::vector<InstanceId> instances;/ {
    sub(/instances;/, "instanceIds;")
  }
  in_scan_outcome && /^  };/ { in_scan_outcome = 0 }
  /std::vector<MergedShape> findNeighbors\(/ { in_find_neighbors = 1 }
  /std::vector<RuleOutcome> evaluate\(/ {
    sub(/evaluate\(/, "evalRule(")
  }
  in_find_neighbors && /const std::vector<MergedShape>& targetShapes,/ { next }
  in_find_neighbors && /excludedInstances\) const;/ { in_find_neighbors = 0 }
  { print }
' ../ImplantLayerChecker.h > "$generated_include/drc/ImplantLayerChecker.h"

# Compile the production checker core without its unavailable UDM extraction
# boundary. The constructor replacement prevents automatic DB discovery; tests
# initialize the real checker explicitly through ImplantInput. Everything from
# initialize() through the overlay/rule/violation implementation is copied
# verbatim from the production source for this build.
awk '
  /ImplantLayerChecker::ImplantLayerChecker\(Grid\* grid\)/ {
    print "ImplantLayerChecker::ImplantLayerChecker(Grid* grid) : DRCChecker(grid) {}"
    skipping_constructor = 1
    next
  }
  skipping_constructor && /ImplantLayerChecker::~ImplantLayerChecker\(\)/ {
    skipping_constructor = 0
  }
  skipping_constructor { next }
  /bool ImplantLayerChecker::dump\(/ {
    skipping_serialization = 1
    next
  }
  skipping_serialization && /bool ImplantLayerChecker::buildRules\(/ {
    skipping_serialization = 0
  }
  skipping_serialization { next }
  /void ImplantLayerChecker::parseLayerName/ {
    skipping_udm = 1
    next
  }
  skipping_udm && /std::string ImplantLayerChecker::inputToString/ {
    skipping_udm = 0
  }
  skipping_udm { next }
  { print }
' ../ImplantLayerChecker.cpp > "$generated_source"

cxx="${CXX:-c++}"
"$cxx" -std=c++17 -Wall -Wextra -Werror -g \
  -Wno-unused-function \
  -Wno-switch \
  -I "$generated_include" \
  -I support/include \
  -I ../.. \
  "$generated_source" \
  ImplantLayerCheckerOverlayTest.cpp \
  support/mini_gtest_main.cpp \
  -o "$build_dir/ImplantLayerCheckerOverlayTest"

"$build_dir/ImplantLayerCheckerOverlayTest"
