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
#include <exec/finally.hpp>
#include <exec/start_detached.hpp>
#include <exec/timed_scheduler.hpp>
#include <exec/timed_thread_scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

namespace detail {
// Detects Future<U> for any U, so thenValue/thenTry/thenError can tell a
// plain-value-returning callback from a Future-returning one and pick the
// automatic-flattening overload accordingly — mirrors
// include/raft/future.hpp's own detail::is_future trait for the Folly
// backend.
template<typename T> struct is_future : std::false_type {};
template<typename T> struct is_future<Future<T>> : std::true_type {};
template<typename T> inline constexpr bool is_future_v = is_future<T>::value;
}  // namespace detail

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

// ── scheduler_handle (Requirement 3, via `via()`'s `void*` overload) ───────
//
// Folly's via(folly::Executor*) takes a raw non-owning pointer because every
// folly::Executor already has stable identity/lifetime managed by its owner.
// stdexec schedulers are arbitrary value types (no common base class), so
// `via()` needs a single concrete type to accept regardless of which
// concrete scheduler the caller wants to run on — this is that type, a
// small hand-rolled type erasure (copyable value semantics, backed by a
// shared_ptr'd virtual interface) satisfying stdexec's `scheduler` concept
// itself so it can be passed straight to `stdexec::continues_on`.
class scheduler_handle {
public:
    template<typename Scheduler>
    requires(!std::is_same_v<std::decay_t<Scheduler>, scheduler_handle>)
    explicit scheduler_handle(Scheduler sched)
        : _impl(std::make_shared<model<std::decay_t<Scheduler>>>(std::move(sched))) {}

    [[nodiscard]] auto schedule() const -> any_sender_t<kythira::unit> { return _impl->schedule(); }

    auto operator==(const scheduler_handle& other) const -> bool { return _impl == other._impl; }

private:
    struct concept_t {
        virtual ~concept_t() = default;
        [[nodiscard]] virtual auto schedule() const -> any_sender_t<kythira::unit> = 0;
    };

    template<typename Scheduler> struct model : concept_t {
        explicit model(Scheduler sched) : _sched(std::move(sched)) {}

        [[nodiscard]] auto schedule() const -> any_sender_t<kythira::unit> override {
            return any_sender_t<kythira::unit>(ex::schedule(_sched) |
                                               ex::then([] { return kythira::unit{}; }));
        }

        Scheduler _sched;
    };

    std::shared_ptr<concept_t> _impl;
};

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
        // Skip the wrap when `sender` is already exactly any_sender_t<erased_t>
        // (the common case: called from Future<T>::get() with its own
        // _sender member, itself already erased at construction time) —
        // constructing a *second* any_sender_t<erased_t> by moving from a
        // first one is a same-type "move" that stdexec's own any_sender
        // implementation does not appear to treat as a true ownership
        // transfer in this configuration; empirically, doing so left both
        // the source (inside the soon-to-be-destroyed Future<T>) and the
        // destination (this function's local `erased`) referencing the
        // same underlying type-erased storage, so both destructors freed
        // it — corrupting the heap silently on the first call and
        // crashing on a later, unrelated allocation. Only genuinely
        // different sender types (e.g. a raw composed sender from
        // FutureCollector) need the wrap at all.
        auto result = [&] {
            if constexpr (std::is_same_v<std::decay_t<Sender>, any_sender_t<erased_t>>) {
                return ex::sync_wait(std::forward<Sender>(sender));
            } else {
                any_sender_t<erased_t> erased(std::forward<Sender>(sender));
                return ex::sync_wait(std::move(erased));
            }
        }();
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

namespace detail {

/// @brief Wraps `sender` so every completion (value, error, or stopped)
///     becomes a single set_value_t() completion that has already invoked
///     `record(Try<T>)` — the shape exec::start_detached requires (it
///     rejects any sender that can complete with set_error_t). Used by
///     FutureCollector's collectAll/collectAny/
///     collectAnyWithoutException/collectN and by Future<T>::within(), all
///     of which race or aggregate a runtime-sized/pairwise set of senders
///     via a shared "first/all writers race to fulfill a channel" pattern
///     rather than stdexec's own when_all/when_any (which only support a
///     compile-time-fixed sender count, and — discovered while implementing
///     within() — don't propagate cancellation correctly across this file's
///     any_sender_t<T> type-erasure boundary, since any_receiver_t<T> is
///     declared with the default empty query-forwarding list and so never
///     forwards get_stop_token through to a type-erased sender's own
///     connect()).
template<typename T, typename Sender, typename Record>
auto spawn_recording(Sender&& sender, Record record) {
    using ErasedT = std::conditional_t<std::is_void_v<T>, kythira::unit, T>;
    return std::forward<Sender>(sender) | ex::then([record](ErasedT value) mutable {
               if constexpr (std::is_void_v<T>) {
                   record(Try<T>(kythira::unit{}));
               } else {
                   record(Try<T>(std::move(value)));
               }
           }) |
           ex::upon_error(
               [record](std::exception_ptr ex_ptr) mutable { record(Try<T>(std::move(ex_ptr))); }) |
           ex::upon_stopped([record]() mutable {
               record(Try<T>(std::make_exception_ptr(operation_cancelled{})));
           });
}

}  // namespace detail

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
//
// Both the empty->complete and waiting->complete transitions happen under
// resume_mu, and single_shot_operation_state::start() (below) takes the
// same lock before ever reading `result` — a plain mutex critical section,
// so the happens-before edge between this write and complete_now()'s read
// is unambiguous. An earlier version CAS'd empty->complete lock-free,
// relying on compare_exchange's failure-order-implies-acquire semantics
// (std::memory_order_acq_rel's failure order is acquire per the standard)
// to synchronize start()'s read of `result` with this write. That's
// well-defined C++, but ThreadSanitizer flagged it as a data race anyway
// under g++-13/libstdc++ (confirmed via an isolated repro during this
// spec's Phase 3 — see spike-notes.md); rather than fight a
// tool/library-specific gap in recognizing that pattern, this always
// takes the lock, which every reader can verify correct without relying
// on that subtlety at all. The lock is uncontended in the overwhelmingly
// common case (fulfillment happens exactly once per channel), so the
// always-lock cost is negligible.
template<typename T, typename Value>
auto try_fulfill(const shared_state_ptr<T>& state, Value&& value) noexcept -> bool {
    std::function<void()> resume_fn;
    bool fulfilled = false;
    {
        std::lock_guard<std::mutex> lock(state->resume_mu);
        auto expected = channel_state::empty;
        if (state->state.compare_exchange_strong(expected, channel_state::complete,
                                                 std::memory_order_relaxed)) {
            state->result = std::forward<Value>(value);
            fulfilled = true;
        } else if (expected == channel_state::waiting) {
            auto expected_waiting = channel_state::waiting;
            if (state->state.compare_exchange_strong(expected_waiting, channel_state::complete,
                                                     std::memory_order_relaxed)) {
                state->result = std::forward<Value>(value);
                resume_fn = std::move(state->resume);
                fulfilled = true;
            }
        }
    }
    // Invoked outside the lock — resume_fn ultimately calls the receiver's
    // set_value/set_error, arbitrary code this file doesn't control, which
    // must not run while holding resume_mu (a nested/re-entrant attempt to
    // take it, e.g. from a chained continuation, would deadlock).
    if (resume_fn) {
        resume_fn();
    }
    return fulfilled;
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

    // No stop-token/cancellation support: this operation state is only
    // ever connected to a receiver produced by this file's any_sender_t<T>
    // erasure (Future<T>'s storage type), and any_receiver_t<T> forwards no
    // queries — so get_stop_token would always observe never_stop_token
    // here regardless. Future<T>::within() (the one place that needs to
    // race this sender against a timeout) works around this by racing two
    // independent writers against a shared single_shot_channel instead of
    // relying on stdexec-level cancellation — see within()'s own comment.
    // Takes resume_mu for the whole empty/complete decision — see
    // try_fulfill's comment on why this doesn't take the lock-free
    // fast-path shortcut its writer side used to take: both sides
    // synchronizing through the same mutex is what makes the
    // happens-before edge into complete_now()'s read of `result`
    // unambiguous (confirmed clean under ThreadSanitizer after this
    // change; the previous compare-exchange-failure-acquire-based version
    // was not, despite being well-defined C++).
    auto start() & noexcept -> void {
        bool complete_now_flag = false;
        {
            std::lock_guard<std::mutex> lock(_state->resume_mu);
            if (_state->state.load(std::memory_order_relaxed) == channel_state::complete) {
                complete_now_flag = true;
            } else {
                auto expected = channel_state::empty;
                if (_state->state.compare_exchange_strong(expected, channel_state::waiting,
                                                          std::memory_order_relaxed)) {
                    _state->resume = [this] { complete_now(); };
                } else {
                    // The only other reachable state here is `complete`:
                    // try_fulfill's empty->complete and waiting->complete
                    // transitions both hold this same lock, so a
                    // concurrent fulfiller cannot be observed mid-transition.
                    complete_now_flag = true;
                }
            }
        }
        if (complete_now_flag) {
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

    // Non-throwing variant of set_value — used by FutureCollector's
    // collectAny/collectAnyWithoutException, where N racing writers
    // legitimately attempt to fulfill the same channel and all but the
    // first are expected to lose, not throw.
    auto try_set_value(T value) noexcept -> bool {
        return detail::try_fulfill(_state, std::move(value));
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

// ── Timed scheduler for delay()/within() (Requirement 9.5) ─────────────────
//
// Confirmed during the Phase 0 spike (spike-notes.md) that
// exec::timed_thread_context/exec::schedule_after are real, usable CPOs in
// the vendored stdexec version — scheduling 10ms into the future completed
// correctly without blocking the calling thread for the delay, so no
// hand-rolled timed single_shot_channel fallback is needed. One background
// thread (the timed_thread_context's own timer thread) is shared by every
// delay()/within() call in the process via this function-local static;
// stdexec's own machinery, not this file, owns that thread's lifetime.
inline auto global_timed_context() -> exec::timed_thread_context& {
    static exec::timed_thread_context context;
    return context;
}

/// @brief Thrown by `Future<T>::within(timeout)` when the timeout elapses
///     before the original future completes. Mirrors folly::FutureTimeout's
///     role for the Folly backend's own `within()`.
class future_timeout : public std::exception {
public:
    [[nodiscard]] auto what() const noexcept -> const char* override {
        return "kythira::stdexec_backend: future timed out (within())";
    }
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

    // Templated so _sender is erased directly from whatever concrete sender
    // the caller has (stdexec::just(...), a then/let_value chain, etc.) in
    // a single conversion — never by move-constructing any_sender_t<T> from
    // an already-erased any_sender_t<T>. That same-type move hits a real
    // bug in this vendored stdexec's exec::any_sender: when the erased
    // sender is small enough to live in any_sender's small-buffer
    // optimization (e.g. stdexec::just(int)), moving one any_sender_t<T>
    // into another corrupts memory under -O3 (confirmed via an isolated
    // repro outside this file — crashes on the 2nd+ occurrence in a
    // process, works at -O0 and for large, heap-allocated payloads that
    // skip the small-buffer path). Every call site in this file that used
    // to pre-wrap its result in any_sender_t<T>(...) before handing it to
    // this constructor was hitting exactly that path; they now pass the
    // concrete sender straight through instead.
    template<typename Sender>
    requires(!std::is_same_v<std::decay_t<Sender>, single_shot_channel<T>>)
    explicit Future(Sender&& sender)
        : _sender(std::forward<Sender>(sender)), _is_ready_fn([] { return true; }) {}

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

    // Not part of any concept — internal accessor used by FutureCollector
    // and by the Future-returning-callback flattening overloads of
    // thenValue/thenTry/thenError below, mirroring how the Folly backend's
    // wrapper exposes get_folly_future() for the same purpose.
    auto extract_sender() && -> any_sender_t<T> { return std::move(_sender); }

    // Chain continuation with value (Requirement 9.1). Non-Future-returning
    // overload; the Future-returning overload below handles automatic
    // flattening, mirroring the Folly backend's dual-overload split.
    template<typename F>
    auto thenValue(F&& func) -> Future<std::invoke_result_t<F, T>>
    requires(!detail::is_future_v<std::invoke_result_t<F, T>>)
    {
        using R = std::invoke_result_t<F, T>;
        if constexpr (std::is_void_v<R>) {
            auto composed =
                std::move(_sender) | ex::then([func = std::forward<F>(func)](T value) mutable {
                    func(std::move(value));
                    return kythira::unit{};
                });
            return Future<void>(std::move(composed));
        } else {
            auto composed =
                std::move(_sender) | ex::then([func = std::forward<F>(func)](T value) mutable {
                    return func(std::move(value));
                });
            return Future<R>(std::move(composed));
        }
    }

    // Future-returning overload (automatic flattening) — stdexec has no
    // built-in flattening equivalent to Folly's, so this composes via
    // let_value, extracting the inner Future's own sender rather than
    // nesting Future<Future<U>>.
    template<typename F>
    auto thenValue(F&& func) -> std::invoke_result_t<F, T>
    requires(detail::is_future_v<std::invoke_result_t<F, T>>)
    {
        using FutureR = std::invoke_result_t<F, T>;
        using InnerT = typename FutureR::value_type;
        using ErasedInnerT = std::conditional_t<std::is_void_v<InnerT>, kythira::unit, InnerT>;
        auto composed =
            std::move(_sender) | ex::let_value([func = std::forward<F>(func)](T value) mutable {
                return std::move(func(std::move(value))).extract_sender();
            });
        return FutureR(std::move(composed));
    }

    // Chain continuation with Try (Requirement 9.1) — func is invoked
    // exactly once regardless of whether the original sender completed
    // with a value or an error, mirroring folly::Future::thenTry.
    template<typename F>
    auto thenTry(F&& func) -> Future<std::invoke_result_t<F, Try<T>>>
    requires(!detail::is_future_v<std::invoke_result_t<F, Try<T>>>)
    {
        using R = std::invoke_result_t<F, Try<T>>;
        auto with_value = std::move(_sender) | ex::let_value([func](T value) mutable {
                              if constexpr (std::is_void_v<R>) {
                                  func(Try<T>(std::move(value)));
                                  return ex::just(kythira::unit{});
                              } else {
                                  return ex::just(func(Try<T>(std::move(value))));
                              }
                          });
        auto composed =
            std::move(with_value) | ex::let_error([func](std::exception_ptr ex_ptr) mutable {
                if constexpr (std::is_void_v<R>) {
                    func(Try<T>(std::move(ex_ptr)));
                    return ex::just(kythira::unit{});
                } else {
                    return ex::just(func(Try<T>(std::move(ex_ptr))));
                }
            });
        using ErasedR = std::conditional_t<std::is_void_v<R>, kythira::unit, R>;
        if constexpr (std::is_void_v<R>) {
            return Future<void>(std::move(composed));
        } else {
            return Future<R>(std::move(composed));
        }
    }

    // Future-returning overload (automatic flattening).
    template<typename F>
    auto thenTry(F&& func) -> std::invoke_result_t<F, Try<T>>
    requires(detail::is_future_v<std::invoke_result_t<F, Try<T>>>)
    {
        using FutureR = std::invoke_result_t<F, Try<T>>;
        using InnerT = typename FutureR::value_type;
        using ErasedInnerT = std::conditional_t<std::is_void_v<InnerT>, kythira::unit, InnerT>;
        auto with_value = std::move(_sender) | ex::let_value([func](T value) mutable {
                              return std::move(func(Try<T>(std::move(value)))).extract_sender();
                          });
        auto composed =
            std::move(with_value) | ex::let_error([func](std::exception_ptr ex_ptr) mutable {
                return std::move(func(Try<T>(std::move(ex_ptr)))).extract_sender();
            });
        return FutureR(std::move(composed));
    }

    // Error handling (Requirement 9.2) — only the error path is
    // transformed; a successful original sender passes its value through
    // unchanged.
    template<typename F>
    auto thenError(F&& func) -> Future<T>
    requires(!detail::is_future_v<std::invoke_result_t<F, std::exception_ptr>>)
    {
        auto composed =
            std::move(_sender) |
            ex::let_error([func = std::forward<F>(func)](std::exception_ptr ex_ptr) mutable {
                if constexpr (std::is_void_v<T>) {
                    func(std::move(ex_ptr));
                    return ex::just(kythira::unit{});
                } else {
                    return ex::just(func(std::move(ex_ptr)));
                }
            });
        return Future<T>(std::move(composed));
    }

    // Future-returning overload (automatic flattening).
    template<typename F>
    auto thenError(F&& func) -> Future<T>
    requires(detail::is_future_v<std::invoke_result_t<F, std::exception_ptr>>)
    {
        auto composed =
            std::move(_sender) |
            ex::let_error([func = std::forward<F>(func)](std::exception_ptr ex_ptr) mutable {
                return std::move(func(std::move(ex_ptr))).extract_sender();
            });
        return Future<T>(std::move(composed));
    }

    // Ensure (Requirement 9.6) — runs func on both the value and error
    // paths via exec::finally (confirmed available during the Phase 0
    // spike), then propagates the original completion unchanged.
    template<typename F> auto ensure(F&& func) -> Future<T> {
        auto cleanup = ex::just(kythira::unit{}) |
                       ex::then([func = std::forward<F>(func)](kythira::unit) mutable { func(); });
        auto composed = exec::finally(std::move(_sender), std::move(cleanup));
        return Future<T>(std::move(composed));
    }

    // Continuation scheduling (Requirement 9.4). Composed the same way as
    // delay() below — let_value/let_error hopping onto handle->schedule()
    // (an already-erased any_sender_t<unit>) — rather than
    // stdexec::continues_on directly against a type-erased any_sender_t<T>,
    // which this vendored version's own domain-resolution machinery
    // rejects at compile time when the upstream sender is itself
    // type-erased.
    auto via(scheduler_handle* handle) -> Future<T> {
        if (handle == nullptr) {
            throw std::invalid_argument("via: scheduler_handle cannot be null");
        }
        auto with_value =
            std::move(_sender) | ex::let_value([handle](T value) mutable {
                // handle->schedule()'s own value completion is
                // set_value_t(kythira::unit) (one argument), not
                // set_value_t() — every downstream let_value callback
                // chained directly onto it must take a kythira::unit
                // parameter to match.
                return handle->schedule() |
                       ex::let_value([value = std::move(value)](kythira::unit) mutable {
                           return ex::just(std::move(value));
                       });
            });
        auto composed =
            std::move(with_value) | ex::let_error([handle](std::exception_ptr ex_ptr) mutable {
                return handle->schedule() | ex::let_value([ex_ptr](kythira::unit) mutable {
                           // Bare just_error(...) has no set_value_t(...)
                           // signature of its own (the same issue the
                           // Phase 0 spike found for sync_wait) — erasing
                           // through any_sender_t<T> gives let_value's own
                           // completion-signature deduction an explicit,
                           // well-formed sender type to work with.
                           return any_sender_t<T>(ex::just_error(std::move(ex_ptr)));
                       });
            });
        return Future<T>(std::move(composed));
    }

    auto via(scheduler_handle& handle) -> Future<T> { return via(&handle); }

    // Concept-compliance overload — future_continuation requires
    // f.via(std::declval<void*>()) to compile, mirroring the Folly
    // backend's own via(void*) overload.
    auto via(void* handle) -> Future<T> { return via(static_cast<scheduler_handle*>(handle)); }

    // Delay (Requirement 9.5) — shifts completion later by `duration`
    // without blocking a thread for the wait; applies uniformly to both
    // the value and error paths so a delayed Future's outcome (success or
    // failure) is unchanged, only its timing shifts.
    auto delay(std::chrono::milliseconds duration) -> Future<T> {
        auto with_value =
            std::move(_sender) | ex::let_value([duration](T value) mutable {
                return exec::schedule_after(global_timed_context().get_scheduler(), duration) |
                       ex::then(
                           [value = std::move(value)]() mutable -> T { return std::move(value); });
            });
        auto composed =
            std::move(with_value) | ex::let_error([duration](std::exception_ptr ex_ptr) mutable {
                return exec::schedule_after(global_timed_context().get_scheduler(), duration) |
                       ex::let_value([ex_ptr]() mutable {
                           return any_sender_t<T>(ex::just_error(std::move(ex_ptr)));
                       });
            });
        return Future<T>(std::move(composed));
    }

    // Timeout (Requirement 9.5) — races the original sender against a
    // timer; whichever completes first wins, matching folly::Future's
    // within() semantics (a fired timeout produces future_timeout, not a
    // silent hang or thread-blocking sleep). Composed as two branches
    // launched via exec::start_detached, both racing to fulfill a shared
    // single_shot_channel via try_set_value/try_set_error (the same
    // first-writer-wins pattern FutureCollector::collectAny uses) rather
    // than exec::when_any: when_any is available and does race correctly,
    // but its cancel-the-loser behavior needs the losing branch's stop
    // token to reach it through this file's any_sender_t<T> type erasure,
    // and any_receiver_t<T> is declared with the default empty
    // query-forwarding list, so that never happens — when_any then waits
    // forever for a losing single_shot_channel-backed branch that can
    // never acknowledge a stop request it never received. This pattern
    // sidesteps the problem entirely: the loser is simply left to run to
    // completion in the background and its result is silently discarded.
    // exec::start_detached rather than exec::async_scope::spawn() for the
    // same reason noted above the FutureCollector helper this reuses:
    // async_scope's destructor asserts every spawned operation has already
    // completed, which a genuinely abandoned loser (e.g. a promise the
    // caller never fulfills) can violate for an unbounded time.
    auto within(std::chrono::milliseconds timeout) -> Future<T> {
        struct within_state {
            single_shot_channel<T> out;
        };
        auto state = std::make_shared<within_state>();
        auto future = Future<T>(state->out);

        auto original_record = [state](Try<T> result) {
            if (result.hasException()) {
                state->out.try_set_error(result.exception());
            } else {
                state->out.try_set_value(std::move(result.value()));
            }
        };
        exec::start_detached(detail::spawn_recording<T>(std::move(_sender), original_record));

        exec::start_detached(exec::schedule_after(global_timed_context().get_scheduler(), timeout) |
                             ex::then([state]() mutable {
                                 state->out.try_set_error(
                                     std::make_exception_ptr(future_timeout{}));
                             }));

        return future;
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

    // See Future<T>'s identical constructor for why this is templated
    // rather than taking any_sender_t<kythira::unit> by value — avoids a
    // real memory-corruption bug in this vendored stdexec's
    // exec::any_sender small-buffer-optimized move constructor.
    template<typename Sender>
    requires(!std::is_same_v<std::decay_t<Sender>, single_shot_channel<kythira::unit>>)
    explicit Future(Sender&& sender)
        : _sender(std::forward<Sender>(sender)), _is_ready_fn([] { return true; }) {}

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

    auto extract_sender() && -> any_sender_t<kythira::unit> { return std::move(_sender); }

    template<typename F>
    auto thenValue(F&& func) -> Future<std::invoke_result_t<F>>
    requires(!detail::is_future_v<std::invoke_result_t<F>>)
    {
        using R = std::invoke_result_t<F>;
        if constexpr (std::is_void_v<R>) {
            auto composed = std::move(_sender) |
                            ex::then([func = std::forward<F>(func)](kythira::unit) mutable {
                                func();
                                return kythira::unit{};
                            });
            return Future<void>(std::move(composed));
        } else {
            auto composed =
                std::move(_sender) |
                ex::then([func = std::forward<F>(func)](kythira::unit) mutable { return func(); });
            return Future<R>(std::move(composed));
        }
    }

    template<typename F>
    auto thenValue(F&& func) -> std::invoke_result_t<F>
    requires(detail::is_future_v<std::invoke_result_t<F>>)
    {
        using FutureR = std::invoke_result_t<F>;
        using InnerT = typename FutureR::value_type;
        using ErasedInnerT = std::conditional_t<std::is_void_v<InnerT>, kythira::unit, InnerT>;
        auto composed = std::move(_sender) |
                        ex::let_value([func = std::forward<F>(func)](kythira::unit) mutable {
                            return std::move(func()).extract_sender();
                        });
        return FutureR(std::move(composed));
    }

    template<typename F>
    auto thenTry(F&& func) -> Future<std::invoke_result_t<F, Try<void>>>
    requires(!detail::is_future_v<std::invoke_result_t<F, Try<void>>>)
    {
        using R = std::invoke_result_t<F, Try<void>>;
        auto with_value = std::move(_sender) | ex::let_value([func](kythira::unit) mutable {
                              if constexpr (std::is_void_v<R>) {
                                  func(Try<void>(kythira::unit{}));
                                  return ex::just(kythira::unit{});
                              } else {
                                  return ex::just(func(Try<void>(kythira::unit{})));
                              }
                          });
        auto composed =
            std::move(with_value) | ex::let_error([func](std::exception_ptr ex_ptr) mutable {
                if constexpr (std::is_void_v<R>) {
                    func(Try<void>(std::move(ex_ptr)));
                    return ex::just(kythira::unit{});
                } else {
                    return ex::just(func(Try<void>(std::move(ex_ptr))));
                }
            });
        using ErasedR = std::conditional_t<std::is_void_v<R>, kythira::unit, R>;
        if constexpr (std::is_void_v<R>) {
            return Future<void>(std::move(composed));
        } else {
            return Future<R>(std::move(composed));
        }
    }

    template<typename F>
    auto thenTry(F&& func) -> std::invoke_result_t<F, Try<void>>
    requires(detail::is_future_v<std::invoke_result_t<F, Try<void>>>)
    {
        using FutureR = std::invoke_result_t<F, Try<void>>;
        using InnerT = typename FutureR::value_type;
        using ErasedInnerT = std::conditional_t<std::is_void_v<InnerT>, kythira::unit, InnerT>;
        auto with_value = std::move(_sender) | ex::let_value([func](kythira::unit) mutable {
                              return std::move(func(Try<void>(kythira::unit{}))).extract_sender();
                          });
        auto composed =
            std::move(with_value) | ex::let_error([func](std::exception_ptr ex_ptr) mutable {
                return std::move(func(Try<void>(std::move(ex_ptr)))).extract_sender();
            });
        return FutureR(std::move(composed));
    }

    template<typename F>
    auto thenError(F&& func) -> Future<void>
    requires(!detail::is_future_v<std::invoke_result_t<F, std::exception_ptr>>)
    {
        auto composed =
            std::move(_sender) |
            ex::let_error([func = std::forward<F>(func)](std::exception_ptr ex_ptr) mutable {
                func(std::move(ex_ptr));
                return ex::just(kythira::unit{});
            });
        return Future<void>(std::move(composed));
    }

    template<typename F>
    auto thenError(F&& func) -> Future<void>
    requires(detail::is_future_v<std::invoke_result_t<F, std::exception_ptr>>)
    {
        auto composed =
            std::move(_sender) |
            ex::let_error([func = std::forward<F>(func)](std::exception_ptr ex_ptr) mutable {
                return std::move(func(std::move(ex_ptr))).extract_sender();
            });
        return Future<void>(std::move(composed));
    }

    template<typename F> auto ensure(F&& func) -> Future<void> {
        auto cleanup = ex::just(kythira::unit{}) |
                       ex::then([func = std::forward<F>(func)](kythira::unit) mutable { func(); });
        auto composed = exec::finally(std::move(_sender), std::move(cleanup));
        return Future<void>(std::move(composed));
    }

    auto via(scheduler_handle* handle) -> Future<void> {
        if (handle == nullptr) {
            throw std::invalid_argument("via: scheduler_handle cannot be null");
        }
        auto with_value = std::move(_sender) | ex::let_value([handle](kythira::unit) mutable {
                              return handle->schedule();
                          });
        auto composed =
            std::move(with_value) | ex::let_error([handle](std::exception_ptr ex_ptr) mutable {
                return handle->schedule() | ex::let_value([ex_ptr](kythira::unit) mutable {
                           return any_sender_t<kythira::unit>(ex::just_error(std::move(ex_ptr)));
                       });
            });
        return Future<void>(std::move(composed));
    }

    auto via(scheduler_handle& handle) -> Future<void> { return via(&handle); }

    auto via(void* handle) -> Future<void> { return via(static_cast<scheduler_handle*>(handle)); }

    auto delay(std::chrono::milliseconds duration) -> Future<void> {
        auto with_value =
            std::move(_sender) | ex::let_value([duration](kythira::unit) mutable {
                return exec::schedule_after(global_timed_context().get_scheduler(), duration) |
                       ex::then([]() mutable { return kythira::unit{}; });
            });
        auto composed =
            std::move(with_value) | ex::let_error([duration](std::exception_ptr ex_ptr) mutable {
                return exec::schedule_after(global_timed_context().get_scheduler(), duration) |
                       ex::let_value([ex_ptr]() mutable {
                           return any_sender_t<kythira::unit>(ex::just_error(std::move(ex_ptr)));
                       });
            });
        return Future<void>(std::move(composed));
    }

    // See Future<T>::within() for why this races via a shared
    // single_shot_channel and exec::start_detached rather than
    // exec::when_any/exec::async_scope.
    auto within(std::chrono::milliseconds timeout) -> Future<void> {
        struct within_state {
            single_shot_channel<kythira::unit> out;
        };
        auto state = std::make_shared<within_state>();
        auto future = Future<void>(state->out);

        auto original_record = [state](Try<void> result) {
            if (result.hasException()) {
                state->out.try_set_error(result.exception());
            } else {
                state->out.try_set_value(kythira::unit{});
            }
        };
        exec::start_detached(detail::spawn_recording<void>(std::move(_sender), original_record));

        exec::start_detached(exec::schedule_after(global_timed_context().get_scheduler(), timeout) |
                             ex::then([state]() mutable {
                                 state->out.try_set_error(
                                     std::make_exception_ptr(future_timeout{}));
                             }));

        return future;
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
static_assert(kythira::future_continuation<Future<int>, int>,
              "stdexec_backend::Future<int> must satisfy future_continuation concept");
static_assert(kythira::future_continuation<Future<void>, void>,
              "stdexec_backend::Future<void> must satisfy future_continuation concept");
// future_transformable is not asserted for T=void: the concept itself uses
// std::function<int(T)>, which is ill-formed for T=void regardless of
// backend — the Folly backend (include/raft/future.hpp) has the same gap.
static_assert(kythira::future_transformable<Future<int>, int>,
              "stdexec_backend::Future<int> must satisfy future_transformable concept");

// ── FutureFactory (Requirement 8) ───────────────────────────────────────────
//
// Direct wrapping, no bridge needed — factory-created futures are the pull
// model's native case (design.md).

class FutureFactory {
public:
    FutureFactory() = delete;
    FutureFactory(const FutureFactory&) = delete;
    FutureFactory(FutureFactory&&) = delete;
    auto operator=(const FutureFactory&) -> FutureFactory& = delete;
    auto operator=(FutureFactory&&) -> FutureFactory& = delete;

    template<typename T> static auto makeFuture(T value) -> Future<std::decay_t<T>> {
        using ValueType = std::decay_t<T>;
        return Future<ValueType>(ex::just(std::move(value)));
    }

    static auto makeFuture() -> Future<void> { return Future<void>(ex::just(kythira::unit{})); }

    static auto makeFuture(kythira::unit) -> Future<void> {
        return Future<void>(ex::just(kythira::unit{}));
    }

    // Non-template overload matching the future_factory concept's exact
    // `makeExceptionalFuture<int>(exception_ptr)` call shape; the explicit
    // template parameter is required (T can't be deduced from ex alone).
    template<typename T> static auto makeExceptionalFuture(std::exception_ptr ex) -> Future<T> {
        if (!ex) {
            throw std::invalid_argument("makeExceptionalFuture: exception_ptr cannot be null");
        }
        using ErasedT = std::conditional_t<std::is_void_v<T>, kythira::unit, T>;
        return Future<T>(ex::just_error(std::move(ex)));
    }

    static auto makeReadyFuture() -> Future<kythira::unit> {
        return Future<kythira::unit>(ex::just(kythira::unit{}));
    }

    template<typename T> static auto makeReadyFuture(T value) -> Future<std::decay_t<T>> {
        return makeFuture(std::move(value));
    }
};

static_assert(kythira::future_factory<FutureFactory>,
              "stdexec_backend::FutureFactory must satisfy future_factory concept");

// ── FutureCollector (Requirement 10) ────────────────────────────────────────
//
// collectAll/collectAny/collectAnyWithoutException/collectN all operate over
// a *runtime-sized* std::vector<Future<T>>, which rules out stdexec's own
// when_all/exec::when_any CPOs directly for the vector-taking overloads —
// both are variadic, compile-time-arity combinators (confirmed: this
// vendored version has no when_all_range/dynamic_when_all equivalent).
// Instead, each input future is launched via exec::start_detached, which
// already solves the hard part (safely running a runtime-dynamic number of
// operations to completion without a hand-rolled, cycle-prone
// operation-state-ownership scheme — every launched operation self-deletes
// on completion via the library's own internal mechanism). An earlier
// version of this file used exec::async_scope::spawn() instead; switched
// after Future<T>::within() (which reuses this same helper) exposed a real
// bug in that choice — collectAny/collectAnyWithoutException/collectN all
// have "losing" branches that may take arbitrarily long (or, given an
// abandoned promise, never) to finish, and async_scope's destructor
// asserts every spawned operation has already completed by the time the
// scope itself goes away, which such a straggler can violate.
// start_detached has no scope object with that lifecycle requirement.
// Completion bookkeeping (which result goes where, when to stop) is done
// via a small per-call shared "collect state" that each spawned
// operation's completion callback records into and holds a shared_ptr to
// for its own lifetime — no cycle, since the state never references the
// operations back, only the eventual output channel/promise.

class FutureCollector {
public:
    FutureCollector() = delete;
    FutureCollector(const FutureCollector&) = delete;
    FutureCollector(FutureCollector&&) = delete;
    auto operator=(const FutureCollector&) -> FutureCollector& = delete;
    auto operator=(FutureCollector&&) -> FutureCollector& = delete;

    template<typename T>
    static auto collectAll(std::vector<Future<T>> futures) -> Future<std::vector<Try<T>>> {
        const std::size_t n = futures.size();
        Promise<std::vector<Try<T>>> promise;
        auto future = promise.getFuture();
        if (n == 0) {
            promise.setValue(std::vector<Try<T>>{});
            return future;
        }

        struct collect_state {
            std::vector<std::optional<Try<T>>> results;
            std::atomic<std::size_t> remaining;
            Promise<std::vector<Try<T>>> promise;
        };
        auto state = std::make_shared<collect_state>();
        state->results.resize(n);
        state->remaining.store(n, std::memory_order_relaxed);
        state->promise = std::move(promise);

        for (std::size_t i = 0; i < n; ++i) {
            auto sender = std::move(futures[i]).extract_sender();
            auto record = [state, i](Try<T> result) {
                state->results[i] = std::move(result);
                if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::vector<Try<T>> out;
                    out.reserve(state->results.size());
                    for (auto& r : state->results) {
                        out.push_back(std::move(*r));
                    }
                    state->promise.setValue(std::move(out));
                }
            };
            exec::start_detached(detail::spawn_recording<T>(std::move(sender), record));
        }
        return future;
    }

    // Returns the index and Try<T> of the first future to complete (value
    // or error) — matching folly::collectAny's exact result shape.
    template<typename T>
    static auto collectAny(std::vector<Future<T>> futures)
        -> Future<std::tuple<std::size_t, Try<T>>> {
        using ResultT = std::tuple<std::size_t, Try<T>>;
        if (futures.empty()) {
            return FutureFactory::makeExceptionalFuture<ResultT>(std::make_exception_ptr(
                std::invalid_argument("collectAny requires at least one future")));
        }
        const std::size_t n = futures.size();

        struct collect_state {
            single_shot_channel<ResultT> out;
        };
        auto state = std::make_shared<collect_state>();
        auto future = Future<ResultT>(state->out);

        for (std::size_t i = 0; i < n; ++i) {
            auto sender = std::move(futures[i]).extract_sender();
            auto record = [state, i](Try<T> result) {
                state->out.try_set_value(std::make_tuple(i, std::move(result)));
            };
            exec::start_detached(detail::spawn_recording<T>(std::move(sender), record));
        }
        return future;
    }

    // Returns the index (T = void) or (index, value) (T != void) of the
    // first future to complete *successfully*; if every future fails, the
    // result future is exceptional with the last-observed failure —
    // matching folly::collectAnyWithoutException's shape and its
    // continue-past-failures behavior confirmed during the Phase 0 spike.
    template<typename T> static auto collectAnyWithoutException(std::vector<Future<T>> futures) {
        using ResultT =
            std::conditional_t<std::is_void_v<T>, std::size_t, std::tuple<std::size_t, T>>;
        if (futures.empty()) {
            return FutureFactory::makeExceptionalFuture<ResultT>(std::make_exception_ptr(
                std::invalid_argument("collectAnyWithoutException requires at least one future")));
        }
        const std::size_t n = futures.size();

        struct collect_state {
            single_shot_channel<ResultT> out;
            std::atomic<std::size_t> remaining;
        };
        auto state = std::make_shared<collect_state>();
        state->remaining.store(n, std::memory_order_relaxed);
        auto future = Future<ResultT>(state->out);

        for (std::size_t i = 0; i < n; ++i) {
            auto sender = std::move(futures[i]).extract_sender();
            auto record = [state, i](Try<T> result) {
                if (result.hasValue()) {
                    if constexpr (std::is_void_v<T>) {
                        state->out.try_set_value(i);
                    } else {
                        state->out.try_set_value(std::make_tuple(i, std::move(result.value())));
                    }
                    return;
                }
                if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    // Last failure and no success ever arrived — fail the
                    // aggregate with this (the last-observed) exception.
                    state->out.try_set_error(result.exception());
                }
            };
            exec::start_detached(detail::spawn_recording<T>(std::move(sender), record));
        }
        return future;
    }

    // Returns the first `n` completions (value or error), tagged with
    // their original index, in arrival order — matching folly::collectN.
    // The remaining futures.size()-n operations are still run to
    // completion (their results are simply discarded) rather than
    // cancelled, matching this codebase's other collectors, none of which
    // support cancellation.
    template<typename T>
    static auto collectN(std::vector<Future<T>> futures, std::size_t n)
        -> Future<std::vector<std::tuple<std::size_t, Try<T>>>> {
        using ResultT = std::vector<std::tuple<std::size_t, Try<T>>>;
        if (n > futures.size()) {
            return FutureFactory::makeExceptionalFuture<ResultT>(std::make_exception_ptr(
                std::invalid_argument("collectN: n cannot be greater than futures.size()")));
        }
        if (n == 0) {
            return FutureFactory::makeFuture(ResultT{});
        }
        const std::size_t total = futures.size();

        struct collect_state {
            ResultT collected;
            std::mutex mu;
            std::size_t target;
            Promise<ResultT> promise;
            bool fulfilled = false;
        };
        auto state = std::make_shared<collect_state>();
        state->target = n;
        Promise<ResultT> promise;
        auto future = promise.getFuture();
        state->promise = std::move(promise);

        for (std::size_t i = 0; i < total; ++i) {
            auto sender = std::move(futures[i]).extract_sender();
            auto record = [state, i](Try<T> result) {
                std::lock_guard<std::mutex> lock(state->mu);
                if (state->fulfilled) {
                    return;
                }
                state->collected.emplace_back(i, std::move(result));
                if (state->collected.size() == state->target) {
                    state->fulfilled = true;
                    state->promise.setValue(std::move(state->collected));
                }
            };
            exec::start_detached(detail::spawn_recording<T>(std::move(sender), record));
        }
        return future;
    }
};

static_assert(kythira::future_collector<FutureCollector, Future<int>>,
              "stdexec_backend::FutureCollector must satisfy future_collector concept for "
              "Future<int>");

// ── scheduler_executor_shim (Requirement 3) ─────────────────────────────────
//
// A compatibility path satisfying the (still Folly-shaped) executor/
// keep_alive concepts by wrapping stdexec::sync_wait(schedule(scheduler) |
// then(func)) inside .add() — this blocks the calling thread of .add() for
// the duration of func, real overhead new code should avoid by using
// via(scheduler) directly instead of routing through this shim. Exists only
// so existing executor/keep_alive-typed call sites (which predate this
// spec and are not being converted) can, if they ever need to, accept a
// stdexec scheduler without their own code changing shape.
class scheduler_executor_shim {
public:
    template<typename Scheduler>
    requires(!std::is_same_v<std::decay_t<Scheduler>, scheduler_executor_shim>)
    explicit scheduler_executor_shim(Scheduler sched) : _handle(std::move(sched)) {}

    scheduler_executor_shim(const scheduler_executor_shim&) = default;
    auto operator=(const scheduler_executor_shim&) -> scheduler_executor_shim& = default;
    scheduler_executor_shim(scheduler_executor_shim&&) = default;
    auto operator=(scheduler_executor_shim&&) -> scheduler_executor_shim& = default;

    template<typename F> auto add(F&& func) -> void {
        ex::sync_wait(_handle.schedule() |
                      ex::then([func = std::forward<F>(func)](kythira::unit) mutable { func(); }));
    }

    [[nodiscard]] auto get() const -> void* {
        // keep_alive's `get()` only needs to return something
        // convertible-to-void*, conventionally used as an identity/validity
        // check by callers — this shim's scheduler_handle already provides
        // reference-equality semantics via operator==, so the KeepAlive
        // vocabulary's exact non-null pointer identity isn't load-bearing
        // here the way it is for the Folly backend's raw folly::Executor*.
        return const_cast<void*>(static_cast<const void*>(this));
    }

private:
    scheduler_handle _handle;
};

static_assert(kythira::executor<scheduler_executor_shim>,
              "stdexec_backend::scheduler_executor_shim must satisfy executor concept");
static_assert(kythira::keep_alive<scheduler_executor_shim>,
              "stdexec_backend::scheduler_executor_shim must satisfy keep_alive concept");

}  // namespace kythira::stdexec_backend

#endif  // KYTHIRA_HAS_STDEXEC
