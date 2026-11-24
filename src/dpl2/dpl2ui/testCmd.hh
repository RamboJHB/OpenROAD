#pragma once

#include <app/appPackage.hh>
#include <unl/nlCci.hh>
#include <util/tclCommand.hh>
#include <cci/cciCommand.hh>

namespace dpl2 {

class TestEcoFlowCmd : public uvTCL::CciCommand
{
 public:
  TestEcoFlowCmd() : uvTCL::CciCommand("test_eco_flow",
      "test eco flow with DRC checker + fake DrcFixer",
      false/*echo*/, false/*hidden*/, false/*internal*/)
  {
  }

  bool exec() override;

  // Command-local test helper.
  class FakeDrcFixer;
};

}  // namespace dpl2
