#pragma once

/// @file future_stdexec.hpp
/// @brief A second, `stdexec`-backed implementation of the `kythira`
///     future/promise/executor family, alongside the default Folly-backed
///     one in `include/raft/future.hpp`.
///
/// Scope (see `.kiro/specs/stdexec-future-backend/` for the full design):
/// no existing production call site is converted from Folly to `stdexec` by
/// this feature; the Folly dependency is not removed or made optional-only;
/// GPU execution (`nvexec`) and other `stdexec` extensions beyond CPU
/// scheduling are out of scope. This header is compiled only when
/// `KYTHIRA_HAS_STDEXEC` is defined (root `CMakeLists.txt`, when
/// `find_package(stdexec CONFIG QUIET)` succeeds); consumers that don't
/// need this backend never pay for it, and the file is a harmless no-op
/// otherwise.
///
/// Everything here lives in `kythira::stdexec_backend`, a distinct
/// namespace from the unqualified `kythira::Future`/`Promise`/`Try` the
/// Folly backend uses — the two backends satisfy the same concepts
/// (`include/concepts/future.hpp`) but are never silently interchangeable
/// (Requirement 11.1/11.4).

#ifdef KYTHIRA_HAS_STDEXEC

#include "../concepts/future.hpp"

#include <stdexec/execution.hpp>
#include <exec/any_sender_of.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace kythira::stdexec_backend {

namespace ex = ::stdexec;

// Forward declarations
template<typename T> class Try;
template<typename T> class SemiPromise;
template<typename T> class Promise;
template<typename T> class Future;
class FutureFactory;
class FutureCollector;
class scheduler_executor_shim;

/// @brief Thrown when a `stdexec` sender completes with the "stopped"
///     (cancellation) signal. `stdexec` has a first-class cancellation
///     channel with no direct Folly equivalent; the concept layer only
///     knows about value/exception, so "stopped" is mapped onto the normal
///     `std::exception_ptr` error channel via this type rather than
///     silently discarded. Code that wants `stdexec`'s richer cancellation
///     semantics should use `stdexec_backend` types with native `stdexec`
///     algorithms directly, bypassing the concept-constrained wrappers.
class operation_cancelled : public std::exception {
public:
    [[nodiscard]] auto what() const noexcept -> const char* override {
        return "kythira::stdexec_backend: operation was cancelled (stdexec 'stopped' signal)";
    }
};

// ── Completion-signature / type-erasure plumbing ───────────────────────────
//
// A bare stdexec sender (e.g. stdexec::just_error(ex)) may have no
// set_value_t(...) completion signature in its own type at all — sync_wait
// then rejects it at compile time, since it requires exactly one successful
// completion signature (confirmed during this spec's Phase 0 spike; see
// spike-notes.md). Future<T> therefore stores a *type-erased* sender that
// explicitly declares all three signatures regardless of which concrete
// sender produced it, via exec::any_sender<exec::any_receiver<...>> — this
// is also what makes Future<T> a concrete class type (comparable to
// folly::Future<T>) rather than an unnameable template-parameterized-on-
// every-continuation raw sender type.

template<typename T>
using completion_sigs =
    ex::completion_signatures<ex::set_value_t(T), ex::set_error_t(std::exception_ptr),
                              ex::set_stopped_t()>;

template<typename T> using any_receiver_t = exec::any_receiver<completion_sigs<T>>;

template<typename T> using any_sender_t = exec::any_sender<any_receiver_t<T>>;

// ── Try<T> (Requirement 5) ──────────────────────────────────────────────────
//
// Built directly as a value/std::exception_ptr tagged union, independent of
// any sender — stdexec has no Try-equivalent type to wrap, unlike the Folly
// backend which wraps folly::Try.

template<typename T> class Try {
public:
    using value_type = T;

    Try()
    requires std::is_default_constructible_v<T>
        : _state(T{}) {}
    explicit Try(T value) : _state(std::move(value)) {}
    explicit Try(std::exception_ptr ex) : _state(std::move(ex)) {
        if (!std::get<std::exception_ptr>(_state)) {
            throw std::invalid_argument("Try<T>: exception_ptr cannot be null");
        }
    }

    [[nodiscard]] auto value() -> T& {
        if (hasException()) {
            std::rethrow_exception(std::get<std::exception_ptr>(_state));
        }
        return std::get<T>(_state);
    }

    [[nodiscard]] auto value() const -> const T& {
        if (hasException()) {
            std::rethrow_exception(std::get<std::exception_ptr>(_state));
        }
        return std::get<T>(_state);
    }

    [[nodiscard]] auto exception() const -> std::exception_ptr {
        if (hasException()) {
            return std::get<std::exception_ptr>(_state);
        }
        return nullptr;
    }

    [[nodiscard]] auto hasValue() const -> bool { return std::holds_alternative<T>(_state); }
    [[nodiscard]] auto hasException() const -> bool {
        return std::holds_alternative<std::exception_ptr>(_state);
    }

private:
    std::variant<T, std::exception_ptr> _state;
};

// Specialization for void — std::variant<void, ...> is ill-formed, so this
// stores kythira::unit internally (matching the Folly backend's own
// Try<void>, which stores folly::Unit internally) while presenting a
// void-shaped external API.
template<> class Try<void> {
public:
    using value_type = void;

    Try() : _state(kythira::unit{}) {}
    explicit Try(kythira::unit) : _state(kythira::unit{}) {}
    explicit Try(std::exception_ptr ex) : _state(std::move(ex)) {
        if (!std::get<std::exception_ptr>(_state)) {
            throw std::invalid_argument("Try<void>: exception_ptr cannot be null");
        }
    }

    auto value() const -> void {
        if (hasException()) {
            std::rethrow_exception(std::get<std::exception_ptr>(_state));
        }
    }

    [[nodiscard]] auto exception() const -> std::exception_ptr {
        if (hasException()) {
            return std::get<std::exception_ptr>(_state);
        }
        return nullptr;
    }

    [[nodiscard]] auto hasValue() const -> bool {
        return std::holds_alternative<kythira::unit>(_state);
    }
    [[nodiscard]] auto hasException() const -> bool {
        return std::holds_alternative<std::exception_ptr>(_state);
    }

private:
    std::variant<kythira::unit, std::exception_ptr> _state;
};

/// @brief Runs `sender` to completion (blocking the calling thread) and
///     captures its outcome in a `Try<T>`, mapping the "stopped" signal to
///     `operation_cancelled` per this file's cancellation policy. Used
///     inside `Future<T>::get()`/`sync_wait`-based code paths and inside
///     `FutureCollector` (Phase 3).
template<typename T, typename Sender> auto into_try(Sender&& sender) -> Try<T> {
    // Erase into any_sender_t<T> (or, for T=void, any_sender_t<unit> — a
    // sender producing kythira::unit, never stdexec's own zero-arg
    // set_value_t() convention; see Future<void>::get(), which always calls
    // into_try<kythira::unit>(...), consistently with Requirement 2.3's
    // "use kythira::unit directly") before sync_wait: a bare
    // just_error(...)/just_stopped() sender has no set_value_t(...)
    // completion signature in its own type at all, and sync_wait requires
    // exactly one. This mirrors the same fix Future<T> already applies for
    // the identical reason (confirmed during this spec's Phase 0 spike;
    // see spike-notes.md) — into_try needed it too.
    using erased_t = std::conditional_t<std::is_void_v<T>, kythira::unit, T>;
    try {
        any_sender_t<erased_t> erased(std::forward<Sender>(sender));
        auto result = ex::sync_wait(std::move(erased));
        if (!result) {
            // Empty optional == the sender completed with set_stopped_t().
            return Try<T>(std::make_exception_ptr(operation_cancelled{}));
        }
        if constexpr (std::is_void_v<T>) {
            return Try<T>(kythira::unit{});
        } else {
            auto& [value] = *result;
            return Try<T>(std::move(value));
        }
    } catch (...) {
        return Try<T>(std::current_exception());
    }
}

static_assert(kythira::try_type<Try<int>, int>,
              "stdexec_backend::Try<int> must satisfy try_type concept");
static_assert(kythira::try_type<Try<void>, void>,
              "stdexec_backend::Try<void> must satisfy try_type concept");
static_assert(kythira::try_type<Try<std::string>, std::string>,
              "stdexec_backend::Try<std::string> must satisfy try_type concept");

namespace detail {
struct test_struct {
    int value;
};
}  // namespace detail

static_assert(kythira::try_type<Try<detail::test_struct>, detail::test_struct>,
              "stdexec_backend::Try<test_struct> must satisfy try_type concept");

// ── single_shot_channel<T> (Requirement 6) ──────────────────────────────────
//
// The core new primitive: bridges Folly's push-style Promise::setValue
// (fulfilled from arbitrary external code, at an arbitrary later time,
// often from a different thread than the one that created the promise —
// e.g. a network I/O completion callback) onto stdexec's pull-style
// senders, which have no first-class "let external code complete this
// later" primitive of their own. Confirmed during this spec's Phase 0
// spike that no existing exec/ primitive covers this (see
// spike-notes.md) — hand-rolled here as design.md anticipated.
//
// State machine (see design.md's Correctness Properties / Property 7):
//
//         set_value/set_error (any thread)
//              |
//   [empty] ---+--- start() before fulfillment --> [waiting] (receiver stored)
//      |                                                |
//      | start() after fulfillment                      | set_value/set_error
//      v                                                v
//   complete synchronously                    complete via stored receiver
//
// Both transitions into "complete" happen under a single atomic
// compare-exchange, so exactly-once completion holds regardless of which
// side — fulfillment or connection — arrives second.

namespace detail {

enum class channel_state : int {
    empty = 0,
    waiting = 1,
    complete = 2
};

template<typename T> struct channel_shared_state {
    std::atomic<channel_state> state{channel_state::empty};
    // Populated exactly once, under the CAS that moves state into
    // `complete`; std::monostate is a placeholder never observed once
    // `state == complete` (T could itself be default-constructible and
    // ambiguous with an unfulfilled state otherwise).
    std::variant<std::monostate, T, std::exception_ptr> result;
    // Type-erased "invoke the waiting receiver's completion" callback,
    // installed by start() when it wins the empty->waiting transition;
    // invoked by whichever fulfill() call wins the waiting->complete
    // transition. Guarded by resume_mu, not the atomic alone, since
    // installing/reading a std::function isn't itself atomic.
    std::function<void()> resume;
    std::mutex resume_mu;
};

template<typename T> using shared_state_ptr = std::shared_ptr<channel_shared_state<T>>;

// Non-throwing fulfillment used by callers (e.g. a promise's destructor)
// that must not let an exception escape; returns false on a losing race
// (already fulfilled) instead of throwing. `value` is exactly what
// channel_shared_state<T>::result should hold — T or std::exception_ptr.
template<typename T, typename Value>
auto try_fulfill(const shared_state_ptr<T>& state, Value&& value) noexcept -> bool {
    auto expected = channel_state::empty;
    if (state->state.compare_exchange_strong(expected, channel_state::complete,
                                             std::memory_order_acq_rel)) {
        state->result = std::forward<Value>(value);
        return true;
    }
    if (expected == channel_state::waiting) {
        std::function<void()> resume_fn;
        {
            std::lock_guard<std::mutex> lock(state->resume_mu);
            auto expected_waiting = channel_state::waiting;
            if (state->state.compare_exchange_strong(expected_waiting, channel_state::complete,
                                                     std::memory_order_acq_rel)) {
                state->result = std::forward<Value>(value);
                resume_fn = std::move(state->resume);
            }
        }
        if (resume_fn) {
            resume_fn();
            return true;
        }
    }
    return false;
}

template<typename T, typename Receiver> class single_shot_operation_state {
public:
    single_shot_operation_state(shared_state_ptr<T> state, Receiver receiver)
        : _state(std::move(state)), _receiver(std::move(receiver)) {}

    single_shot_operation_state(const single_shot_operation_state&) = delete;
    single_shot_operation_state(single_shot_operation_state&&) = delete;
    auto operator=(const single_shot_operation_state&) -> single_shot_operation_state& = delete;
    auto operator=(single_shot_operation_state&&) -> single_shot_operation_state& = delete;
    ~single_shot_operation_state() = default;

    auto start() & noexcept -> void {
        if (_state->state.load(std::memory_order_acquire) == channel_state::complete) {
            complete_now();
            return;
        }
        bool registered = false;
        {
            std::lock_guard<std::mutex> lock(_state->resume_mu);
            auto expected = channel_state::empty;
            if (_state->state.compare_exchange_strong(expected, channel_state::waiting,
                                                      std::memory_order_acq_rel)) {
                _state->resume = [this] { complete_now(); };
                registered = true;
            }
        }
        if (!registered) {
            // Lost the race: fulfill() already flipped empty -> complete
            // between our load() above and acquiring resume_mu.
            complete_now();
        }
    }

private:
    auto complete_now() noexcept -> void {
        auto& result = _state->result;
        if (std::holds_alternative<std::exception_ptr>(result)) {
            ex::set_error(std::move(_receiver), std::get<std::exception_ptr>(std::move(result)));
        } else if constexpr (std::is_void_v<T>) {
            ex::set_value(std::move(_receiver));
        } else {
            ex::set_value(std::move(_receiver), std::get<T>(std::move(result)));
        }
    }

    shared_state_ptr<T> _state;
    Receiver _receiver;
};

template<typename T> class single_shot_sender {
public:
    using sender_concept = ex::sender_t;
    using completion_signatures = kythira::stdexec_backend::completion_sigs<T>;

    explicit single_shot_sender(shared_state_ptr<T> state) : _state(std::move(state)) {}

    template<typename Receiver>
    auto connect(Receiver receiver) const -> single_shot_operation_state<T, Receiver> {
        return single_shot_operation_state<T, Receiver>(_state, std::move(receiver));
    }

private:
    shared_state_ptr<T> _state;
};

}  // namespace detail

template<typename T> class single_shot_channel {
public:
    single_shot_channel() : _state(std::make_shared<detail::channel_shared_state<T>>()) {}

    // Called from arbitrary external code, on any thread — the entire
    // reason this primitive exists (Requirement 6.2). Throws on a losing
    // double-fulfillment race (Requirement 6.3), matching folly::Promise's
    // documented double-set behavior.
    auto set_value(T value) -> void {
        if (!detail::try_fulfill(_state, std::move(value))) {
            throw std::logic_error("single_shot_channel: already fulfilled");
        }
    }

    auto set_error(std::exception_ptr ex) -> void {
        if (!ex) {
            throw std::invalid_argument("single_shot_channel: exception_ptr cannot be null");
        }
        if (!detail::try_fulfill(_state, std::move(ex))) {
            throw std::logic_error("single_shot_channel: already fulfilled");
        }
    }

    // Non-throwing variant for callers that must not let an exception
    // escape (e.g. a promise destructor implementing broken-promise
    // semantics, Requirement 6.5) — returns false on a losing race
    // instead of throwing.
    auto try_set_error(std::exception_ptr ex) noexcept -> bool {
        return detail::try_fulfill(_state, std::move(ex));
    }

    [[nodiscard]] auto is_fulfilled() const -> bool {
        return _state->state.load(std::memory_order_acquire) == detail::channel_state::complete;
    }

    [[nodiscard]] auto get_sender() const -> detail::single_shot_sender<T> {
        return detail::single_shot_sender<T>(_state);
    }

private:
    detail::shared_state_ptr<T> _state;
};

// ── SemiPromise<T> / Promise<T> (Requirement 6.6) ───────────────────────────
//
// Thin wrappers over single_shot_channel<T>. Move-only, matching the Folly
// backend's SemiPromise/Promise (folly::Promise is move-only); the
// destructor implements broken-promise semantics (Requirement 6.5): if this
// promise is destroyed without ever being fulfilled, it completes the
// channel with an error rather than leaving a connected-and-started
// receiver waiting forever. Uses single_shot_channel::try_set_error (the
// non-throwing variant) specifically so a losing race against a concurrent,
// legitimate fulfillment can never make a destructor throw.

class broken_promise : public std::exception {
public:
    [[nodiscard]] auto what() const noexcept -> const char* override {
        return "kythira::stdexec_backend: promise destroyed before being fulfilled";
    }
};

template<typename T> class SemiPromise {
public:
    using value_type = T;

    SemiPromise() = default;

    SemiPromise(SemiPromise&& other) noexcept
        : _channel(std::move(other._channel)), _active(std::exchange(other._active, false)) {}

    auto operator=(SemiPromise&& other) noexcept -> SemiPromise& {
        if (this != &other) {
            maybe_break_promise();
            _channel = std::move(other._channel);
            _active = std::exchange(other._active, false);
        }
        return *this;
    }

    SemiPromise(const SemiPromise&) = delete;
    auto operator=(const SemiPromise&) -> SemiPromise& = delete;

    ~SemiPromise() { maybe_break_promise(); }

    template<typename U = T>
    auto setValue(U&& value) -> void
    requires(!std::is_void_v<T>)
    {
        _channel.set_value(std::forward<U>(value));
    }

    auto setException(std::exception_ptr ex) -> void {
        if (!ex) {
            throw std::invalid_argument("SemiPromise: exception_ptr cannot be null");
        }
        _channel.set_error(std::move(ex));
    }

    [[nodiscard]] auto isFulfilled() const -> bool { return _channel.is_fulfilled(); }

    // Not part of any concept — used internally by Promise<T>::getFuture()/
    // getSemiFuture() to share the same underlying channel.
    [[nodiscard]] auto get_channel() const -> single_shot_channel<T> { return _channel; }

private:
    auto maybe_break_promise() noexcept -> void {
        if (_active && !_channel.is_fulfilled()) {
            _channel.try_set_error(std::make_exception_ptr(broken_promise{}));
        }
    }

    single_shot_channel<T> _channel;
    bool _active = true;
};

// Specialization for void
template<> class SemiPromise<void> {
public:
    using value_type = void;

    SemiPromise() = default;

    SemiPromise(SemiPromise&& other) noexcept
        : _channel(std::move(other._channel)), _active(std::exchange(other._active, false)) {}

    auto operator=(SemiPromise&& other) noexcept -> SemiPromise& {
        if (this != &other) {
            maybe_break_promise();
            _channel = std::move(other._channel);
            _active = std::exchange(other._active, false);
        }
        return *this;
    }

    SemiPromise(const SemiPromise&) = delete;
    auto operator=(const SemiPromise&) -> SemiPromise& = delete;

    ~SemiPromise() { maybe_break_promise(); }

    auto setValue(kythira::unit) -> void { _channel.set_value(kythira::unit{}); }
    auto setValue() -> void { _channel.set_value(kythira::unit{}); }

    auto setException(std::exception_ptr ex) -> void {
        if (!ex) {
            throw std::invalid_argument("SemiPromise<void>: exception_ptr cannot be null");
        }
        _channel.set_error(std::move(ex));
    }

    [[nodiscard]] auto isFulfilled() const -> bool { return _channel.is_fulfilled(); }

    [[nodiscard]] auto get_channel() const -> single_shot_channel<kythira::unit> {
        return _channel;
    }

private:
    auto maybe_break_promise() noexcept -> void {
        if (_active && !_channel.is_fulfilled()) {
            _channel.try_set_error(std::make_exception_ptr(broken_promise{}));
        }
    }

    single_shot_channel<kythira::unit> _channel;
    bool _active = true;
};

static_assert(kythira::semi_promise<SemiPromise<int>, int>,
              "stdexec_backend::SemiPromise<int> must satisfy semi_promise concept");
static_assert(kythira::semi_promise<SemiPromise<void>, void>,
              "stdexec_backend::SemiPromise<void> must satisfy semi_promise concept");

template<typename T> class Promise : public SemiPromise<T> {
public:
    Promise() = default;
    Promise(Promise&&) = default;
    auto operator=(Promise&&) -> Promise& = default;
    Promise(const Promise&) = delete;
    auto operator=(const Promise&) -> Promise& = delete;

    [[nodiscard]] auto getFuture() -> Future<T> { return Future<T>(this->get_channel()); }
    [[nodiscard]] auto getSemiFuture() -> Future<T> { return Future<T>(this->get_channel()); }
};

// ── Future<T> (Requirement 7) ───────────────────────────────────────────────
//
// Wraps a type-erased any_sender_t<T> (see this file's completion-signature
// plumbing, above) rather than a bare concrete sender — the type-erasure
// boundary is what makes a factory-created sender (e.g. stdexec::just_error,
// which alone has no set_value_t(...) completion signature) sync_wait-able,
// and what makes Future<T> a concrete class type comparable to
// folly::Future<T>, not an unnameable template-parameterized-on-every-
// continuation raw sender type. Confirmed necessary during this spec's
// Phase 0 spike (spike-notes.md) — not part of the original design sketch.
//
// This class provides get()/isReady()/wait(timeout) only (Requirement 7.1-
// 7.3) — no continuations yet (thenValue/thenError/via/delay/within/ensure
// are Phase 3, layered on top once these core primitives are checkpointed).

template<typename T> class Future {
public:
    using value_type = T;

    explicit Future(any_sender_t<T> sender)
        : _sender(std::move(sender)), _is_ready_fn([] { return true; }) {}

    // Constructed from a channel (the common case: Promise<T>::getFuture()),
    // so isReady()/wait(timeout) can poll the channel's fulfillment state
    // without consuming _sender (which get() alone does, via &&).
    explicit Future(single_shot_channel<T> channel)
        : _sender(channel.get_sender()),
          _is_ready_fn([channel] { return channel.is_fulfilled(); }) {}

    Future(Future&&) = default;
    auto operator=(Future&&) -> Future& = default;
    Future(const Future&) = delete;
    auto operator=(const Future&) -> Future& = delete;

    auto get() && -> T { return into_try<T>(std::move(_sender)).value(); }

    [[nodiscard]] auto isReady() const -> bool { return _is_ready_fn(); }

    // Polls is_ready_fn() rather than connecting/starting an operation
    // state, so there is nothing to leak regardless of whether the timeout
    // elapses first (Requirement 7.3) — the underlying channel state (if
    // any) stays alive via its own shared_ptr refcounting independent of
    // this call's outcome, and _sender remains untouched for a subsequent
    // get().
    auto wait(std::chrono::milliseconds timeout) -> bool {
        constexpr auto poll_interval = std::chrono::milliseconds(1);
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!isReady()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::min(poll_interval, timeout));
        }
        return true;
    }

private:
    any_sender_t<T> _sender;
    std::function<bool()> _is_ready_fn;
};

// Specialization for void — mirrors Try<void>/SemiPromise<void>: an
// any_sender_t<kythira::unit> internally (completion_sigs<void> is
// ill-formed — a completion signature can't declare a void value
// parameter), a void-shaped external API.
template<> class Future<void> {
public:
    using value_type = void;

    explicit Future(any_sender_t<kythira::unit> sender)
        : _sender(std::move(sender)), _is_ready_fn([] { return true; }) {}

    explicit Future(single_shot_channel<kythira::unit> channel)
        : _sender(channel.get_sender()),
          _is_ready_fn([channel] { return channel.is_fulfilled(); }) {}

    Future(Future&&) = default;
    auto operator=(Future&&) -> Future& = default;
    Future(const Future&) = delete;
    auto operator=(const Future&) -> Future& = delete;

    auto get() && -> void { std::ignore = into_try<kythira::unit>(std::move(_sender)).value(); }

    [[nodiscard]] auto isReady() const -> bool { return _is_ready_fn(); }

    auto wait(std::chrono::milliseconds timeout) -> bool {
        constexpr auto poll_interval = std::chrono::milliseconds(1);
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!isReady()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::min(poll_interval, timeout));
        }
        return true;
    }

private:
    any_sender_t<kythira::unit> _sender;
    std::function<bool()> _is_ready_fn;
};

static_assert(kythira::future<Future<int>, int>,
              "stdexec_backend::Future<int> must satisfy future concept");
static_assert(kythira::future<Future<void>, void>,
              "stdexec_backend::Future<void> must satisfy future concept");
static_assert(kythira::future<Future<kythira::unit>, kythira::unit>,
              "stdexec_backend::Future<kythira::unit> must satisfy future concept");
static_assert(kythira::future<Future<std::string>, std::string>,
              "stdexec_backend::Future<std::string> must satisfy future concept");
static_assert(kythira::promise<Promise<int>, int>,
              "stdexec_backend::Promise<int> must satisfy promise concept");
static_assert(kythira::promise<Promise<void>, void>,
              "stdexec_backend::Promise<void> must satisfy promise concept");

}  // namespace kythira::stdexec_backend

#endif  // KYTHIRA_HAS_STDEXEC
