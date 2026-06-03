# LEF58-focused `check_drc -check_pg` toy: every violation is on a PG net (VDD),
# and a SIG1 (USE SIGNAL) copy of each cluster is loaded fixed to prove
# suppression. Tech drc_test_pg_lef58.lef = Nangate45 + LEF58 rules injected on
# dedicated layers so each fires in isolation:
#   metal3 LEF58_AREA              -> Lef58Area
#   metal4 LEF58_EOLKEEPOUT        -> Lef58EolKeepOut
#   metal5 LEF58_EOLEXTENSIONSPACING -> Lef58EolExtension
#   metal6 LEF58_SPACING (EOL)     -> Lef58SpacingEndOfLine
#
# These are the LEF58 violation types the GC engine can actually emit as markers
# under check_drc. Most other Lef58* getViolName values are enum/sub-property
# names that are never a marker's constraint (e.g. corner -> "Corner Spacing",
# min step -> "Min Step"), so they are unreachable here -- the engine's LEF58
# algorithms themselves are unit-tested in gcTest.cpp. See docs/agents.
source "helpers.tcl"
read_lef drc_test_pg_lef58.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def drc_test_pg_lef58.def
read_guides drc_test_pg_lef58.route_guide
set_thread_count 1
set drc_file [make_result_file drc_test_pg_lef58.drc]
check_drc -output_file $drc_file -check_pg
diff_files $drc_file drc_test_pg_lef58.drcok
