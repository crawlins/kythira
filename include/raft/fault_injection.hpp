#pragma once

// Fault injection gate.  In normal builds (FIU_ENABLE not defined) every
// fiu_do_on / fiu_fail call compiles to a no-op with zero runtime cost.
// Chaos test executables are built with -DFIU_ENABLE and linked against libfiu.

#ifdef FIU_ENABLE
#include <fiu-local.h>
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — intentional no-op stubs
#define fiu_do_on(name, action) ((void)0)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define fiu_fail(name) (0)
#endif
