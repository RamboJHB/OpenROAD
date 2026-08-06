#pragma once

#include <cci/cciCommand.hh>

#include <string>
#include <utility>

namespace eUNL {

class CciStringOption
{
 public:
  CciStringOption(uvTCL::CciCommand* command,
                  const char* name,
                  const char*,
                  bool,
                  bool,
                  bool)
  {
    command->registerStringOption(
        name, [this](std::string value) { value_ = std::move(value); });
  }

  const std::string& getValue() const { return value_; }

 private:
  std::string value_;
};

}  // namespace eUNL
