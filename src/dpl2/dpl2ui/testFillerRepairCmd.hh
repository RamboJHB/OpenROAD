#pragma once

#include <app/appPackage.hh>
#include <cci/cciCommand.hh>
#include <unl/nlCci.hh>
#include <util/tclCommand.hh>

namespace dpl2 {

// Exercises the same-footprint replacement flow used by opto:
//
//   test_filler_repair
//   test_filler_repair -inst <instance> -master <master>
//   test_filler_repair -load <checker.dump.gz>
//   test_filler_repair -load <checker.dump.gz>
//                      -inst <node-id> -master <master-id>
//
// With a live design, names or numeric Network ids are accepted. With -load,
// ids must be numeric values from the checker-helper dump. The command never
// commits the target swap or returned filler replacements.
class TestFillerRepairCmd : public uvTCL::CciCommand
{
 public:
  TestFillerRepairCmd()
      : uvTCL::CciCommand("test_filler_repair",
                          "test same-footprint std-cell replacement and "
                          "surrounding filler repair",
                          false,
                          false,
                          false),
        instOpt_(this, "inst", "instance name or Network node id", false,
                 false, false),
        masterOpt_(this, "master", "replacement master name or Network id",
                   false, false, false),
        loadOpt_(this, "load", "gzip checker-helper dump to replay", false,
                 false, false)
  {
  }

  bool exec() override;

 private:
  eUNL::CciStringOption instOpt_;
  eUNL::CciStringOption masterOpt_;
  eUNL::CciStringOption loadOpt_;
};

}  // namespace dpl2
