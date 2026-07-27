#pragma once
#include "../fake_udm.h"

#include <cassert>

// Test-only stand-in for the UDM assertion header included by src/drc/util.h.
// The real macro aborts on a failed invariant; asserting the same condition
// keeps that behavior in local Debug builds, where every fillerRepair test
// runs, so a checker invariant break still fails loudly here.
#ifndef uvAssert
#define uvAssert(cond) assert(cond)
#endif
