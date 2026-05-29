# Toy case for `check_drc -check_pg`.
#
# The design contains two deliberate shorts:
#   * a signal-to-signal short  (net1 vs net2)   on metal1
#   * a PG-to-PG short          (VDD  vs VSS)     on metal2
#
# Running check_drc with -check_pg must report ONLY the PG short; the signal
# short is filtered out because signal objects are never loaded into the GC
# engine in PG-only mode. (A plain check_drc on this design reports the signal
# short instead, because the fixed PDN stripes are not re-checked against each
# other -- see docs/agents/drc_check_pg.md.)
source "helpers.tcl"
read_lef Nangate45/Nangate45_tech.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def drc_test_pg.def
read_guides drc_test_pg.route_guide

# Single thread so the per-worker debug logic chain is deterministic.
set_thread_count 1
# Print the PG-only logic chain (entry -> filter -> init -> done) to the log.
set_debug_level DRT checkPG 1

set drc_file [make_result_file drc_test_pg.drc]
drt::check_drc -output_file $drc_file -check_pg
diff_files $drc_file drc_test_pg.drcok
