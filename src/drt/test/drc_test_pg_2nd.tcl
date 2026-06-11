# `check_drc -check_pg` with a "2nd PG connection".
#
# VDD is a POWER special net whose geometry is an L: a horizontal main rail plus
# a vertical *secondary branch* dropping toward the cell area (a 2nd PG
# connection, not the main mesh). The branch runs parallel to a regular signal
# wire (sig1, metal2) at sub-spacing distance.
#
# The PG classifier keys on the net sigType (frNet->getType().isSupply()), a
# net-level property, so EVERY VDD shape -- including the secondary branch -- is
# treated as PG. The branch-vs-sig1 Metal Spacing violation therefore shows up
# under -check_pg as `net:VDD net:sig1`, confirming a secondary PG connection is
# checked (it does not depend on isSpecial() per-segment wiring).
source "helpers.tcl"
read_lef drc_test_pg.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def drc_test_pg_2nd.def
set_thread_count 1
set drc_file [make_result_file drc_test_pg_2nd.drc]
check_drc -output_file $drc_file -check_pg
diff_files $drc_file drc_test_pg_2nd.drcok
