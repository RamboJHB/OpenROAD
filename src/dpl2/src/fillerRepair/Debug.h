// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Debug transcript for the repair pipeline.
//
// Each line states cause -> effect so a captured transcript reads as a
// decision chain ("window L0 -> baseline -> candidate"), including the
// concrete data that changed. Output goes to stdout with a "[fr][stage]"
// prefix and is disabled by default; enabling it never changes search order
// or acceptance.

#pragma once

#include <cstdio>
#include <sstream>
#include <string>

#include "RepairTypes.h"

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

class DebugLog
{
 public:
  explicit DebugLog(bool enabled = false) : enabled_(enabled) {}

  bool enabled() const { return enabled_; }
  void setEnabled(bool enabled) { enabled_ = enabled; }

  void msg(const char* stage, const std::string& text) const
  {
    if (enabled_) {
      std::printf("[fr][%s] %s\n", stage, text.c_str());
      std::fflush(stdout);
    }
  }

 private:
  bool enabled_;
};

}  // namespace dpl2::fillerRepair
