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
//   test_filler_repair                               sweep master swaps
//   test_filler_repair -operation replace
//                      -inst <inst> -master <name> [-orient <orient>]
//   test_filler_repair -operation delete -inst <inst>
//   test_filler_repair -operation add -master <name>
//                      -row <row> -col <column> [-orient <orient>]
//   test_filler_repair -load <checker.dump.gz>       replay a saved design
//   test_filler_repair -load <checker.dump.gz>
//                      -inst <node-id> -master <master-id>
//
// On a loaded design, `-inst` takes an instance name or numeric Node id and
// `-master` takes a master cell name or Network id. Omitting `-operation`
// preserves the old interface: no target options means sweep, while paired
// `-inst/-master` means Replace. Add tests opto's request-local buffer path at
// the selected Grid row/column; Delete tests filling the removed std-cell
// footprint. Supported explicit orientations are R0, R180, MX, and MY.
// With `-load`, instance/master arguments are numeric dump ids and only the
// existing sweep/Replace replay is available.
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
// configured filler and requested target master with the infrastructure-owned
// edge table.
//
// No placement is committed. Master registration may extend Network's catalog
// before the engine snapshot; every evaluated request verifies that UDM,
// Network, and Grid state remain unchanged.
// [FRPORT] Optional command surface for exercising the migrated engine.
class TestFillerRepairCmd : public uvTCL::CciCommand
{
 public:
  TestFillerRepairCmd()
      : uvTCL::CciCommand("test_filler_repair",
                           "test std-cell Replace/Delete/Add and report the "
                           "complete filler transaction; use -load "
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
                   "Replace/Add standard-cell master name or Network id",
                   false /*isRequired*/,
                   false /*isHidden*/,
                   false /*isPositional*/),
        operationOpt_(this,
                      "operation",
                      "target operation: replace, delete, or add",
                      false /*isRequired*/,
                      false /*isHidden*/,
                      false /*isPositional*/),
        orientOpt_(this,
                   "orient",
                   "target orientation: R0, R180, MX, or MY",
                   false /*isRequired*/,
                   false /*isHidden*/,
                   false /*isPositional*/),
        rowOpt_(this,
                "row",
                "Grid row for an Add target",
                false /*isRequired*/,
                false /*isHidden*/,
                false /*isPositional*/),
        colOpt_(this,
                "col",
                "Grid column for an Add target",
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
  eUNL::CciStringOption operationOpt_;
  eUNL::CciStringOption orientOpt_;
  eUNL::CciStringOption rowOpt_;
  eUNL::CciStringOption colOpt_;
  eUNL::CciStringOption loadOpt_;
};

}  // namespace dpl2
