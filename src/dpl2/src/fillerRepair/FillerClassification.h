// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

// Filler classification helpers owned by fillerRepair. Some destination
// databases expose incorrect cell-type metadata but preserve the conventional
// Fill... instance/master name. Keep the fallback local to the repair snapshot
// instead of changing shared infrastructure or checker classification.

#pragma once

#include <string_view>

namespace dpl2::fillerRepair::internal {

inline bool hasFillerNamePrefix(std::string_view name)
{
  if (name.size() < 4) {
    return false;
  }
  const auto sameIgnoringAsciiCase = [](char value, char lower) {
    return value == lower
           || value == static_cast<char>(lower - ('a' - 'A'));
  };
  return sameIgnoringAsciiCase(name[0], 'f')
         && sameIgnoringAsciiCase(name[1], 'i')
         && sameIgnoringAsciiCase(name[2], 'l')
         && sameIgnoringAsciiCase(name[3], 'l');
}

// Real UDM revisions do not expose names on exactly the same handle. Probe
// each candidate object without adding a hard dependency: named handles use
// the fallback, while the local test-only UDM (whose IDs have no getName())
// simply returns false.
template <typename Named>
auto objectHasFillerNamePrefixImpl(const Named& object, int)
    -> decltype(std::string_view(object.getName()), bool())
{
  return hasFillerNamePrefix(std::string_view(object.getName()));
}

template <typename Named>
bool objectHasFillerNamePrefixImpl(const Named&, long)
{
  return false;
}

template <typename Named>
bool objectHasFillerNamePrefix(const Named& object)
{
  return objectHasFillerNamePrefixImpl(object, 0);
}

}  // namespace dpl2::fillerRepair::internal
