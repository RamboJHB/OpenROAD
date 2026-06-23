# repair_dirty_fillers: delete a dirty filler and refill its footprint.
#
# NOTE: requires the `repair_dirty_fillers` command to be wired in (see
# docs/filler_repair_dpl.md for the SWIG/Tcl/CMake snippets). Not run in the
# sandbox (no swig/bazel build); generate the golden on first successful run.
source "helpers.tcl"
read_lef repair_dirty_fillers_data/impl.lef
read_def repair_dirty_fillers_data/design.def

# dirtyF is a 6-site LVT filler between two LVT cells; mark it dirty and repair.
# The upstream DRC step normally marks dirty fillers; here we pass them by name.
repair_dirty_fillers -masters {FILL_L6 FILL_L4 FILL_L2} -dirty {dirtyF}

check_placement

set def_file [make_result_file repair_dirty_fillers.def]
write_def $def_file
diff_file $def_file repair_dirty_fillers.defok
