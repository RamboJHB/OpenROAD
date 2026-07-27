#pragma once

#include <app/appPackage.hh>
#include <unl/nlCci.hh>
#include <util/tclCommand.hh>
#include <cci/cciCommand.hh>

// Forward declarations
namespace eUNL {
  class Design;
}

namespace dpl2 {

class DePlace;

class TestEcoFlowCmd : public uvTCL::CciCommand
{
 public:
  TestEcoFlowCmd() : uvTCL::CciCommand("test_eco_flow",
      "test eco flow with DRC checker +
      fake DrcFixer", false/*echo*/, false/*hidden*/, false/*internal*/)
  {
  };

  bool exec() override;

  // Nested class to inherit friend access to DePlace private members.
  class FakeDrcFixer;
};

}  // namespace dpl2