source "helpers.tcl"
read_lef multi_height_fillers.lef
read_def fillers_multi_height_exact_failure.def

catch {filler_placement -prefix "FAIL_" FILLER_X2_H2} message
puts $message
puts "instances after exact failure: [llength [[ord::get_db_block] getInsts]]"
