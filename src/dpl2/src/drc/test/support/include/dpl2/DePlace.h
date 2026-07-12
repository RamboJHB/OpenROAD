#pragma once

#include "fake_udm.h"

namespace dpl2 {

struct GridX
{
  int v = 0;
  GridX() = default;
  GridX(int value) : v(value) {}
};

struct GridY
{
  int v = 0;
  GridY() = default;
  GridY(int value) : v(value) {}
};

class DePlace
{
};

}  // namespace dpl2
