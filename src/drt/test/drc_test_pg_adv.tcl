# Companion toy for `check_drc -check_pg`. Uses the Nangate45 layer stack with
# extra rules injected into metal2 (AREA / MINIMUMCUT) that stock Nangate45
# lacks, so PG (special net VDD) geometry triggers:
#   * Min Area     (a small VDD metal2 shape below the area rule)
#   * Minimum Cut  (a wide VDD metal2 strap connected by a single via1 cut)
# check_drc -check_pg must report both. (Min Area is special-cased to emit a
# marker in PG mode; see docs/agents/drc_check_pg.md sec 2.2.)
source "helpers.tcl"
read_lef drc_test_pg_adv_tech.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def drc_test_pg_adv.def
read_guides drc_test_pg_adv.route_guide
set_thread_count 1
set_debug_level DRT checkPG 1
set drc_file [make_result_file drc_test_pg_adv.drc]
drt::check_drc -output_file $drc_file -check_pg
diff_files $drc_file drc_test_pg_adv.drcok
