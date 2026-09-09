source "helpers.tcl"
read_lef multi_height_fillers.lef
read_def fillers_multi_height.def

filler_placement -prefix "MH_" {FILLER_X2_H2 FILLER_X1}

set placements {}
foreach inst [[ord::get_db_block] getInsts] {
  lappend placements "[$inst getName] [[$inst getMaster] getName] [$inst getLocation] [$inst getOrient] [$inst getPlacementStatus] [$inst getSourceType]"
}
puts "instances: [llength $placements]"
foreach placement [lsort $placements] {
  puts $placement
}
check_placement
