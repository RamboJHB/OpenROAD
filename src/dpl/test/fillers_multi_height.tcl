source "helpers.tcl"
read_lef multi_height_fillers.lef
read_def fillers_multi_height.def

set_filler_option -prefix "MH_" -follow_order true -fit_space true \
  {FILLER_X2_H2 FILLER_X1}
filler_placement

set placements {}
foreach inst [[ord::get_db_block] getInsts] {
  lappend placements "[$inst getName] [[$inst getMaster] getName] [$inst getLocation] [$inst getOrient] [$inst getPlacementStatus] [$inst getSourceType]"
}
puts "instances: [llength $placements]"
foreach placement [lsort $placements] {
  puts $placement
}
check_placement
