#pragma once

#include <app/appPackage.hh>
#include <unl/nlCci.hh>
#include <util/tclCommand.hh>
#include <cci/cciCommand.hh>

namespace dpl2 {

// test_filler_repair -- exercise the filler VT overlay repair chain on the
// loaded design.
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
  TestFillerRepairCmd() : uvTCL::CciCommand("test_filler_repair",
      "check the loaded design's implant DRC and report the filler swaps "
      "the repair engine proposes for VT changes", false/*echo*/,
      false/*hidden*/, false/*internal*/)
  {
  };

  bool exec() override;
};

}  // namespace dpl2
