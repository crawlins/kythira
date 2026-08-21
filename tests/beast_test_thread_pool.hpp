// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file beast_test_thread_pool.hpp
/// @brief RAII helpers for the io_context-worker-thread pattern shared by
///        every beast_* test file (beast_server_test.cpp,
///        beast_integration_test.cpp, beast_ssl_test.cpp,
///        beast_cross_transport_equivalence_test.cpp).
///
/// Every one of those files previously wrote out, by hand, in each test
/// case: spin up N threads calling `ioc.run()`, then at the end of the test
/// explicitly `work_guard.reset(); ioc.stop(); for (auto& t : threads)
/// t.join();` -- and separately, for a one-off worker thread (e.g. the
/// thread that calls the blocking `server.stop()`), a raw `std::thread`
/// joined explicitly at a single call site. Both patterns share the same
/// hazard: any control-flow path that skips the explicit join/reset call
/// (an early return, a path this test didn't anticipate, or -- as observed
/// running this suite under ThreadSanitizer, where a std::thread was still
/// joinable when its enclosing test_method's locals unwound at the closing
/// brace despite no exception ever being thrown, per `catch throw` in gdb --
/// destructs a still-joinable std::thread, which calls std::terminate()
/// unconditionally. Wrapping both patterns in RAII removes the possibility
/// of a skipped join entirely, by construction, regardless of how such a
/// path is reached.
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <thread>
#include <vector>

namespace kythira::testing {

/// Wraps a single std::thread; joins it (if still joinable) in the
/// destructor instead of requiring every caller to remember an explicit
/// `.join()` at the end of the test.
class joining_thread {
public:
    template<typename F> explicit joining_thread(F&& f) : _thread(std::forward<F>(f)) {}

    joining_thread(const joining_thread&) = delete;
    joining_thread& operator=(const joining_thread&) = delete;
    joining_thread(joining_thread&&) = delete;
    joining_thread& operator=(joining_thread&&) = delete;

    ~joining_thread() {
        if (_thread.joinable()) {
            _thread.join();
        }
    }

private:
    std::thread _thread;
};

/// Owns a pool of `ioc.run()` worker threads for a caller-owned
/// `boost::asio::io_context`. Stops the io_context and joins every worker
/// (if still joinable) in the destructor, so a test case no longer needs
/// its own explicit `work_guard.reset(); ioc.stop(); for (auto& t :
/// threads) t.join();` sequence at every exit path.
class io_thread_pool {
public:
    explicit io_thread_pool(boost::asio::io_context& ioc, std::size_t thread_count)
        : _ioc(ioc), _work_guard(boost::asio::make_work_guard(ioc)) {
        _threads.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            _threads.emplace_back([&ioc] { ioc.run(); });
        }
    }

    io_thread_pool(const io_thread_pool&) = delete;
    io_thread_pool& operator=(const io_thread_pool&) = delete;
    io_thread_pool(io_thread_pool&&) = delete;
    io_thread_pool& operator=(io_thread_pool&&) = delete;

    ~io_thread_pool() {
        _work_guard.reset();
        _ioc.stop();
        for (auto& t : _threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

private:
    boost::asio::io_context& _ioc;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work_guard;
    std::vector<std::thread> _threads;
};

}  // namespace kythira::testing
