#pragma once
#include "../fake_udm.h"

// UDM utility macros (real ones live in the UDM util headers): property
// setter/getter generators used by infrastructure Objects.h/fillerSetting.h.
#ifndef ADD_SETTER_GETTER_PP
#define ADD_SETTER_GETTER_PP(TYPE, NAME, MEMBER) \
  void set##NAME(TYPE value) { MEMBER = value; } \
  TYPE get##NAME() const { return MEMBER; }
#endif
#ifndef ADD_SETTER_GETTER_PTR_PP
#define ADD_SETTER_GETTER_PTR_PP(TYPE, NAME, MEMBER) \
  void set##NAME(TYPE* value) { MEMBER = value; }    \
  TYPE* get##NAME() const { return MEMBER; }
#endif
