#pragma once

#include <fake_udm.h>

namespace eUNL {

// The destination DePlace API accepts an optional voltage-area pointer.  The
// fake model stores only the rectangles needed by placement search tests.
class VoltageArea
{
 public:
  const std::vector<eUTL::Rect>& getRects() const { return rects_; }
  std::vector<eUTL::Rect>& getRects() { return rects_; }

 private:
  std::vector<eUTL::Rect> rects_;
};

}  // namespace eUNL
