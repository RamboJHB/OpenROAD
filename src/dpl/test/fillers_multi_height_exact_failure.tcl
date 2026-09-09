source "helpers.tcl"
read_lef multi_height_fillers.lef
read_def fillers_multi_height_exact_failure.def

set_filler_option -prefix "FAIL_" -follow_order true -fit_space true \
  FILLER_X2_H2
catch {filler_placement} message
puts $message
puts "instances after exact failure: [llength [[ord::get_db_block] getInsts]]"
