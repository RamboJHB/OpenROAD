# Geometry smoke on a real placed GCD. This Nangate45 LEF has no implant
# layers; the controlled mixed-height smoke exercises implant DRC separately.
set here [file dirname [file normalize [info script]]]
set root [file normalize [file join $here ../../../..]]
read_lef [file join $root test Nangate45 Nangate45.lef]
read_lef -library [file join $here nangate45_resize_target.lef]
read_def [file join $root src dpo test gcd.def]

set db [ord::get_db]
set block [ord::get_db_block]
set target [$block findInst _281_]
set filler [$db findMaster FILLCELL_X1]
set width [$filler getWidth]
set height [$filler getHeight]
lassign [$target getLocation] x y
set upperY [expr {$y + $height}]
set upperRow {}
foreach row [$block getRows] {
  if {[$row getName] eq "ROW_54"} {
    set upperRow $row
    break
  }
}
if {$upperRow eq "" || [[$target getMaster] getName] ne "INV_X1"
    || $width != 380 || $height != 2800 || $x != 144280 || $upperY != 179200
    || [[$upperRow getBBox] yMin] != $upperY} {
  error "Nangate45 GCD anchor geometry changed"
}

# Populate only the two adjacent sites in the existing empty upper row.
foreach inst [$block getInsts] {
  if {![$inst isPlaced]} {
    continue
  }
  set box [$inst getBBox]
  if {[$box xMin] < $x + 2 * $width && [$box xMax] > $x
      && [$box yMin] < $upperY + $height && [$box yMax] > $upperY} {
    error "Nangate45 GCD resize region already occupied by [$inst getName]"
  }
}
foreach offset {0 1} {
  set inst [odb::dbInst_create $block $filler DPL2_RESIZE_FILL_$offset true]
  $inst setOrient [$upperRow getOrient]
  $inst setLocation [expr {$x + $offset * $width}] $upperY
  $inst setPlacementStatus PLACED
}

proc nangate_resize_source_snapshot {block} {
  set placement {}
  foreach inst [$block getInsts] {
    lappend placement [list [$inst getName] [[$inst getMaster] getName] \
        [$inst getLocation] [$inst getOrient] [$inst getPlacementStatus]]
  }
  set connections {}
  foreach net [$block getNets] {
    set endpoints {}
    foreach term [$net getITerms] {
      lappend endpoints [list [[$term getInst] getName] [[$term getMTerm] getName]]
    }
    lappend connections [list [$net getName] [lsort $endpoints]]
  }
  return [list [lsort $placement] [lsort $connections]]
}

set before [nangate_resize_source_snapshot $block]
dpl2_import_db
foreach master {INV_X1_DPL2_2H INV_X1} {
  if {[test_filler_repair -inst _281_ -master $master] ne "1"} {
    error "Nangate45 resize to $master did not commit"
  }
  # Success applies only to the local UDM projection. This check does not
  # exercise net handling or writeback in the destination database.
  if {[nangate_resize_source_snapshot $block] ne $before} {
    error "Nangate45 resize to $master changed source ODB placement or connectivity"
  }
}
puts "DPL2_REAL_NANGATE45_RESIZE_TEST_PASS"
