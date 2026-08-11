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
//   test_filler_repair -load <checker.dump.gz>      replay a saved design
//   test_filler_repair -load <checker.dump.gz>
//                      -inst <node-id> -master <master-id>
//
// On a loaded design, `-inst` takes an instance name or numeric node id and
// `-master` takes the replacement master's cell name or Network id. With
// `-load`, both arguments are numeric ids from the dump. Both must be given
// together; with neither, either mode sweeps.
//
// It talks to ImplantLayerChecker DIRECTLY, not through DePlace::isLegal /
// PlacementDRC: this command must be usable before that wiring exists, and
// keeping it out of the way means a failure here is the checker or the repair
// engine, never the dispatch layer around them.
// The dump path reconstructs the helper's Grid, Network, and real checker and
// runs the pure planner over them. It needs no loaded Design or UDM objects.
//
// Prerequisite: run `set_filler_option` first. The repair engine's candidate
// universe is exactly the resulting fillerSetting allow list, so with an empty
// one the command refuses to run rather than reporting a misleading pass.
// Before constructing its checker, the command asks DePlace to register every
// configured filler master with the infrastructure-owned edge table.
//
// No placement is committed. Master registration may extend Network's catalog;
// proposal Node masters are restored and UDM is never written.
// [FRPORT] Optional command surface for exercising the migrated engine.
class TestFillerRepairCmd : public uvTCL::CciCommand
{
 public:
  TestFillerRepairCmd()
      : uvTCL::CciCommand("test_filler_repair",
                           "check implant DRC and report the filler swaps the "
                           "repair engine proposes; use -load "
                           "<checker.dump.gz> to replay a helper dump",
                           false /*echo*/,
                           false /*hidden*/,
                           false /*internal*/),
        instOpt_(this,
                 "inst",
                 "std cell instance name/the node id the sweep prints",
                 false /*isRequired*/,
                 false /*isHidden*/,
                 false /*isPositional*/),
        masterOpt_(this,
                   "master",
                   "replacement master cell name(same width&height)",
                   false /*isRequired*/,
                   false /*isHidden*/,
                   false /*isPositional*/),
        loadOpt_(this,
                 "load",
                 "gzip checker-helper dump to replay",
                 false /*isRequired*/,
                 false /*isHidden*/,
                 false /*isPositional*/)
  {
  }
  bool exec() override;

 private:
  eUNL::CciStringOption instOpt_;
  eUNL::CciStringOption masterOpt_;
  eUNL::CciStringOption loadOpt_;
};

}  // namespace dpl2
