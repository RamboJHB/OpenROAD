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
//   test_filler_repair -inst <inst> -master <name>  one specific replacement
//
// `-inst` takes a standard-cell or filler instance name, or the numeric node
// id the sweep prints. `-master` takes a same-footprint standard-cell master.
// Both must be given together; with neither, the command sweeps both std->std
// and filler->std replacements.
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
// Before constructing its checker, the command asks DePlace to register every
// configured filler master with the infrastructure-owned edge table.
//
// No placement is committed. Master registration may extend Network's catalog;
// proposals use temporary Nodes and UDM is never written.
class TestFillerRepairCmd : public uvTCL::CciCommand
{
 public:
  TestFillerRepairCmd()
      : uvTCL::CciCommand("test_filler_repair",
          "check implant DRC and report the filler swaps the repair engine "
          "proposes; -inst <inst> -master <name> for one replacement",
          false /*echo*/, false /*hidden*/, false /*internal*/),
      instOpt_(this, "inst", "std/filler instance name or printed node id",
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
