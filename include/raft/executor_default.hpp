// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file executor_default.hpp
/// @brief `kythira::executor_default` — a small, owning N-thread pool whose
///     `handle()` is directly usable as the argument to
///     `future_default<T>::via(...)`, across all three future backends
///     (Folly, `stdexec`, `boost`), and whose `submit(Func)` runs a
///     fire-and-forget callable on the pool regardless of backend.
///
/// `.via()` itself is NOT unified across backends — Folly takes a
/// `folly::Executor&`, `stdexec` a `stdexec_backend::scheduler_handle&`,
/// `boost` an arbitrary duck-typed `Ex&` (Boost.Thread's own concrete pool
/// types, e.g. `basic_thread_pool`, don't derive from a common executor
/// base) — so there is no single portable executor *type*. What IS
/// portable is the call shape: every backend's `Future<T>::via()` has a
/// reference-taking overload, so `future.via(executor.handle())` compiles
/// unchanged regardless of which backend `handle()` actually returns.
/// `submit(Func)` similarly normalizes each backend's own fire-and-forget
/// dispatch primitive (`folly::Executor::add`, `boost::basic_thread_pool::
/// submit`, `exec::start_detached` composed over the stdexec scheduler)
/// behind one call shape.
///
/// Originally scoped to test code needing a real, bounded thread pool to
/// verify genuine concurrency properties without hardcoding a specific
/// backend's executor type. `submit()` extends that to production
/// fire-and-forget task dispatch (e.g. `tcp_rpc_client`'s per-call RPC
/// dispatch, previously hardcoded to a private `folly::CPUThreadPoolExecutor`
/// regardless of the selected `KYTHIRA_DEFAULT_FUTURE_BACKEND`) — code that
/// already knows its backend and wants that backend's fuller native executor
/// API (not just fire-and-forget submission) should still use that native
/// type directly.

#include "future_default.hpp"

#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>
#include <exec/static_thread_pool.hpp>
#elif defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <boost/thread/executors/basic_thread_pool.hpp>
#else
#include <folly/executors/CPUThreadPoolExecutor.h>
#endif

#include <cstddef>
#include <cstdint>
#include <utility>

namespace kythira {

#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)

class executor_default {
public:
    explicit executor_default(std::size_t thread_count)
        : _pool(static_cast<std::uint32_t>(thread_count)), _handle(_pool.get_scheduler()) {}

    auto handle() -> stdexec_backend::scheduler_handle& { return _handle; }

    template<typename Func> void submit(Func&& func) {
        // schedule()'s sender completes with a kythira::unit value (not
        // void, per scheduler_handle's own contract in future_stdexec.hpp),
        // so the continuation must accept -- and discard -- that parameter.
        ::exec::start_detached(
            _handle.schedule() |
            ::stdexec::then([f = std::forward<Func>(func)](kythira::unit) mutable { f(); }));
    }

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

    template<typename Func> void submit(Func&& func) { _pool.submit(std::forward<Func>(func)); }

private:
    boost::executors::basic_thread_pool _pool;
};

#else

class executor_default {
public:
    explicit executor_default(std::size_t thread_count) : _pool(thread_count) {}

    auto handle() -> folly::Executor& { return _pool; }

    template<typename Func> void submit(Func&& func) { _pool.add(std::forward<Func>(func)); }

private:
    folly::CPUThreadPoolExecutor _pool;
};

#endif

}  // namespace kythira
