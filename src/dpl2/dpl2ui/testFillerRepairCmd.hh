#pragma once

#include <app/appPackage.hh>
#include <cci/cciCommand.hh>
#include <unl/nlCci.hh>
#include <util/tclCommand.hh>

namespace dpl2 {

// Exercises the checker overlay flow used by opto:
//
//   test_filler_repair -inst {<instance> ...} -master <master>
//   test_filler_repair -inst <std-instance> [-master {}]
//
// Instance/master names and numeric Network ids are accepted. The instance
// list may name one committed std cell or every filler covered by one target
// std cell. An omitted/empty master removes one movable std cell and refills
// its released area through a null-target checker request. Otherwise it checks
// a temporary target. Commit combines the caller-owned std operation with FR's
// complete filler transaction. A std Replace keeps its DB identity/connections.
class TestFillerRepairCmd : public uvTCL::CciCommand
{
 public:
  TestFillerRepairCmd()
      : uvTCL::CciCommand("test_filler_repair",
                          "check and commit std-cell placement/removal with "
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
                   "replacement master name or Network id; empty removes one std cell",
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
