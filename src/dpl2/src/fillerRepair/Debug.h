// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// The decision trail.
//
// Every line says what happened and why, so reading a captured run top to
// bottom explains the answer: which window, what the baseline was, which
// candidates were tried, what blocked them, where it grew next. Lines are
// prefixed "[fr][stage]" and go to stdout.
//
// ON by default -- a runtime run that gets a surprising answer should
// already have the evidence, without a rebuild and a rerun. FR_VERBOSE=0
// silences it. Logging never changes what the search does or accepts.

#pragma once

#include <fillerRepair/RepairTypes.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

struct DebugField
{
  std::string name;
  std::string value;
};

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
      emit(stage, splitAndWrap(text), false);
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

  // A visible phase boundary. The leading blank line prevents initialization,
  // snapshot, search-window, and result records from becoming one dense wall
  // of text in a redirected transcript.
  void section(const char* stage, const std::string& title) const
  {
    if (enabled_) {
      emit(stage, {cat("===== ", title, " =====")}, true);
    }
  }

  // A named record. Each value stays paired with its label and long values
  // wrap beneath the record instead of extending one terminal-wide line.
  void block(const char* stage,
             const std::string& title,
             std::initializer_list<DebugField> fields) const
  {
    if (!enabled_) {
      return;
    }
    size_t labelWidth = 0;
    for (const DebugField& field : fields) {
      labelWidth = std::max(labelWidth, field.name.size());
    }
    labelWidth = std::min(labelWidth, kMaxLabelWidth);

    std::vector<std::string> lines{title};
    lines.reserve(fields.size() + 1);
    for (const DebugField& field : fields) {
      const std::string label
          = field.name.size() < labelWidth
                ? field.name + std::string(labelWidth - field.name.size(), ' ')
                : field.name;
      const std::vector<std::string> values = splitLogicalLines(field.value);
      lines.push_back(cat("  ", label, " : ", values.front()));
      const std::string continuation(labelWidth + 5, ' ');
      for (size_t i = 1; i < values.size(); ++i) {
        lines.push_back(continuation + values[i]);
      }
    }
    emit(stage, wrapLines(lines), false);
  }

  void list(const char* stage,
            const std::string& title,
            const std::vector<std::string>& items) const
  {
    if (!enabled_) {
      return;
    }
    std::vector<std::string> lines{title};
    lines.reserve(items.size() + 1);
    if (items.empty()) {
      lines.emplace_back("  (none)");
    } else {
      for (const std::string& item : items) {
        lines.push_back(cat("  - ", item));
      }
    }
    emit(stage, wrapLines(lines), false);
  }

  // A compact table with adaptive column widths. Wide cells are wrapped in
  // place, so adding a long diagnostic or master description never recreates
  // the unreadable one-line transcript this interface is meant to avoid.
  void table(const char* stage,
             const std::string& title,
             const std::vector<std::string>& headers,
             const std::vector<std::vector<std::string>>& rows) const
  {
    if (!enabled_) {
      return;
    }
    if (headers.empty()) {
      list(stage, title, {});
      return;
    }

    std::vector<size_t> widths(headers.size(), kMinColumnWidth);
    for (size_t col = 0; col < headers.size(); ++col) {
      widths[col] = std::min(kMaxColumnWidth,
                             std::max(kMinColumnWidth, headers[col].size()));
    }
    for (const auto& row : rows) {
      for (size_t col = 0; col < std::min(row.size(), widths.size()); ++col) {
        widths[col]
            = std::min(kMaxColumnWidth,
                       std::max(widths[col], longestLogicalLine(row[col])));
      }
    }

    const size_t separators = (headers.size() - 1) * 3;
    const size_t available = kContentWidth - 2;
    while (sum(widths) + separators > available) {
      const auto widest = std::max_element(widths.begin(), widths.end());
      if (widest == widths.end() || *widest <= kMinColumnWidth) {
        break;
      }
      --*widest;
    }

    std::vector<std::string> lines{title};
    appendTableRow(lines, headers, widths);
    std::string divider = "  ";
    for (size_t col = 0; col < widths.size(); ++col) {
      if (col != 0) {
        divider += "-+-";
      }
      divider += std::string(widths[col], '-');
    }
    lines.push_back(std::move(divider));
    if (rows.empty()) {
      lines.emplace_back("  (none)");
    } else {
      for (const auto& row : rows) {
        appendTableRow(lines, row, widths);
      }
    }
    emit(stage, lines, false);
  }

 private:
  static constexpr size_t kContentWidth = 96;
  static constexpr size_t kMaxLabelWidth = 28;
  static constexpr size_t kMinColumnWidth = 3;
  static constexpr size_t kMaxColumnWidth = 28;

  static std::vector<std::string> splitLogicalLines(const std::string& text)
  {
    std::vector<std::string> lines;
    size_t begin = 0;
    do {
      const size_t end = text.find('\n', begin);
      lines.push_back(text.substr(begin, end - begin));
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1;
    } while (begin <= text.size());
    return lines;
  }

  static std::vector<std::string> wrapLine(const std::string& line,
                                           size_t width)
  {
    if (line.empty() || line.size() <= width) {
      return {line};
    }

    const size_t firstText = line.find_first_not_of(' ');
    const size_t indent = firstText == std::string::npos ? 0 : firstText;
    const std::string continuation(
        std::min(indent + 2, width > 1 ? width - 1 : size_t{0}), ' ');
    std::vector<std::string> wrapped;
    std::string remaining = line;
    std::string prefix;
    while (prefix.size() + remaining.size() > width) {
      const size_t available = width - prefix.size();
      const size_t boundary = remaining.find_last_of(" ,;}", available - 1);
      size_t take = boundary;
      if (boundary != std::string::npos && remaining[boundary] != ' ') {
        take = boundary + 1;
      }
      if (take == std::string::npos || take == 0) {
        take = available;
      }
      wrapped.push_back(prefix + remaining.substr(0, take));
      remaining.erase(0, take);
      const size_t next = remaining.find_first_not_of(' ');
      remaining.erase(0, next == std::string::npos ? remaining.size() : next);
      prefix = continuation;
    }
    wrapped.push_back(prefix + remaining);
    return wrapped;
  }

  static std::vector<std::string> wrapLines(
      const std::vector<std::string>& lines)
  {
    std::vector<std::string> wrapped;
    for (const std::string& line : lines) {
      const std::vector<std::string> pieces = wrapLine(line, kContentWidth);
      wrapped.insert(wrapped.end(), pieces.begin(), pieces.end());
    }
    return wrapped;
  }

  static std::vector<std::string> splitAndWrap(const std::string& text)
  {
    return wrapLines(splitLogicalLines(text));
  }

  static size_t longestLogicalLine(const std::string& text)
  {
    size_t length = 0;
    for (const std::string& line : splitLogicalLines(text)) {
      length = std::max(length, line.size());
    }
    return length;
  }

  static size_t sum(const std::vector<size_t>& values)
  {
    size_t total = 0;
    for (const size_t value : values) {
      total += value;
    }
    return total;
  }

  static std::string pad(const std::string& value, size_t width)
  {
    return value.size() < width ? value + std::string(width - value.size(), ' ')
                                : value;
  }

  static void appendTableRow(std::vector<std::string>& lines,
                             const std::vector<std::string>& row,
                             const std::vector<size_t>& widths)
  {
    std::vector<std::vector<std::string>> cells(widths.size());
    size_t height = 1;
    for (size_t col = 0; col < widths.size(); ++col) {
      cells[col] = wrapLine(col < row.size() ? row[col] : "", widths[col]);
      height = std::max(height, cells[col].size());
    }
    for (size_t line = 0; line < height; ++line) {
      std::string rendered = "  ";
      for (size_t col = 0; col < widths.size(); ++col) {
        if (col != 0) {
          rendered += " | ";
        }
        rendered += pad(line < cells[col].size() ? cells[col][line] : "",
                        widths[col]);
      }
      lines.push_back(std::move(rendered));
    }
  }

  static void emit(const char* stage,
                   const std::vector<std::string>& lines,
                   bool leadingSeparator)
  {
    std::string output;
    if (leadingSeparator) {
      output += '\n';
    }
    for (const std::string& line : lines) {
      output += cat("[fr][", stage, "] ", line, '\n');
    }
    // One stdio call per logical record keeps a table or field block together
    // when multiple checker workers emit transcripts concurrently.
    std::fwrite(output.data(), 1, output.size(), stdout);
  }

  bool enabled_;
};

}  // namespace dpl2::fillerRepair
