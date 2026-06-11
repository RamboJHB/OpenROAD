# `check_drc -check_pg` on the same comprehensive design as drc_test_pg but with
# NO GCELLGRID and NO route guides. check_drc must synthesize a gcell grid on its
# own (buildGCellPatterns falls back to a track-pitch/die-derived grid when
# guides can't supply one), so it runs with no crash and no `No GCELLGRIDX`
# error. The reported violations are the SAME SET as drc_test_pg (20 markers);
# only their order differs, because the marker emission order follows the gcell
# tile sweep and the synthesized pitch differs from the DEF grid. This confirms
# the gcell tiling is only worker scaffolding and does not change DRC results.
source "helpers.tcl"
read_lef drc_test_pg.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def drc_test_pg_nogcell.def
set_thread_count 1
set drc_file [make_result_file drc_test_pg_nogcell.drc]
check_drc -output_file $drc_file -check_pg
diff_files $drc_file drc_test_pg_nogcell.drcok
