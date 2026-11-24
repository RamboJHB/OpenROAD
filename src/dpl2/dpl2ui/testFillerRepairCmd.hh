#pragma once

#include <app/appPackage.hh>
#include <cci/cciCommand.hh>
#include <unl/nlCci.hh>
#include <util/tclCommand.hh>

#include <vector>

namespace dpl2 {

class DePlace;
class Node;
struct CellChangeRecord;
namespace ipl {
class ImplantLayerChecker;
}

// Exercises the checker overlay flow used by opto:
//
//   test_filler_repair -inst {<instance> ...} -master <master>
//   test_filler_repair -inst <std-instance> -master <master> [-diameter <DBU>]
//   test_filler_repair -inst <std-instance> [-master {}]
//
// Instance/master names and numeric Network ids are accepted. The instance
// list may name one committed std cell or every filler covered by one target
// std cell. An omitted/empty master removes one movable std cell and refills
// its released area through a null-target checker request. Otherwise it checks
// a temporary target. A single std-cell resize tries its original position by
// default; a positive diameter allows the test command to search nearby.
// Commit combines the caller-owned std operation with FR's complete filler
// transaction. A std Replace keeps its DB identity/connections.
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
                   false),
        diameterOpt_(this,
                   "diameter",
                   "nonnegative resize search extension in DBU; default 0",
                   false,
                   false,
                   false)
  {
  }

  bool exec() override;

 private:
  bool probeResize(DePlace& placement,
                   const ipl::ImplantLayerChecker& checker,
                   const Node& old,
                   Node& target,
                   int diameter,
                   std::vector<CellChangeRecord>& changes) const;

  eUNL::CciStringOption instOpt_;
  eUNL::CciStringOption masterOpt_;
  eUNL::CciStringOption diameterOpt_;
};

}  // namespace dpl2
