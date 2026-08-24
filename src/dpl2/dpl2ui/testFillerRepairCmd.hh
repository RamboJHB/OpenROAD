#pragma once

#include <app/appPackage.hh>
#include <cci/cciCommand.hh>
#include <unl/nlCci.hh>
#include <util/tclCommand.hh>

namespace dpl2 {

// Exercises the checker overlay flow used by opto:
//
//   test_filler_repair -inst {<instance> ...} -master <master>
//
// Instance/master names and numeric Network ids are accepted. The instance
// list may name one committed std cell or every filler covered by one target
// std cell. The command builds a throw-away Node, invokes ImplantLayerChecker,
// and never commits the target replacement or returned filler repairs.
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
        instOpt_(this,
                 "inst",
                 "instance names or Network node ids",
                 false,
                 false,
                 false),
        masterOpt_(this,
                   "master",
                   "replacement master name or Network id",
                   false,
                   false,
                   false)
  {
  }

  bool exec() override;

 private:
  eUNL::CciStringOption instOpt_;
  eUNL::CciStringOption masterOpt_;
};

}  // namespace dpl2
