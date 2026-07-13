#pragma once

/// @file future_default.hpp
/// @brief `kythira::future_default<T>` — the future/promise backend
///     selected by the `KYTHIRA_DEFAULT_FUTURE_BACKEND` CMake option
///     (`folly`, the default, or `stdexec`).
///
/// Templated core code that is already generic over a future type (the
/// prior `folly-concept-wrappers` conversion) never references this alias
/// and is completely unaffected by which backend is selected — it accepts
/// whatever `future`-satisfying type its caller instantiates it with. This
/// alias exists only for the remaining non-templated call sites that want
/// "the project's chosen backend" without spelling out which one
/// (Requirement 11.1-11.3, 11.5). Switching the CMake option requires no
/// change to templated core code (Requirement 11.2), and every backend
/// keeps its symbols in its own namespace (`kythira::` for Folly,
/// `kythira::stdexec_backend::` for stdexec) so nothing is silently
/// mixed (Requirement 11.4 — see also
/// `tests/backend_non_interference_compile_fail_test.cpp`).

#include "future.hpp"

#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
#include "future_stdexec.hpp"
#endif

namespace kythira {

#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
template<typename T> using future_default = stdexec_backend::Future<T>;
#else
template<typename T> using future_default = Future<T>;
#endif

}  // namespace kythira
