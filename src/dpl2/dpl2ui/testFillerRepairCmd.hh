#pragma once

#include <app/appPackage.hh>
#include <unl/nlCci.hh>
#include <util/tclCommand.hh>
#include <cci/cciCommand.hh>

#include <string>

namespace dpl2 {

// test_filler_repair -- exercise the filler VT overlay repair chain on the
// loaded design.
//
//   test_filler_repair                          sweep every movable cell
//   test_filler_repair -inst <id> -master <id>  one specific VT swap
//
// Both options take **id numbers**, not names: `-inst` the instance id
// (`LeafCellID`) and `-master` the replacement master's id (`LibCellID`) --
// the same two handles `DePlace::isLegal(LeafCellID, LibCellID, fcRecord)`
// takes, so a drill-in reproduces exactly the call opto makes. The values are
// the ids' index values, which is what the sweep prints, so a reported line
// pastes straight back as options. Both must be given together; with neither,
// the command sweeps.
//
// It talks to ImplantLayerChecker DIRECTLY, not through DePlace::isLegal /
// PlacementDRC: this command must be usable before that wiring exists, and
// keeping it out of the way means a failure here is the checker or the repair
// engine, never the dispatch layer around them.
//
// Prerequisite: run `set_filler_option` first. The repair engine's candidate
// universe is exactly the resulting fillerSetting allow list, so with an empty
// one the command refuses to run rather than reporting a misleading pass.
//
// Nothing is committed. Node masters are swapped to a proposal, checked, and
// restored; UDM is never written.
class TestFillerRepairCmd : public uvTCL::CciCommand
{
 public:
  TestFillerRepairCmd();

  bool exec() override;

 private:
  // The only two places this file touches the command framework's option
  // API; see the ADAPT block at the top of the .cc.
  void declareOptions();
  bool readOption(const char* name, std::string& value) const;
};

}  // namespace dpl2
