// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file striped_executor.hpp
/// @brief A fixed-size pool of serial queues: every key is bound to one stripe,
///        and one stripe is one thread, so work for a key is never entered
///        concurrently.
///
/// This is MicroRaft's `RaftNodeExecutor` discipline — "enforces serial task
/// execution using the Actor model … maintaining happens-before relationships
/// for deterministic protocol execution" — applied to `multi_raft`'s groups
/// (`.kiro/specs/multi-raft/` design §4.1).
///
/// **The pool size is a machine property, never a shard-count property.** One
/// thread per group is the obvious implementation and the one that stops
/// working at a few hundred groups; a thousand groups on a four-thread pool is
/// the case this exists to serve. `node<Types>`'s own `_mutex` then becomes
/// uncontended rather than being relied on for correctness under fan-out.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace kythira {

/// @brief A fixed-size pool of single-threaded serial queues.
///
/// Keys map to stripes by `hash % stripe_count`, so two keys can share a
/// stripe (and therefore serialise against each other) but one key never
/// spans two. Sharing is the price of a bounded thread count and is
/// deliberate: correctness depends only on *per-key* serialisation.
class striped_serial_executor {
public:
    /// @brief The default pool size: `min(hardware_concurrency, 8)`, at least 1.
    ///
    /// Capped because the work on these threads is short and latency-bound
    /// rather than throughput-bound — past a handful of stripes the extra
    /// threads mostly add context switches.
    [[nodiscard]] static auto default_stripe_count() -> std::size_t {
        const auto hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
        const auto capped = hw == 0 ? std::size_t{1} : (hw < 8 ? hw : std::size_t{8});
        return capped == 0 ? std::size_t{1} : capped;
    }

    explicit striped_serial_executor(std::size_t stripe_count = default_stripe_count())
        : _stripes(stripe_count == 0 ? 1 : stripe_count) {
        _threads.reserve(_stripes.size());
        for (std::size_t i = 0; i < _stripes.size(); ++i) {
            _threads.emplace_back([this, i] { run(i); });
        }
    }

    striped_serial_executor(const striped_serial_executor&) = delete;
    auto operator=(const striped_serial_executor&) -> striped_serial_executor& = delete;
    striped_serial_executor(striped_serial_executor&&) = delete;
    auto operator=(striped_serial_executor&&) -> striped_serial_executor& = delete;

    ~striped_serial_executor() { stop(); }

    [[nodiscard]] auto stripe_count() const -> std::size_t { return _stripes.size(); }

    /// @brief Which stripe a key with hash `h` runs on.
    [[nodiscard]] auto stripe_for(std::size_t h) const -> std::size_t {
        return h % _stripes.size();
    }

    /// @brief Queue `task` on `stripe`. Returns `false` if the pool is stopping.
    auto post(std::size_t stripe, std::function<void()> task) -> bool {
        auto& s = _stripes.at(stripe);
        {
            std::lock_guard lock(s._mutex);
            if (_stopping.load(std::memory_order_acquire)) {
                return false;
            }
            s._queue.push_back(std::move(task));
        }
        s._not_empty.notify_one();
        return true;
    }

    /// @brief Run `task` on `stripe` and block until it has finished.
    ///
    /// Because a stripe is FIFO, this also *drains* everything queued ahead of
    /// it — which is what group teardown needs: destroy the group only after
    /// its own queue is empty (design §4.1).
    ///
    /// Calling this from inside the same stripe would deadlock a
    /// single-threaded queue against itself, so it is diagnosed rather than
    /// hung: a deadlock here would look exactly like a stuck node.
    ///
    /// @throws std::runtime_error if called from the target stripe, or if the
    ///         pool is stopping.
    auto post_and_wait(std::size_t stripe, std::function<void()> task) -> void {
        if (current_stripe() == stripe) {
            throw std::runtime_error(
                "striped_serial_executor: post_and_wait from inside its own stripe would "
                "deadlock");
        }

        std::mutex done_mutex;
        std::condition_variable done_cv;
        bool done = false;
        std::exception_ptr failure;

        const bool queued = post(stripe, [&] {
            try {
                task();
            } catch (...) {
                failure = std::current_exception();
            }
            {
                std::lock_guard lock(done_mutex);
                done = true;
            }
            done_cv.notify_one();
        });
        if (!queued) {
            throw std::runtime_error("striped_serial_executor: pool is stopping");
        }

        std::unique_lock lock(done_mutex);
        done_cv.wait(lock, [&] { return done; });
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    /// @brief Block until every stripe's queue is empty and idle.
    auto drain_all() -> void {
        for (std::size_t i = 0; i < _stripes.size(); ++i) {
            if (current_stripe() == i) {
                continue;  // Already inside it; everything ahead of us has run.
            }
            post_and_wait(i, [] {});
        }
    }

    /// @brief Total tasks queued but not yet started, across all stripes.
    [[nodiscard]] auto pending() const -> std::size_t {
        std::size_t total = 0;
        for (const auto& s : _stripes) {
            std::lock_guard lock(s._mutex);
            total += s._queue.size();
        }
        return total;
    }

    /// @brief Stop accepting work, let queued tasks finish, and join every thread.
    ///
    /// Queued tasks are run rather than discarded: a discarded teardown task
    /// leaks whatever it was going to release, and the queue is short by
    /// construction because the tick posts and waits.
    auto stop() -> void {
        if (_stopping.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        for (auto& s : _stripes) {
            {
                std::lock_guard lock(s._mutex);
                s._shutdown = true;
            }
            s._not_empty.notify_all();
        }
        for (auto& t : _threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        _threads.clear();
    }

    [[nodiscard]] auto stopped() const -> bool { return _stopping.load(std::memory_order_acquire); }

    /// @brief The stripe of the calling thread, or `npos` if it is not a pool thread.
    ///
    /// Exposed so callers can assert they are on the stripe they think they
    /// are on — the invariant this class exists to provide is only useful if
    /// violations are detectable.
    [[nodiscard]] static auto current_stripe() -> std::size_t { return _current_stripe; }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    struct stripe_state {
        mutable std::mutex _mutex;
        std::condition_variable _not_empty;
        std::deque<std::function<void()>> _queue;
        bool _shutdown{false};
    };

    auto run(std::size_t index) -> void {
        _current_stripe = index;
        auto& s = _stripes[index];
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(s._mutex);
                s._not_empty.wait(lock, [&] { return s._shutdown || !s._queue.empty(); });
                if (s._queue.empty()) {
                    // Shutdown with nothing left to run.
                    return;
                }
                task = std::move(s._queue.front());
                s._queue.pop_front();
            }
            // Run outside the lock: a task that posts to its own stripe (a
            // continuation scheduling a follow-up) must not deadlock, and a
            // long task must not block `pending()`.
            try {
                task();
            } catch (...) {
                // A task that throws must not take the stripe's thread with it —
                // that would silently stop every group sharing the stripe. The
                // exception is swallowed here because there is no caller to
                // return it to; tasks that need failures reported wrap their own
                // body (see `post_and_wait`).
            }
        }
    }

    std::deque<stripe_state> _stripes;
    std::vector<std::thread> _threads;
    std::atomic<bool> _stopping{false};

    static thread_local std::size_t _current_stripe;
};

inline thread_local std::size_t striped_serial_executor::_current_stripe =
    striped_serial_executor::npos;

}  // namespace kythira
