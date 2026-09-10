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
// std cell. The command checks a temporary Node, then commits its target,
// caller Delete overlays and returned filler Add/Replace records together.
// A one-to-one std replacement keeps the old DB identity and connections.
class TestFillerRepairCmd : public uvTCL::CciCommand
{
 public:
  TestFillerRepairCmd()
      : uvTCL::CciCommand("test_filler_repair",
                          "check and commit std-cell placement with "
                          "gap filling and surrounding filler repair",
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
