#pragma once
#include <string>
#include <iostream>
#include <stdexcept>
namespace utl {
enum ToolId { DPL };
class Logger {
 public:
  template <typename... A> void info(ToolId, int id, const std::string& f, A&&...){
    std::cout << "[INFO DPL-" << id << "] " << f << "\n";
  }
  template <typename... A> void error(ToolId, int id, const std::string& f, A&&...){
    throw std::runtime_error("DPL-" + std::to_string(id) + ": " + f);
  }
  template <typename... A> void warn(ToolId, int id, const std::string& f, A&&...){
    std::cout << "[WARN DPL-" << id << "] " << f << "\n";
  }
};
}  // namespace utl
