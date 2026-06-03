# Comprehensive `check_drc -check_pg` toy. Three rows of clusters, one column
# per violation type (left->right in kAbbrev order):
#   row 1 (top)    : the violation is PG-vs-PG      (VDD vs VSS)     -> reported
#   row 2 (middle) : the violation is PG-vs-signal  (VDD vs SIG1)    -> reported
#   row 3 (bottom) : the violation is signal-vs-sig (SIG1 vs SIG2)   -> SUPPRESSED
#
# All geometry is on SPECIAL nets so widths are controllable: VDD/VSS are PG
# (loaded non-fixed under -check_pg); SIG1/SIG2 use `USE SIGNAL` so drt sees them
# as non-PG and loads them fixed (PG classification is by sigType.isSupply()).
# Pairwise checks skip both-fixed pairs and single-shape checks skip fully-fixed
# shapes, so the entire signal-vs-signal row produces no markers -- proving the
# -check_pg scoping. See docs/agents/drc_check_pg.md.
#
# (The DEF/guide are generated; this covers the reliably-triggerable rule
# families. LEF58 sub-variants need a foundry-style rule deck.)
source "helpers.tcl"
read_lef drc_test_pg.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def drc_test_pg_full.def
read_guides drc_test_pg_full.route_guide
set_thread_count 1
set drc_file [make_result_file drc_test_pg_full.drc]
check_drc -output_file $drc_file -check_pg
diff_files $drc_file drc_test_pg_full.drcok
