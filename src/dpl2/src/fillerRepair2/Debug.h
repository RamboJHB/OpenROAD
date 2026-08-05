// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The decision trail.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <type_traits>

#include <fillerRepair/RepairTypes.h>

namespace dpl2::fillerRepair {

// Builds a string from stream-printable parts: cat("row=", 3, " x=", 17).
template <typename... Parts>
std::string cat(Parts&&... parts)
{
  std::ostringstream os;
  (os << ... << parts);
  return os.str();
}

inline std::string show(const XInterval& iv)
{
  return cat('[', iv.xl, ',', iv.xh, ')');
}

inline std::string show(const Region& r)
{
  return cat(show(r.x), " rows[", r.rowLo, ',', r.rowHi, ']');
}

// Honours FR_VERBOSE: unset -> on, "0" -> off, anything else -> on.
inline bool debugLoggingDefault()
{
  const char* env = std::getenv("FR_VERBOSE");
  return env == nullptr || std::strcmp(env, "0") != 0;
}

class DebugLog
{
 public:
  explicit DebugLog(bool enabled = true) : enabled_(enabled) {}

  bool enabled() const { return enabled_; }
  void setEnabled(bool enabled) { enabled_ = enabled; }

  void msg(const char* stage, const std::string& text) const
  {
    if (enabled_) {
// No explicit flush: the transcript is on by default, and a flush per
      std::fprintf(stdout, "[fr][%s] %s\n", stage, text.c_str());
    }
  }

// Deferred form for call sites inside loops: msg(stage, [&] { return
  template <typename Fn,
            typename = std::enable_if_t<std::is_invocable_v<const Fn&>>>
  void msg(const char* stage, const Fn& make) const
  {
    if (enabled_) {
      msg(stage, std::string(make()));
    }
  }

 private:
  bool enabled_;
};

}  // namespace dpl2::fillerRepair
