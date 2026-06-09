# `check_drc -check_pg` on a DEF that has NO GCELLGRID and NO route guides.
# check_drc must synthesize a gcell grid on its own (buildGCellPatterns falls
# back to a track-pitch/die-derived grid when guides can't supply one), so it
# runs with no crash and no `No GCELLGRIDX` error. The violations must match
# drc_test_pg byte-for-byte (the gcell tiling is only worker scaffolding and
# does not affect DRC results).
source "helpers.tcl"
read_lef drc_test_pg.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def drc_test_pg_nogcell.def
set_thread_count 1
set drc_file [make_result_file drc_test_pg_nogcell.drc]
check_drc -output_file $drc_file -check_pg
diff_files $drc_file drc_test_pg_nogcell.drcok
