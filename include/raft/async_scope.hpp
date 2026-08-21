// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file async_scope.hpp
/// @brief Tracks in-flight async continuations so shutdown can *wait* for them
///        rather than merely asking them to stop.
///
/// ## Why this exists
///
/// This codebase guards async callbacks that capture `this` with a shared
/// `std::shared_ptr<std::atomic<bool>>` stop flag: the callback captures it by
/// value and returns early if it is set. That pattern is necessary but not
/// sufficient. It is a *check*, and a check that passes tells you only that the
/// owner was alive a moment ago — nothing stops the owner being destroyed
/// between the check and the next line. The window is narrow, which is why it
/// took a fault-injection suite under a backend that schedules continuations on
/// freshly spawned threads to expose it (see `doc/TODO.md`, and PR #177's
/// AddressSanitizer report).
///
/// `async_scope` closes the window instead of narrowing it. A continuation
/// takes a `ticket` for the duration of its body; `close_and_drain()` refuses
/// new tickets and then blocks until every outstanding one is gone. After it
/// returns, no callback is executing and none can start, so the owner is safe
/// to destroy.
///
/// ## Usage
///
/// @code
/// // In the owner, alongside the existing _stop_flag:
/// std::shared_ptr<kythira::async_scope> _async_scope{
///     std::make_shared<kythira::async_scope>()};
///
/// // In a continuation — capture the scope by value, exactly like the flag:
/// .thenTry([this, scope = _async_scope](auto result) {
///     const auto ticket = scope->enter();
///     if (!ticket) {
///         return;  // shutting down: `this` may already be gone
///     }
///     // `this` is guaranteed alive for as long as `ticket` lives.
/// });
///
/// // In stop(), holding no lock the continuations might need:
/// _async_scope->close_and_drain();
/// @endcode
///
/// ## Two design decisions that matter
///
/// **A ticket covers a continuation's *execution*, not its whole async chain.**
/// If a ticket were held for the lifetime of a pending future, a drain would
/// block until the network responded — or forever, if it never did. Holding it
/// only across the body means a drain waits on work that is already running and
/// therefore about to finish. A continuation scheduled *later* (a retry, say)
/// is not lost track of: it calls `enter()` when it runs, gets `nullopt`
/// because the scope is closed, and safely does nothing.
///
/// **`close_and_drain()` waits without a deadline.** A bounded wait that gave
/// up would return to a caller that then destroys an object a callback is still
/// inside — trading a diagnosable hang for memory corruption, which is the
/// worse failure and the harder one to attribute. It logs through the supplied
/// callback if the wait exceeds `slow_drain_threshold`, so a stuck drain is
/// visible rather than silent.
///
/// ## Deadlock note
///
/// Call `close_and_drain()` with **no lock held that a guarded continuation
/// might acquire**. A continuation blocked on a mutex the draining thread owns
/// can never release its ticket. `node::stop()` satisfies this: it holds
/// `_mutex` nowhere near its drain point.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace kythira {

/// @brief Admission control plus a drain barrier for async continuations.
///
/// Neither copyable nor movable: outstanding tickets refer to it by address,
/// and it is meant to be held by `shared_ptr` so a continuation's captured copy
/// keeps it alive after the owner is gone — which is exactly the situation it
/// exists to make safe.
class async_scope {
public:
    /// @brief Proof of admission. `this` on the owner stays valid while one is
    ///     held, because `close_and_drain()` cannot return until it is dropped.
    ///
    /// Move-only, so it cannot be duplicated into a longer-lived home by
    /// accident.
    class ticket {
    public:
        ticket(const ticket&) = delete;
        auto operator=(const ticket&) -> ticket& = delete;

        ticket(ticket&& other) noexcept : _scope(std::exchange(other._scope, nullptr)) {}
        auto operator=(ticket&& other) noexcept -> ticket& {
            if (this != &other) {
                release();
                _scope = std::exchange(other._scope, nullptr);
            }
            return *this;
        }

        ~ticket() { release(); }

    private:
        friend class async_scope;
        explicit ticket(async_scope* scope) : _scope(scope) {}

        auto release() -> void {
            if (_scope != nullptr) {
                _scope->leave();
                _scope = nullptr;
            }
        }

        async_scope* _scope{nullptr};
    };

    async_scope() = default;
    async_scope(const async_scope&) = delete;
    auto operator=(const async_scope&) -> async_scope& = delete;
    async_scope(async_scope&&) = delete;
    auto operator=(async_scope&&) -> async_scope& = delete;
    ~async_scope() = default;

    /// @brief Admit a continuation, or refuse it because shutdown has begun.
    /// @return A ticket while open; `std::nullopt` once `close_and_drain()` has
    ///     been entered. Callers must treat `nullopt` as "the owner may already
    ///     be destroyed" and return without touching it.
    ///
    /// `[[nodiscard]]` deliberately: discarding the result both drops the
    /// admission immediately and ignores the refusal, which is precisely the
    /// use-after-free this type exists to prevent.
    [[nodiscard]] auto enter() -> std::optional<ticket> {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (_closed) {
            return std::nullopt;
        }
        ++_in_flight;
        return ticket{this};
    }

    /// @brief Refuse further admissions, then wait for those already inside.
    ///
    /// Idempotent: a second call on an already-drained scope returns at once,
    /// so a `stop()` that runs twice is not an error.
    ///
    /// @param on_slow_drain Invoked once, with the elapsed wait, if draining
    ///     takes longer than @p slow_drain_threshold. Optional so the primitive
    ///     stays free of any logger dependency; `node` passes its own.
    /// @param slow_drain_threshold How long is long enough to be worth saying.
    auto close_and_drain(const std::function<void(std::chrono::milliseconds)>& on_slow_drain = {},
                         std::chrono::milliseconds slow_drain_threshold = std::chrono::milliseconds{
                             1000}) -> void {
        const auto started = std::chrono::steady_clock::now();
        bool reported_slow = false;

        std::unique_lock<std::mutex> lock(_mutex);
        _closed = true;

        while (_in_flight != 0) {
            // Waiting in slices rather than on a plain predicate wait so a drain
            // that is taking too long can say so *while* it is stuck, not only
            // after it finally finishes — a stuck shutdown that reports nothing
            // until it unsticks is indistinguishable from a hang.
            if (_drained.wait_for(lock, slow_drain_threshold, [this] { return _in_flight == 0; })) {
                break;
            }
            if (!reported_slow && on_slow_drain) {
                reported_slow = true;
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                // Released around the callback: it is caller-supplied and may
                // log, and holding this mutex through arbitrary code invites
                // exactly the lock-ordering problem this class warns about.
                lock.unlock();
                on_slow_drain(elapsed);
                lock.lock();
            }
        }
    }

    /// @brief Whether admissions are being refused.
    [[nodiscard]] auto closed() const -> bool {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _closed;
    }

    /// @brief Continuations currently executing under a ticket. Test/diagnostic
    ///     use — it is a sample, and stale the moment it is read.
    [[nodiscard]] auto in_flight() const -> std::size_t {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _in_flight;
    }

private:
    auto leave() -> void {
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            --_in_flight;
            if (_in_flight != 0) {
                return;
            }
        }
        // Notify outside the lock: a waking drainer would otherwise immediately
        // block on the mutex this thread still holds.
        _drained.notify_all();
    }

    mutable std::mutex _mutex;
    std::condition_variable _drained;
    std::size_t _in_flight{0};
    bool _closed{false};
};

}  // namespace kythira
