# filler_repair_mock — sandbox logic-test scaffolding (NOT part of the build)

A minimal in-memory MOCK of the small odb / utl surface that
`src/dpl/src/DplFillerGrid.cpp` uses, so the dpl adapter can be **compiled and
logic-tested without a full OpenROAD build** (no swig/bazel needed).

It validates that the adapter is well-formed C++ and that its logic is correct
(grid build, dirty detection, delete + refill incl. multi-height, master
mapping, unsolvable reporting).  It does NOT replace the real odb integration
test: the mock encodes the *assumed* odb signatures, so final signature
correctness still requires building inside OpenROAD.

Build & run:
    g++ -std=c++17 -I src/dpl/src -I src/dpl/test/filler_repair_mock \
        src/dpl/src/DplFillerGrid.cpp src/dpl/src/FillerRepair.cpp \
        src/dpl/test/dpl_filler_grid_test.cpp -o /tmp/dpl_test && /tmp/dpl_test
