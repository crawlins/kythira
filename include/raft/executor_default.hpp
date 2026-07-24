#pragma once

/// @file executor_default.hpp
/// @brief `kythira::executor_default` — a small, owning N-thread pool whose
///     `handle()` is directly usable as the argument to
///     `future_default<T>::via(...)`, across all three future backends
///     (Folly, `stdexec`, `boost`).
///
/// `.via()` itself is NOT unified across backends — Folly takes a
/// `folly::Executor&`, `stdexec` a `stdexec_backend::scheduler_handle&`,
/// `boost` an arbitrary duck-typed `Ex&` (Boost.Thread's own concrete pool
/// types, e.g. `basic_thread_pool`, don't derive from a common executor
/// base) — so there is no single portable executor *type*. What IS
/// portable is the call shape: every backend's `Future<T>::via()` has a
/// reference-taking overload, so `future.via(executor.handle())` compiles
/// unchanged regardless of which backend `handle()` actually returns.
///
/// Exists for test code that needs a real, bounded thread pool to verify
/// genuine concurrency properties (e.g. "retry backoff doesn't starve
/// other work queued on the same pool") without hardcoding a specific
/// backend's executor type and thereby becoming un-portable. Production
/// code that already knows its backend should keep using that backend's
/// native executor type directly; this exists for the `future_default`-only
/// test surface.

#include "future_default.hpp"

#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
#include <exec/static_thread_pool.hpp>
#elif defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <boost/thread/executors/basic_thread_pool.hpp>
#else
#include <folly/executors/CPUThreadPoolExecutor.h>
#endif

#include <cstddef>
#include <cstdint>

namespace kythira {

#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)

class executor_default {
public:
    explicit executor_default(std::size_t thread_count)
        : _pool(static_cast<std::uint32_t>(thread_count)), _handle(_pool.get_scheduler()) {}

    auto handle() -> stdexec_backend::scheduler_handle& { return _handle; }

private:
    exec::static_thread_pool _pool;
    stdexec_backend::scheduler_handle _handle;
};

#elif defined(KYTHIRA_FUTURE_BACKEND_BOOST)

class executor_default {
public:
    explicit executor_default(std::size_t thread_count)
        : _pool(static_cast<unsigned>(thread_count)) {}

    auto handle() -> boost::executors::basic_thread_pool& { return _pool; }

private:
    boost::executors::basic_thread_pool _pool;
};

#else

class executor_default {
public:
    explicit executor_default(std::size_t thread_count) : _pool(thread_count) {}

    auto handle() -> folly::Executor& { return _pool; }

private:
    folly::CPUThreadPoolExecutor _pool;
};

#endif

}  // namespace kythira
