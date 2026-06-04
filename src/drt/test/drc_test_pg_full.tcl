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
# Tech is drc_test_pg_full.lef = Nangate45 + injected rules on dedicated layers
# (metal2 AREA/MINSTEP/MINIMUMCUT; metal3 PROPERTY LEF58_AREA; metal4
# MINENCLOSEDAREA) so each rule fires in isolation. Covered types: Short,
# Min Width, Metal Spacing, Cut Spacing, Min Hole, Off Grid, Min Area,
# Minimum Cut, Lef58Area, Lef58SpacingEndOfLine, Lef58EolExtension,
# Lef58EolKeepOut -- i.e. the LEF58 types the GC engine can actually emit as
# markers. (Most other Lef58* getViolName values are never a marker's
# constraint, so check_drc can never output them; see the UNREACH tags in
# kAbbrev, TritonRoute.cpp. The LEF58 algorithms are unit-tested in gcTest.cpp.)
# Pairwise types (Short, Metal Spacing) appear in all three rows; single-shape
# and LEF58 types only in rows 1 and 3. The DEF/guide are generated.
source "helpers.tcl"
read_lef drc_test_pg_full.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def drc_test_pg_full.def
read_guides drc_test_pg_full.route_guide
set_thread_count 1
set drc_file [make_result_file drc_test_pg_full.drc]
check_drc -output_file $drc_file -check_pg
diff_files $drc_file drc_test_pg_full.drcok
