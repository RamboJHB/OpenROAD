# Real OpenROAD LEF/DEF input; std commits update Network/Grid only, while
# fillers also update local UDM. The source ODB block stays unchanged.
set here [file dirname [file normalize [info script]]]
if {![file exists [file join $here filler_repair.lef]]} {
  set here [file normalize [file join [pwd] src dpl2 local testdata]]
}
read_lef [file join $here filler_repair.lef]
read_def [file join $here filler_resize.def]

proc odb_placement_snapshot {block} {
  set state {}
  foreach inst [$block getInsts] {
    set bbox [$inst getBBox]
    lappend state [list [$inst getName] [[$inst getMaster] getName] \
        [$bbox xMin] [$bbox yMin] [$bbox xMax] [$bbox yMax] \
        [$inst getOrient] [$inst getPlacementStatus]]
  }
  return [lsort -index 0 $state]
}

proc check_resize {label inst master expected {diameter ""}} {
  set command [list test_filler_repair -inst $inst -master $master]
  if {$diameter ne ""} {
    lappend command -diameter $diameter
  }
  puts "DPL2_RESIZE_CASE $label: $command"
  set code [catch {{*}$command} result]
  if {$expected eq "commit"} {
    if {$code != 0 || $result ne "1"} {
      error "$label: expected a committed resize, got '$result' (Tcl status $code)"
    }
  } elseif {$code != 1 || $result ne "test_filler_repair failed"} {
    error "$label: expected the resize command to reject the placement, got '$result' (Tcl status $code)"
  }
  puts "DPL2_RESIZE_CASE_PASS $label $expected"
}

set block [ord::get_db_block]
set before [odb_placement_snapshot $block]
dpl2_import_db -fillers {FL1 FL2 FH1 FH2}

# Both directions use the same named standard-cell instance across commits.
check_resize grow_in_place UP0 TL4D commit
check_resize shrink_after_grow UP0 TL4 commit 0
check_resize shrink_in_place DOWN2 TL4 commit 0
check_resize regrow_after_shrink DOWN2 TL4D commit 0

# A standard cell blocks the second row. A bounded search can relocate the
# target into neighboring single-row fillers, after the original probe fails.
check_resize blocked_at_original_origin BLOCKED2 TL4D reject 0
check_resize grow_with_bounded_search BLOCKED2 TL4D commit 400
check_resize shrink_at_committed_destination BLOCKED2 TL4 commit 0

# Retry each rejected movable instance with its original master; the local
# projection must still contain a usable instance at its current origin.
check_resize incompatible_row_polarity ODD1 TL4D reject 0
check_resize original_odd_row_cell ODD1 TL4 commit 0
check_resize missing_upper_row TOP4 TL4D reject 0
check_resize original_top_row_cell TOP4 TL4 commit 0
check_resize empty_site_in_target GAP0 TL4D reject 0
check_resize original_gap_neighbor GAP0 TL4 commit 0
check_resize implant_width_violation DRC_UP0 TH4D reject 0
check_resize original_implant_cell DRC_UP0 TL4 commit 0
check_resize fixed_filler_in_target FIXUP0 TL4D reject 0
check_resize search_around_fixed_filler FIXUP0 TL4D commit 2400
check_resize original_fixed_filler_neighbor FIXUP0 TL4 commit 0
check_resize fixed_standard_cell FIXED4 TL4D reject 0

if {[odb_placement_snapshot $block] ne $before} {
  error "resize checks unexpectedly changed the source ODB placement"
}
puts "DPL2_REAL_ODB_RESIZE_TEST_PASS"
