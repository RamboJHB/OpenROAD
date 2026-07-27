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

// Exercises the filler VT repair chain end to end through
// DePlace::isLegal -> checkDRC -> ImplantLayerChecker::check(fcRecord&).
// Prerequisite: the caller ran set_filler_option first, so
// DePlace::getFillerSetting() carries the filler master allow list the
// lazily-initialized FillerRepairEngine consumes.
class TestFillerRepairCmd : public uvTCL::CciCommand
{
 public:
  TestFillerRepairCmd() : uvTCL::CciCommand("test_filler_repair",
      "check each placed cell; report the filler swaps the repair "
      "engine proposes for DRC-illegal ones", false/*echo*/,
      false/*hidden*/, false/*internal*/)
  {
  };

  bool exec() override;
};

}  // namespace dpl2