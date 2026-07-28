// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Debug transcript for the repair pipeline.
//
// Each line states cause -> effect so a captured transcript reads as a
// decision chain ("window L0 -> baseline -> candidate"), including the
// concrete data that changed. Output goes to stdout with a "[fr][stage]"
// prefix. It is ENABLED by default so a production run leaves a diagnosable
// trail; set FR_VERBOSE=0 to silence it. Logging never changes search order
// or acceptance.

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
      std::printf("[fr][%s] %s\n", stage, text.c_str());
      std::fflush(stdout);
    }
  }

  // Deferred form for call sites inside loops: msg(stage, [&] { return
  // cat(...); }). The plain overload above evaluates its argument at the call
  // site, so a silenced log still pays for every cat() -- here the callable
  // only runs when the transcript is on.
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
