// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The decision trail.
//
// Every line says what happened and why, so reading a captured run top to
// bottom explains the answer: which window, what the baseline was, which
// candidates were tried, what blocked them, where it grew next. Lines are
// prefixed "[fr][stage]" and go to stdout.
//
// ON by default -- a production run that gets a surprising answer should
// already have the evidence, without a rebuild and a rerun. FR_VERBOSE=0
// silences it. Logging never changes what the search does or accepts.

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
      // line is a syscall per line on a path that emits thousands. Normal
      // stdio buffering already gives the behaviour each use wants -- line
      // buffered on a terminal (interactive debugging sees each line as it
      // happens), block buffered when redirected to a file (bulk runs pay
      // almost nothing).
      std::fprintf(stdout, "[fr][%s] %s\n", stage, text.c_str());
    }
  }

  // Coarse progress markers are explicitly flushed so a redirected transcript
  // still identifies the call that has not returned. Keep these out of hot
  // per-candidate paths unless they bracket an expensive external operation.
  void checkpoint(const char* stage, const std::string& text) const
  {
    if (enabled_) {
      std::fprintf(stdout, "[fr][%s] %s\n", stage, text.c_str());
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
