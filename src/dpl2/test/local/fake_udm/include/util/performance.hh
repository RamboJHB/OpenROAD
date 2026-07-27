#pragma once
#include "../fake_udm.h"

// Test-only stand-in for the UDM performance/instrumentation header included
// by drc/ImplantLayerChecker.cpp and drc/ImplantLayerCheckerHelper.cpp.
//
// Those translation units include it but reference nothing from it, so an
// empty header is enough to build them here. It exists only so the checker
// sources compile unmodified against the fake UDM; if the checker later uses a
// real symbol from this header, add a matching stand-in rather than editing
// the checker.
