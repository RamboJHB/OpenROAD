# `check_drc -check_pg` on a DEF that has NO GCELLGRID and NO route guides.
# `setup_check_drc` synthesizes a gcell grid + a dummy route guide in odb so
# check_drc can tile/run; the violations must match drc_test_pg byte-for-byte
# (the gcell tiling is only worker scaffolding and does not affect DRC results).
source "helpers.tcl"
read_lef drc_test_pg.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def drc_test_pg_nogcell.def
set_thread_count 1
setup_check_drc
set drc_file [make_result_file drc_test_pg_nogcell.drc]
check_drc -output_file $drc_file -check_pg
diff_files $drc_file drc_test_pg_nogcell.drcok
