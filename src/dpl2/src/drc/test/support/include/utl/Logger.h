// Fake utl/Logger.h (tier-1 local build): the real one needs spdlog. The
// infrastructure code only holds a Logger* (all call sites are commented
// out), so an empty type with the debugPrint macro stub is sufficient.
#pragma once
#include <string>
namespace utl {
enum ToolId { DPL };
class Logger
{
 public:
  template <typename... Args> void error(Args&&...) {}
  template <typename... Args> void warn(Args&&...) {}
  template <typename... Args> void info(Args&&...) {}
  template <typename... Args> void report(Args&&...) {}
};
}  // namespace utl
#ifndef debugPrint
#define debugPrint(logger, tool, group, level, ...) \
  do {                                              \
  } while (false)
#endif
