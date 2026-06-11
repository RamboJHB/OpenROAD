# Comprehensive `check_drc -check_pg` toy -- all PG DRC scenarios in one design.
#
# Layout: three rows of clusters, one column (A..L) per violation type, plus
# extra clusters (M..N) and a clean special PDN mesh in the top band:
#   row 1 (y~100um): violation is PG-vs-PG     (VDD vs VSS)   -> reported
#   row 2 (y~68um) : violation is PG-vs-signal (VDD vs SIG1)  -> reported
#   row 3 (y~32um) : violation is signal-vs-sig(SIG1 vs SIG2) -> SUPPRESSED
# Each VDD cluster in the DEF is annotated with the constraint it triggers:
#   A Short        B Min Width   C Metal Spacing  D Cut Spacing  E Min Hole
#   F Off Grid     G Min Area    H Minimum Cut    I Lef58Area    J Lef58 EOL
#   K Lef58EolExtension          L Lef58EolKeepOut
#   M VDD vs metal2 blockage (Short + Metal Spacing)
#   N VDD 2nd PG connection (secondary branch) vs regular signal reg1 -> Metal Spacing
#   + a DRC-clean special PDN mesh (VDD/VSS metal4 stripes + metal5 rails)
#
# PG (VDD/VSS) is loaded non-fixed under -check_pg; everything else (SIG1/SIG2
# `USE SIGNAL`, the regular net reg1, blockages) is loaded fixed. Both-fixed
# pairs are skipped, so the signal-vs-signal row produces no markers. PG is
# classified by sigType.isSupply(), so every VDD shape -- main mesh, secondary
# branch, blockage-adjacent strap -- is checked. See docs/agents/drc_check_pg.md.
source "helpers.tcl"
read_lef drc_test_pg.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def drc_test_pg.def
set_thread_count 1
set drc_file [make_result_file drc_test_pg.drc]
check_drc -output_file $drc_file -check_pg
diff_files $drc_file drc_test_pg.drcok
