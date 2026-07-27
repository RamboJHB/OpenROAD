#pragma once

// Test-only stand-in: drc/ImplantLayerChecker.cpp includes the tech rule-check
// header under its physlib/ path while other dpl2 sources include it
// unqualified. Both spellings must name the same declarations, so this simply
// forwards to the existing fake header rather than duplicating it.
#include "../techRuleCheck.hh"
