set here [file dirname [file normalize [info script]]]
if {![file exists [file join $here filler_repair.lef]]} {
  set here [file normalize [file join [pwd] src dpl2 local testdata]]
}
read_lef [file join $here filler_repair.lef]
read_def [file join $here filler_repair.def]

dpl2_import_db

# Existing real-design sweep: baseline checker gate plus one same-footprint
# proposal per movable standard cell.
test_filler_repair

# Backward-compatible same-footprint master swap.
test_filler_repair -inst U0 -master TH4

# Explicit orientation-only Replace (U0 is MX in the loaded DEF; R180 keeps
# the row-slot polarity while exercising a different legal orientation).
test_filler_repair -operation replace -inst U0 -master TL4 -orient R180

# Opto removes a std cell; filler repair fills the released footprint.
test_filler_repair -operation delete -inst U0

# Opto adds a buffer at an already filler-occupied location; filler repair
# deletes the covered fillers. Orientation is derived from Grid row/site data.
test_filler_repair -operation add -master TL4 -row 0 -col 4

puts "DPL2_REAL_ODB_TEST_PASS"
