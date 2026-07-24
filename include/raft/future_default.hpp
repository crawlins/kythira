#pragma once

/// @file future_default.hpp
/// @brief `kythira::future_default<T>` (plus `promise_default`,
///     `semi_promise_default`, `future_factory_default`,
///     `future_collector_default`) — the future/promise backend selected
///     by the `KYTHIRA_DEFAULT_FUTURE_BACKEND` CMake option (`folly`, the
///     default, `stdexec`, or `boost`).
///
/// Templated core code that is already generic over a future type (the
/// prior `folly-concept-wrappers` conversion) never references these
/// aliases and is completely unaffected by which backend is selected — it
/// accepts whatever `future`-satisfying type its caller instantiates it
/// with. These aliases exist only for the remaining non-templated call
/// sites that want "the project's chosen backend" without spelling out
/// which one (Requirement 11.1-11.3, 11.5 in
/// `.kiro/specs/stdexec-future-backend/`; Requirement 8.1-8.6 in
/// `.kiro/specs/boost-future-backend/`). Test code in particular SHALL use
/// these instead of hardcoding the Folly-specific `kythira::Future`/
/// `Promise`/`FutureFactory`/`FutureCollector` (or the Folly-only
/// convenience surface — `.then()`/`.onError()`/a direct-value `Future`
/// constructor/the free function `kythira::wait_for_all` — none of which
/// exist on the `stdexec`/`boost` backends), unless the test is itself
/// explicitly about Folly-specific or cross-backend behavior. Switching
/// the CMake option requires no change to templated core code, and every
/// backend keeps its symbols in its own namespace (`kythira::` for Folly,
/// `kythira::stdexec_backend::` for stdexec, `kythira::boost_backend::`
/// for boost) so nothing is silently mixed (see also
/// `tests/backend_non_interference_compile_fail_test.cpp`).

#include "future.hpp"

#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
#include "future_stdexec.hpp"
#elif defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include "future_boost.hpp"
#endif

namespace kythira {

#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
template<typename T> using future_default = stdexec_backend::Future<T>;
template<typename T> using promise_default = stdexec_backend::Promise<T>;
template<typename T> using semi_promise_default = stdexec_backend::SemiPromise<T>;
template<typename T> using try_default = stdexec_backend::Try<T>;
using future_factory_default = stdexec_backend::FutureFactory;
using future_collector_default = stdexec_backend::FutureCollector;
#elif defined(KYTHIRA_FUTURE_BACKEND_BOOST)
template<typename T> using future_default = boost_backend::Future<T>;
template<typename T> using promise_default = boost_backend::Promise<T>;
template<typename T> using semi_promise_default = boost_backend::SemiPromise<T>;
template<typename T> using try_default = boost_backend::Try<T>;
using future_factory_default = boost_backend::FutureFactory;
using future_collector_default = boost_backend::FutureCollector;
#else
template<typename T> using future_default = Future<T>;
template<typename T> using promise_default = Promise<T>;
template<typename T> using semi_promise_default = SemiPromise<T>;
template<typename T> using try_default = Try<T>;
using future_factory_default = FutureFactory;
using future_collector_default = FutureCollector;
#endif

}  // namespace kythira
