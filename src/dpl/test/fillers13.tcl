# filler_placement in a design with mixed 1x and 2x row heights in
# different x-regions.  1x fillers must fill the left (StdSite) region and
# 2x fillers must fill the right (DblSite) region.
source "helpers.tcl"
read_lef fillers13.lef
read_def fillers13.def
filler_placement FILL*
check_placement

set def_file [make_result_file fillers13.def]
write_def $def_file
diff_file $def_file fillers13.defok
