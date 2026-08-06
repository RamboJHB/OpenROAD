#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace uvTCL {

// Local substitute for the destination command framework. It only implements
// the string-option surface used by TestFillerRepairCmd.
class CciCommand
{
 public:
  CciCommand(const char* name, const char*, bool, bool, bool) : name_(name) {}
  virtual ~CciCommand() = default;
  virtual bool exec() = 0;

  void registerStringOption(const std::string& name,
                            std::function<void(std::string)> setter)
  {
    options_[name] = std::move(setter);
  }

  bool run(const std::vector<std::string>& args, std::string& error)
  {
    for (const auto& [name, setter] : options_) {
      (void) name;
      setter({});
    }
    for (size_t index = 0; index < args.size(); index += 2) {
      if (index + 1 >= args.size() || args[index].size() < 2
          || args[index].front() != '-') {
        error = "options must be -name value pairs";
        return false;
      }
      const std::string name = args[index].substr(1);
      const auto found = options_.find(name);
      if (found == options_.end()) {
        error = "unknown option: -" + name;
        return false;
      }
      found->second(args[index + 1]);
    }
    return exec();
  }

 private:
  std::string name_;
  std::map<std::string, std::function<void(std::string)>> options_;
};

}  // namespace uvTCL
