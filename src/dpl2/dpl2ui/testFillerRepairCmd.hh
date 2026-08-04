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
//   test_filler_repair -inst <inst> -master <name>  one specific VT swap
//
// `-inst` takes an instance name, or the numeric node id the sweep prints.
// `-master` takes the replacement master's cell name. Both must be given
// together; with neither, the command sweeps.
//
// It talks to ImplantLayerChecker DIRECTLY, not through DePlace::isLegal /
// PlacementDRC: this command must be usable before that wiring exists, and
// keeping it out of the way means a failure here is the checker or the repair
// engine, never the dispatch layer around them.
// Because this command constructs its own checker, exec() also supplies that
// instance's repair context before the first check.
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
  TestFillerRepairCmd()
      : uvTCL::CciCommand("test_filler_repair",
          "check implant DRC and report the filler swaps the repair engine "
          "proposes; -inst <inst> -master <name> for one specific VT swap",
          false /*echo*/, false /*hidden*/, false /*internal*/),
      instOpt_(this, "inst", "std cell instance name/the node id the sweep prints",
               false /*isRequired*/, false /*isHidden*/,false /*isPositional*/),
      masterOpt_(this, "master", "replacement master cell name(same width&height)",
                 false /*isRequired*/, false /*isHidden*/, false /*isPositional*/)
   {};
  bool exec() override;

 private:
  eUNL::CciStringOption instOpt_;
  eUNL::CciStringOption masterOpt_;
};

}  // namespace dpl2
