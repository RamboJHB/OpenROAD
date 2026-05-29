# Toy case for `check_drc -check_pg`.
#
# The design contains four deliberate situations:
#   * a signal-to-signal short      (net1 vs net2)        on metal1
#   * a PG-to-signal short          (net1 vs VDD)          on metal2
#   * a PG-to-PG short              (VDD  vs VSS)          on metal2
#   * a PG-to-obstruction short     (VDD  vs metal2 OBS)   on metal2
#
# In PG-only mode, PG geometry is loaded NON-fixed (under test) and everything
# else (signal/clock pins+routing, blockages) is loaded FIXED (background); the
# GC engine skips pairs where both shapes are fixed. So `check_drc -check_pg`
# reports exactly the three violations that involve PG (net1/VDD, VDD/VSS,
# VDD/obstruction) and suppresses the pure signal-to-signal short (net1/net2).
# A plain check_drc instead reports the signal-to-signal short (and net1/VDD,
# which also involves a non-fixed signal shape) but not the both-fixed PG-PG
# or PG-obstruction pairs. See docs/agents/drc_check_pg.md.
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
