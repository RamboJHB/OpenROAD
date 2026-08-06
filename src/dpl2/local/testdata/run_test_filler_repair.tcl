set here [file dirname [file normalize [info script]]]
if {![file exists [file join $here filler_repair.lef]]} {
  set here [file normalize [file join [pwd] src dpl2 local testdata]]
}
read_lef [file join $here filler_repair.lef]
read_def [file join $here filler_repair.def]

dpl2_import_db
test_filler_repair -inst U0 -master TH4
puts "DPL2_REAL_ODB_TEST_PASS"
