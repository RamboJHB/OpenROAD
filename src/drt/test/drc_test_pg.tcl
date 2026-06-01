# Toy case for `check_drc -check_pg`.
#
# Uses the Nangate45 layer stack with three extra rules injected into metal2
# (AREA / MINSTEP / MINIMUMCUT, see drc_test_pg.lef) that stock Nangate45 lacks.
# The design has spatially-separated clusters, each producing one PG DRC so the
# golden exercises every rule expressible in PG-only mode:
#   * PG-to-PG       Short + Metal Spacing   (VDD vs VSS,  metal2)
#   * PG-to-signal   Short + Metal Spacing   (VDD vs net1/net2, metal2)
#   * PG-to-blockage Short + Metal Spacing   (VDD vs metal2 OBS)
#   * PG Min Width                           (narrow VDD strap, metal2)
#   * PG Off Grid                            (off-grid VDD strap, metal2)
#   * PG Cut Spacing                         (two VDD via1 cuts too close)
#   * PG Min Area                            (small VDD metal2 shape)
#   * PG Minimum Cut                         (wide VDD strap, single via1 cut)
# The metal-spacing checks use metal2's width-dependent SPACINGTABLE (PRL), so
# they also cover layer/width-dependent spacing. (Min Area is special-cased to
# emit a marker in PG mode; see docs/agents/drc_check_pg.md sec 2.2.)
#
# In PG-only mode PG geometry is loaded NON-fixed (under test) and everything
# else (signal pins+routing, blockages) is loaded FIXED (background); the GC
# engine skips pairs where both shapes are fixed. So `check_drc -check_pg`
# reports exactly the violations that involve PG and suppresses the pure
# signal-to-signal short (net1/net2 on metal1). A plain check_drc instead
# reports that signal-to-signal short and not the both-fixed PG/PG, PG/OBS
# pairs. See docs/agents/drc_check_pg.md.
#
# Min Step is not surfaced by standalone check_drc, and boundary/ring-perimeter
# checks do not exist in the OpenROAD GC engine; both are out of scope.
source "helpers.tcl"
read_lef drc_test_pg.lef
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
