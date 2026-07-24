#pragma once

/// @file future_boost.hpp
/// @brief A third, `boost::thread`-backed implementation of the `kythira`
///     future/promise/executor family, alongside the default Folly-backed
///     one (`include/raft/future.hpp`) and the `stdexec`-backed one
///     (`include/raft/future_stdexec.hpp`).
///
/// Scope (see `.kiro/specs/boost-future-backend/` for the full design): no
/// production call site is hardcoded to this backend specifically by this
/// feature — call sites that use `kythira::future_default` (rather than
/// this namespace directly) resolve here only when
/// `KYTHIRA_DEFAULT_FUTURE_BACKEND=boost` is selected; neither the Folly
/// nor `stdexec` dependency is removed or made optional-only;
/// `boost::fibers`, Boost.Cobalt, and any other Boost async facility
/// besides `boost::thread`'s future/promise extension surface are out of
/// scope. This header is compiled only when
/// `KYTHIRA_HAS_BOOST_FUTURE` is defined (root `CMakeLists.txt`, when
/// `KYTHIRA_BUILD_BOOST_FUTURE_BACKEND` or
/// `KYTHIRA_DEFAULT_FUTURE_BACKEND=boost`); consumers that don't need this
/// backend never pay for it, and the file is a harmless no-op otherwise.
///
/// `then()`/`when_all`/`when_any` (and the executor-taking `then(Ex&, F)`
/// overload) are Boost's own `// EXTENSION`-labeled surface, not part of
/// the standard `std::future` API — confirmed by direct inspection of the
/// vendored Boost.Thread source, not assumed from documentation. Using
/// them is a deliberate, accepted maintenance risk: a future Boost
/// upgrade changing or removing this surface could require rework here.
/// See `.kiro/specs/boost-future-backend/spike-notes.md` for the exact
/// Boost version validated and two real corrections the Phase 0/2 spikes
/// found relative to this file's own original design (which macro
/// actually enables `then(Ex&, F)`, and the non-obvious
/// `boost::exception_ptr` vs. `std::exception_ptr` bridge).
///
/// Everything here lives in `kythira::boost_backend`, a distinct namespace
/// from the unqualified `kythira::Future`/`Promise`/`Try` the Folly
/// backend uses and from `kythira::stdexec_backend` — all three backends
/// satisfy the same concepts (`include/concepts/future.hpp`) but are
/// never silently interchangeable (Requirement 8.1/8.5).
///
/// Unlike `stdexec`, `boost::promise<T>::set_value()`/`set_exception()`
/// are already push-model (callable from arbitrary code, on any thread, at
/// any later time) — the same shape as `folly::Promise`. There is no
/// `stdexec`-style pull/push mismatch to bridge here, so
/// `SemiPromise<T>`/`Promise<T>` below are direct wraps of
/// `boost::promise<T>`, not a hand-rolled primitive.
///
/// `BOOST_THREAD_VERSION=4` and `BOOST_THREAD_PROVIDES_EXECUTORS` are
/// defined project-wide via `target_compile_definitions(network_simulator
/// INTERFACE ...)` in the root `CMakeLists.txt`, not in this header —
/// `BOOST_THREAD_VERSION` is a project-wide ABI setting (mixing
/// translation units compiled with different values is undefined behavior
/// for any `boost::future`/`boost::promise` object crossing a TU
/// boundary), so it must be set identically everywhere, not redefined
/// per-file.

#ifdef KYTHIRA_HAS_BOOST_FUTURE

#include "../concepts/future.hpp"

#include <boost/thread/future.hpp>
#include <boost/thread/executors/basic_thread_pool.hpp>
#include <boost/thread/executors/executor.hpp>
#include <boost/exception_ptr.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/executor_work_guard.hpp>

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
#include <type_traits>
#include <utility>
#include <vector>

namespace kythira::boost_backend {

// Forward declarations
template<typename T> class Try;
template<typename T> class SemiPromise;
template<typename T> class Promise;
template<typename T> class Future;
class FutureFactory;
class FutureCollector;
class executor_shim;

namespace detail {

// Detects Future<U> for any U, so thenValue/thenTry/thenError can tell a
// plain-value-returning callback from a Future-returning one and pick the
// automatic-flattening overload accordingly — mirrors
// include/raft/future.hpp's own detail::is_future trait for the Folly
// backend and future_stdexec.hpp's identical trait for the stdexec one.
template<typename T> struct is_future : std::false_type {};
template<typename T> struct is_future<Future<T>> : std::true_type {};
template<typename T> inline constexpr bool is_future_v = is_future<T>::value;

// The only correct bridge between std::exception_ptr and
// boost::exception_ptr (spike-notes.md Finding 6): boost::promise::
// set_exception takes boost::exception_ptr, which has an *implicit*
// converting constructor from std::exception_ptr - but using that
// implicit conversion directly compiles fine and then silently rethrows
// a boost::exception_detail::clone_impl<std::exception_ptr> wrapper
// instead of the original exception on get(), which no ordinary
// `catch (const std::exception&)` catches. A genuine catch-and-rethrow
// in both directions is required instead; verified end-to-end (original
// exception type and message both survive) before being used here.
template<typename T>
auto set_exception_from_std(boost::promise<T>& p, std::exception_ptr ep) -> void {
    // std::rethrow_exception(nullptr) is undefined behavior per the
    // standard (its precondition is a non-null exception_ptr) - libstdc++
    // segfaults rather than throwing, so this must be checked explicitly
    // rather than relying on the catch-all below.
    if (!ep) {
        try {
            throw std::invalid_argument(
                "kythira::boost_backend: null exception_ptr passed to makeExceptionalFuture/"
                "setException");
        } catch (...) {
            p.set_exception(boost::current_exception());
        }
        return;
    }
    try {
        std::rethrow_exception(ep);
    } catch (...) {
        p.set_exception(boost::current_exception());
    }
}

[[nodiscard]] inline auto to_std_exception_ptr(const boost::exception_ptr& bep)
    -> std::exception_ptr {
    if (!bep) {
        return nullptr;
    }
    try {
        boost::rethrow_exception(bep);
    } catch (...) {
        return std::current_exception();
    }
    return nullptr;  // unreachable: the catch above always returns
}

}  // namespace detail

// ── Try<T> (Requirement 2) ──────────────────────────────────────────────

/// @brief Snapshot of a ready `boost::future<T>`'s result — a value or an
///     exception, never both, never neither. Constructed from an
///     already-ready future (precondition: `f.is_ready()`); does not hold
///     a live future handle afterward, matching the Folly backend's
///     `Try<T>` being a snapshot, not a live handle.
template<typename T> class Try {
public:
    explicit Try(boost::future<T>&& f) {
        if (f.has_exception()) {
            _exception = detail::to_std_exception_ptr(f.get_exception_ptr());
        } else {
            _value = std::move(f).get();
        }
    }

    [[nodiscard]] auto hasValue() const -> bool { return _exception == nullptr; }
    [[nodiscard]] auto hasException() const -> bool { return _exception != nullptr; }

    [[nodiscard]] auto value() -> T& {
        if (_exception) {
            std::rethrow_exception(_exception);
        }
        return *_value;
    }
    [[nodiscard]] auto value() const -> const T& {
        if (_exception) {
            std::rethrow_exception(_exception);
        }
        return *_value;
    }

    [[nodiscard]] auto exception() const -> std::exception_ptr { return _exception; }

private:
    std::optional<T> _value;
    std::exception_ptr _exception;
};

template<> class Try<void> {
public:
    explicit Try(boost::future<void>&& f) {
        if (f.has_exception()) {
            _exception = detail::to_std_exception_ptr(f.get_exception_ptr());
        } else {
            std::move(f).get();
        }
    }

    [[nodiscard]] auto hasValue() const -> bool { return _exception == nullptr; }
    [[nodiscard]] auto hasException() const -> bool { return _exception != nullptr; }
    [[nodiscard]] auto exception() const -> std::exception_ptr { return _exception; }

private:
    std::exception_ptr _exception;
};

}  // namespace kythira::boost_backend

namespace kythira::boost_backend::detail {
struct test_struct {
    int value{0};
    [[nodiscard]] auto operator==(const test_struct&) const -> bool = default;
};
}  // namespace kythira::boost_backend::detail

namespace kythira::boost_backend {

static_assert(kythira::try_type<Try<int>, int>,
              "boost_backend::Try<int> must satisfy try_type concept");
static_assert(kythira::try_type<Try<void>, void>,
              "boost_backend::Try<void> must satisfy try_type concept");
static_assert(kythira::try_type<Try<std::string>, std::string>,
              "boost_backend::Try<std::string> must satisfy try_type concept");
static_assert(kythira::try_type<Try<detail::test_struct>, detail::test_struct>,
              "boost_backend::Try<test_struct> must satisfy try_type concept");

// ── SemiPromise<T>/Promise<T> (Requirement 3) ───────────────────────────
//
// Direct wraps of boost::promise<T> - no bridge primitive needed, unlike
// the stdexec backend's single_shot_channel: boost::promise::set_value()/
// set_exception() are already push-model (see this file's own top
// comment). Double-fulfillment and broken-promise both fall out of
// boost::promise's own documented behavior
// (boost::promise_already_satisfied, a future ready with
// boost::broken_promise) - both are std::exception-derived, so no
// conversion beyond the usual std::exception_ptr capture is needed.

template<typename T> class SemiPromise {
public:
    SemiPromise() = default;
    SemiPromise(const SemiPromise&) = delete;
    auto operator=(const SemiPromise&) -> SemiPromise& = delete;
    // std::atomic<bool> has no implicit move (its copy/move constructors
    // are both deleted, per the standard), so `= default` here would
    // itself be implicitly deleted - a manual load/store transfer instead,
    // matching the relaxed ordering appropriate for a move (the source is
    // being destroyed, no concurrent access to race against).
    SemiPromise(SemiPromise&& other) noexcept
        : _p(std::move(other._p)), _fulfilled(other._fulfilled.load(std::memory_order_relaxed)) {}
    auto operator=(SemiPromise&& other) noexcept -> SemiPromise& {
        _p = std::move(other._p);
        _fulfilled.store(other._fulfilled.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
        return *this;
    }

    auto setValue(T value) -> void {
        _p.set_value(std::move(value));
        _fulfilled.store(true, std::memory_order_release);
    }
    auto setException(std::exception_ptr ex) -> void {
        detail::set_exception_from_std(_p, ex);
        _fulfilled.store(true, std::memory_order_release);
    }
    // isFulfilled() tracks a wrapper-owned flag rather than querying
    // boost::promise directly - it has no direct query for "has this
    // promise's value/exception been set", only future-side readiness
    // checks. Set immediately after each successful setValue/
    // setException above; a failed (double-fulfillment) attempt throws
    // before reaching the store, so isFulfilled() correctly stays false
    // if the one-and-only legitimate fulfillment never actually happened.
    [[nodiscard]] auto isFulfilled() const -> bool {
        return _fulfilled.load(std::memory_order_acquire);
    }

protected:
    boost::promise<T> _p;
    std::atomic<bool> _fulfilled{false};
};

template<> class SemiPromise<void> {
public:
    SemiPromise() = default;
    SemiPromise(const SemiPromise&) = delete;
    auto operator=(const SemiPromise&) -> SemiPromise& = delete;
    SemiPromise(SemiPromise&& other) noexcept
        : _p(std::move(other._p)), _fulfilled(other._fulfilled.load(std::memory_order_relaxed)) {}
    auto operator=(SemiPromise&& other) noexcept -> SemiPromise& {
        _p = std::move(other._p);
        _fulfilled.store(other._fulfilled.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
        return *this;
    }

    auto setValue(kythira::unit) -> void {
        _p.set_value();
        _fulfilled.store(true, std::memory_order_release);
    }
    auto setException(std::exception_ptr ex) -> void {
        detail::set_exception_from_std(_p, ex);
        _fulfilled.store(true, std::memory_order_release);
    }
    [[nodiscard]] auto isFulfilled() const -> bool {
        return _fulfilled.load(std::memory_order_acquire);
    }

protected:
    boost::promise<void> _p;
    std::atomic<bool> _fulfilled{false};
};

template<typename T> class Promise : public SemiPromise<T> {
public:
    // getFuture()/getSemiFuture() both return the same Future<T> - no
    // separate semi-future type on this backend, matching what the
    // semi_promise concept already allows (it doesn't require getFuture()
    // to exist at all; promise<P,T> is the one that adds it). Declared
    // here, defined after Future<T> below (mirrors include/raft/
    // future.hpp's own via(const KeepAlive&) precedent) - Future<T> isn't
    // a complete type yet at this point, only forward-declared.
    auto getFuture() -> Future<T>;
    auto getSemiFuture() -> Future<T>;
};

static_assert(kythira::semi_promise<SemiPromise<int>, int>,
              "boost_backend::SemiPromise<int> must satisfy semi_promise concept");
static_assert(kythira::semi_promise<SemiPromise<void>, void>,
              "boost_backend::SemiPromise<void> must satisfy semi_promise concept");
static_assert(kythira::semi_promise<SemiPromise<std::string>, std::string>,
              "boost_backend::SemiPromise<std::string> must satisfy semi_promise concept");
// promise<Promise<T>, T> static_asserts deferred to just after Future<T>'s
// own definition below, for the same reason getFuture()/getSemiFuture()
// are declared-only here - Promise<T>::getFuture() must be a complete,
// defined function before the concept check that calls it can compile.

namespace detail {

// ── timer_service (Requirement 6.5) ─────────────────────────────────────
//
// One shared io_context + background thread + executor_work_guard for the
// whole process, not one thread per delay()/within() call. Boost.Thread
// has no built-in timed-continuation combinator (unlike stdexec's
// optional timed schedulers), so delay()/within() below reuse boost::asio
// (already a required vcpkg dependency of this project for unrelated
// reasons) instead of inventing a new primitive.
class timer_service {
public:
    static auto instance() -> timer_service& {
        static timer_service svc;
        return svc;
    }

    // Fires callback (on the timer thread) after `d` has elapsed. Returns
    // the underlying steady_timer so the caller can extend its lifetime
    // (and, if desired, cancel it) - the timer must outlive the async
    // wait or the callback never fires.
    auto schedule_after(std::chrono::milliseconds d, std::function<void()> callback)
        -> std::shared_ptr<boost::asio::steady_timer> {
        auto timer = std::make_shared<boost::asio::steady_timer>(_ctx, d);
        timer->async_wait(
            [timer, callback = std::move(callback)](const boost::system::error_code& ec) {
                if (!ec) {
                    callback();
                }
                // ec set (e.g. operation_aborted from an explicit cancel()) —
                // deliberately silent: a cancelled timer means the racing
                // real completion already won, exactly the "loser is a
                // harmless no-op" behavior delay()/within() rely on below.
            });
        return timer;
    }

private:
    timer_service() : _work(boost::asio::make_work_guard(_ctx)) {
        _thread = std::thread([this] { _ctx.run(); });
    }
    ~timer_service() {
        _work.reset();
        _ctx.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }
    timer_service(const timer_service&) = delete;
    auto operator=(const timer_service&) -> timer_service& = delete;

    boost::asio::io_context _ctx;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work;
    std::thread _thread;
};

}  // namespace detail

// ── Future<T> (Requirement 4) ────────────────────────────────────────────

template<typename T> class Future {
public:
    using value_type = T;

    explicit Future(boost::future<T>&& f) : _f(std::move(f)) {}

    auto get() && -> T { return std::move(_f).get(); }
    [[nodiscard]] auto isReady() const -> bool { return _f.is_ready(); }
    auto wait(std::chrono::milliseconds timeout) -> bool {
        return _f.wait_for(boost::chrono::milliseconds(timeout.count())) ==
               boost::future_status::ready;
    }

    // Run this future to completion in the background, discarding its
    // result (Requirement: portable fire-and-forget). No-op beyond
    // consuming *this: Boost.Thread's then() is eager - any continuation
    // already attached runs regardless of whether the returned Future
    // object is kept alive, unlike stdexec_backend's lazy senders (see
    // that backend's detach() for the contrast this method exists to
    // paper over).
    auto detach() && -> void {}

    // Not part of any concept — internal accessor used by the
    // Future-returning-callback flattening overload of thenValue below,
    // mirroring future_stdexec.hpp's own extract_sender() for the same
    // purpose.
    auto extract_boost_future() && -> boost::future<T> { return std::move(_f); }

    // thenValue (Requirement 6.1/6.3): then()'s callback receives the
    // completed boost::future<T> itself, not T - unwrapped here via
    // .get() (which rethrows on the error path, propagating through
    // then()'s own future normally, exactly like the Folly backend's
    // thenValue). Two overloads split on whether func returns a plain
    // value or another Future<U>, mirroring future_stdexec.hpp's
    // identical dual-overload split for the same flattening reason.
    template<typename F>
    auto thenValue(F&& func) -> Future<std::invoke_result_t<F, T>>
    requires(!detail::is_future_v<std::invoke_result_t<F, T>>)
    {
        using R = std::invoke_result_t<F, T>;
        auto continuation =
            _f.then([func = std::forward<F>(func)](boost::future<T> completed) mutable -> R {
                return func(completed.get());
            });
        return Future<R>(std::move(continuation));
    }

    // Future-returning overload (automatic flattening) — boost::future<T>::
    // then() does NOT auto-unwrap a future-returning callback (confirmed
    // empirically during Phase 3, not assumed: a then() callback returning
    // boost::future<U> directly produces boost::future<boost::future<U>>>,
    // not boost::future<U>). Bridged manually via a new Promise<U> that a
    // nested continuation on the inner Future<U> fulfills - also confirmed
    // empirically that discarding a then() call's own return value does
    // NOT cancel or prevent that continuation from running, which is what
    // makes the nested, fire-and-forget then() calls below safe.
    template<typename F>
    auto thenValue(F&& func) -> std::invoke_result_t<F, T>
    requires(detail::is_future_v<std::invoke_result_t<F, T>>)
    {
        using FutureU = std::invoke_result_t<F, T>;
        using U = typename FutureU::value_type;
        auto bridge = std::make_shared<Promise<U>>();
        Future<U> result = bridge->getFuture();
        _f.then([func = std::forward<F>(func), bridge](boost::future<T> completed) mutable {
            T value;
            try {
                value = completed.get();
            } catch (...) {
                bridge->setException(std::current_exception());
                return;
            }
            FutureU inner = func(std::move(value));
            // extract_boost_future() + then(), not inner.thenValue()/
            // thenError(): those two calls would need a U-typed lambda
            // parameter, which is ill-formed C++ when U is void (a
            // Future<void>-returning func). if constexpr below handles
            // both cases in one place instead.
            std::move(inner).extract_boost_future().then(
                [bridge](boost::future<U> inner_completed) mutable {
                    if (inner_completed.has_exception()) {
                        bridge->setException(
                            detail::to_std_exception_ptr(inner_completed.get_exception_ptr()));
                    } else if constexpr (std::is_void_v<U>) {
                        inner_completed.get();
                        bridge->setValue(kythira::unit{});
                    } else {
                        bridge->setValue(inner_completed.get());
                    }
                });
        });
        return result;
    }

    // thenTry: func receives a Try<T> snapshot of this future's result
    // (value or exception, never both, never neither) in a single
    // callback - no separate success/error path, unlike thenValue/
    // thenError. Not part of any concept (include/concepts/future.hpp has
    // no thenTry clause), but present on the Folly and stdexec backends
    // and used directly by include/raft/raft.hpp - added here for parity
    // so node<Types> genuinely works when instantiated with this backend,
    // not just Folly/stdexec. A more natural fit for boost::future<T>::
    // then() than thenValue is: then()'s callback already receives the
    // completed boost::future<T> itself, which is exactly the shape
    // Try<T>'s own constructor expects - no .get()-and-rethrow dance
    // needed. Two overloads split on plain-value vs Future<U>-returning
    // func, mirroring thenValue's identical split above.
    template<typename F>
    auto thenTry(F&& func) -> Future<std::invoke_result_t<F, Try<T>>>
    requires(!detail::is_future_v<std::invoke_result_t<F, Try<T>>>)
    {
        using R = std::invoke_result_t<F, Try<T>>;
        auto continuation =
            _f.then([func = std::forward<F>(func)](boost::future<T> completed) mutable -> R {
                return func(Try<T>(std::move(completed)));
            });
        return Future<R>(std::move(continuation));
    }

    template<typename F>
    auto thenTry(F&& func) -> std::invoke_result_t<F, Try<T>>
    requires(detail::is_future_v<std::invoke_result_t<F, Try<T>>>)
    {
        using FutureU = std::invoke_result_t<F, Try<T>>;
        using U = typename FutureU::value_type;
        auto bridge = std::make_shared<Promise<U>>();
        Future<U> result = bridge->getFuture();
        _f.then([func = std::forward<F>(func), bridge](boost::future<T> completed) mutable {
            FutureU inner = func(Try<T>(std::move(completed)));
            std::move(inner).extract_boost_future().then(
                [bridge](boost::future<U> inner_completed) mutable {
                    if (inner_completed.has_exception()) {
                        bridge->setException(
                            detail::to_std_exception_ptr(inner_completed.get_exception_ptr()));
                    } else if constexpr (std::is_void_v<U>) {
                        inner_completed.get();
                        bridge->setValue(kythira::unit{});
                    } else {
                        bridge->setValue(inner_completed.get());
                    }
                });
        });
        return result;
    }

    // thenError (Requirement 6.2): no separate thenError-shaped primitive
    // on this backend (Boost.Thread's error handling is inspected inside
    // the same then() callback, not a distinct combinator) - inspect
    // has_exception() before calling .get() so the success path passes
    // the value through unchanged and only the error path invokes func.
    template<typename F> auto thenError(F&& func) -> Future<T> {
        auto continuation =
            _f.then([func = std::forward<F>(func)](boost::future<T> completed) mutable -> T {
                if (completed.has_exception()) {
                    return func(detail::to_std_exception_ptr(completed.get_exception_ptr()));
                }
                return completed.get();
            });
        return Future<T>(std::move(continuation));
    }

    // ensure (Requirement 6.6): func runs on both the value and error
    // paths, result (value or exception) passed through unchanged.
    template<typename F> auto ensure(F&& func) -> Future<T> {
        auto continuation =
            _f.then([func = std::forward<F>(func)](boost::future<T> completed) mutable -> T {
                func();
                return completed.get();  // rethrows on the error path, after func() already ran
            });
        return Future<T>(std::move(continuation));
    }

    // via (Requirement 6.4): then(Ex&, F) overload, requires
    // BOOST_THREAD_PROVIDES_EXECUTORS (explicitly defined project-wide,
    // per this file's own top comment and spike-notes.md Finding 1).
    // Templated on the executor type, not fixed to boost::executors::
    // executor&: confirmed empirically that Boost.Thread's own concrete
    // executors (e.g. basic_thread_pool) do NOT inherit from that abstract
    // base - then(Ex&, F) itself is a template for exactly this reason
    // (duck-typed, any type with the right submit-shaped interface), so
    // via() mirrors that rather than artificially narrowing to one base
    // class most concrete executors don't actually derive from.
    template<typename Ex> auto via(Ex& ex) -> Future<T> {
        auto continuation =
            _f.then(ex, [](boost::future<T> completed) -> T { return completed.get(); });
        return Future<T>(std::move(continuation));
    }
    // void* overload: required by the future_continuation concept
    // (`f.via(std::declval<void*>())`), mirroring include/raft/future.hpp's
    // and future_stdexec.hpp's identical void*-accepting via() overload.
    // Casts to boost::executors::executor* specifically (the one Boost.
    // Thread type actually designed as a common abstraction, matching
    // executor_shim's own use of it above) - callers with a concrete
    // executor type that doesn't derive from it should call the
    // templated via(Ex&) overload directly instead of going through void*.
    auto via(void* ex) -> Future<T> { return via(*static_cast<boost::executors::executor*>(ex)); }

    // delay (Requirement 6.5): result (value or exception) passed through
    // unchanged once at least `d` has elapsed, via detail::timer_service.
    auto delay(std::chrono::milliseconds d) -> Future<T> {
        auto bridge = std::make_shared<Promise<T>>();
        Future<T> result = bridge->getFuture();
        auto timer_holder = std::make_shared<std::shared_ptr<boost::asio::steady_timer>>();
        _f.then([bridge, d, timer_holder](boost::future<T> completed) mutable {
            auto shared_completed = std::make_shared<boost::future<T>>(std::move(completed));
            *timer_holder = detail::timer_service::instance().schedule_after(
                d, [bridge, shared_completed]() mutable {
                    if (shared_completed->has_exception()) {
                        bridge->setException(
                            detail::to_std_exception_ptr(shared_completed->get_exception_ptr()));
                    } else {
                        bridge->setValue(shared_completed->get());
                    }
                });
        });
        return result;
    }

    // within (Requirement 6.5): races the original future's completion
    // against a timer_service expiry via bridge's own exactly-once
    // Promise semantics (Phase 2) - whichever fulfills first wins; the
    // loser is a harmless no-op (a losing setValue/setException throws
    // internally and is caught/ignored, matching "second fulfillment
    // attempt is discarded" rather than propagating a
    // promise_already_satisfied error to either racer).
    auto within(std::chrono::milliseconds timeout) -> Future<T> {
        auto bridge = std::make_shared<Promise<T>>();
        Future<T> result = bridge->getFuture();
        auto timer_holder = std::make_shared<std::shared_ptr<boost::asio::steady_timer>>();
        *timer_holder = detail::timer_service::instance().schedule_after(timeout, [bridge]() {
            try {
                bridge->setException(
                    std::make_exception_ptr(std::runtime_error("kythira::boost_backend: "
                                                               "future timed out (within())")));
            } catch (const boost::promise_already_satisfied&) {
                // Real completion already won the race - harmless.
            }
        });
        _f.then([bridge, timer_holder](boost::future<T> completed) mutable {
            (*timer_holder)->cancel();
            try {
                if (completed.has_exception()) {
                    bridge->setException(
                        detail::to_std_exception_ptr(completed.get_exception_ptr()));
                } else {
                    bridge->setValue(completed.get());
                }
            } catch (const boost::promise_already_satisfied&) {
                // Timeout already won the race - harmless.
            }
        });
        return result;
    }

private:
    boost::future<T> _f;
};

template<> class Future<void> {
public:
    using value_type = void;

    explicit Future(boost::future<void>&& f) : _f(std::move(f)) {}

    auto get() && -> void { std::move(_f).get(); }
    [[nodiscard]] auto isReady() const -> bool { return _f.is_ready(); }
    auto wait(std::chrono::milliseconds timeout) -> bool {
        return _f.wait_for(boost::chrono::milliseconds(timeout.count())) ==
               boost::future_status::ready;
    }

    // See Future<T>::detach() above - same eager-backend no-op rationale.
    auto detach() && -> void {}

    auto extract_boost_future() && -> boost::future<void> { return std::move(_f); }

    template<typename F>
    auto thenValue(F&& func) -> Future<std::invoke_result_t<F>>
    requires(!detail::is_future_v<std::invoke_result_t<F>>)
    {
        using R = std::invoke_result_t<F>;
        auto continuation =
            _f.then([func = std::forward<F>(func)](boost::future<void> completed) mutable -> R {
                completed.get();
                return func();
            });
        return Future<R>(std::move(continuation));
    }

    template<typename F>
    auto thenValue(F&& func) -> std::invoke_result_t<F>
    requires(detail::is_future_v<std::invoke_result_t<F>>)
    {
        using FutureU = std::invoke_result_t<F>;
        using U = typename FutureU::value_type;
        auto bridge = std::make_shared<Promise<U>>();
        Future<U> result = bridge->getFuture();
        _f.then([func = std::forward<F>(func), bridge](boost::future<void> completed) mutable {
            try {
                completed.get();
            } catch (...) {
                bridge->setException(std::current_exception());
                return;
            }
            FutureU inner = func();
            std::move(inner).extract_boost_future().then(
                [bridge](boost::future<U> inner_completed) mutable {
                    if (inner_completed.has_exception()) {
                        bridge->setException(
                            detail::to_std_exception_ptr(inner_completed.get_exception_ptr()));
                    } else if constexpr (std::is_void_v<U>) {
                        inner_completed.get();
                        bridge->setValue(kythira::unit{});
                    } else {
                        bridge->setValue(inner_completed.get());
                    }
                });
        });
        return result;
    }

    // thenTry: the Future<void> counterpart of Future<T>::thenTry above -
    // func receives a Try<void> snapshot of this future's result in a
    // single callback.
    template<typename F>
    auto thenTry(F&& func) -> Future<std::invoke_result_t<F, Try<void>>>
    requires(!detail::is_future_v<std::invoke_result_t<F, Try<void>>>)
    {
        using R = std::invoke_result_t<F, Try<void>>;
        auto continuation =
            _f.then([func = std::forward<F>(func)](boost::future<void> completed) mutable -> R {
                return func(Try<void>(std::move(completed)));
            });
        return Future<R>(std::move(continuation));
    }

    template<typename F>
    auto thenTry(F&& func) -> std::invoke_result_t<F, Try<void>>
    requires(detail::is_future_v<std::invoke_result_t<F, Try<void>>>)
    {
        using FutureU = std::invoke_result_t<F, Try<void>>;
        using U = typename FutureU::value_type;
        auto bridge = std::make_shared<Promise<U>>();
        Future<U> result = bridge->getFuture();
        _f.then([func = std::forward<F>(func), bridge](boost::future<void> completed) mutable {
            FutureU inner = func(Try<void>(std::move(completed)));
            std::move(inner).extract_boost_future().then(
                [bridge](boost::future<U> inner_completed) mutable {
                    if (inner_completed.has_exception()) {
                        bridge->setException(
                            detail::to_std_exception_ptr(inner_completed.get_exception_ptr()));
                    } else if constexpr (std::is_void_v<U>) {
                        inner_completed.get();
                        bridge->setValue(kythira::unit{});
                    } else {
                        bridge->setValue(inner_completed.get());
                    }
                });
        });
        return result;
    }

    template<typename F> auto thenError(F&& func) -> Future<void> {
        auto continuation =
            _f.then([func = std::forward<F>(func)](boost::future<void> completed) mutable -> void {
                if (completed.has_exception()) {
                    func(detail::to_std_exception_ptr(completed.get_exception_ptr()));
                    return;
                }
                completed.get();
            });
        return Future<void>(std::move(continuation));
    }

    template<typename F> auto ensure(F&& func) -> Future<void> {
        auto continuation =
            _f.then([func = std::forward<F>(func)](boost::future<void> completed) mutable -> void {
                func();
                completed.get();
            });
        return Future<void>(std::move(continuation));
    }

    template<typename Ex> auto via(Ex& ex) -> Future<void> {
        auto continuation =
            _f.then(ex, [](boost::future<void> completed) -> void { completed.get(); });
        return Future<void>(std::move(continuation));
    }
    auto via(void* ex) -> Future<void> {
        return via(*static_cast<boost::executors::executor*>(ex));
    }

    auto delay(std::chrono::milliseconds d) -> Future<void> {
        auto bridge = std::make_shared<Promise<void>>();
        Future<void> result = bridge->getFuture();
        auto timer_holder = std::make_shared<std::shared_ptr<boost::asio::steady_timer>>();
        _f.then([bridge, d, timer_holder](boost::future<void> completed) mutable {
            auto shared_completed = std::make_shared<boost::future<void>>(std::move(completed));
            *timer_holder = detail::timer_service::instance().schedule_after(
                d, [bridge, shared_completed]() mutable {
                    if (shared_completed->has_exception()) {
                        bridge->setException(
                            detail::to_std_exception_ptr(shared_completed->get_exception_ptr()));
                    } else {
                        bridge->setValue(kythira::unit{});
                    }
                });
        });
        return result;
    }

    auto within(std::chrono::milliseconds timeout) -> Future<void> {
        auto bridge = std::make_shared<Promise<void>>();
        Future<void> result = bridge->getFuture();
        auto timer_holder = std::make_shared<std::shared_ptr<boost::asio::steady_timer>>();
        *timer_holder = detail::timer_service::instance().schedule_after(timeout, [bridge]() {
            try {
                bridge->setException(
                    std::make_exception_ptr(std::runtime_error("kythira::boost_backend: "
                                                               "future timed out (within())")));
            } catch (const boost::promise_already_satisfied&) {
            }
        });
        _f.then([bridge, timer_holder](boost::future<void> completed) mutable {
            (*timer_holder)->cancel();
            try {
                if (completed.has_exception()) {
                    bridge->setException(
                        detail::to_std_exception_ptr(completed.get_exception_ptr()));
                } else {
                    completed.get();
                    bridge->setValue(kythira::unit{});
                }
            } catch (const boost::promise_already_satisfied&) {
            }
        });
        return result;
    }

private:
    boost::future<void> _f;
};

// Promise<T>::getFuture()/getSemiFuture() out-of-line definitions, now
// that Future<T> is complete.
template<typename T> auto Promise<T>::getFuture() -> Future<T> {
    return Future<T>(this->_p.get_future());
}
template<typename T> auto Promise<T>::getSemiFuture() -> Future<T> {
    return getFuture();
}

static_assert(kythira::promise<Promise<int>, int>,
              "boost_backend::Promise<int> must satisfy promise concept");
static_assert(kythira::promise<Promise<void>, void>,
              "boost_backend::Promise<void> must satisfy promise concept");
static_assert(kythira::future<Future<int>, int>,
              "boost_backend::Future<int> must satisfy future concept");
static_assert(kythira::future<Future<void>, void>,
              "boost_backend::Future<void> must satisfy future concept");
static_assert(kythira::future<Future<kythira::unit>, kythira::unit>,
              "boost_backend::Future<unit> must satisfy future concept");
static_assert(kythira::future<Future<std::string>, std::string>,
              "boost_backend::Future<std::string> must satisfy future concept");
static_assert(kythira::future_continuation<Future<int>, int>,
              "boost_backend::Future<int> must satisfy future_continuation concept");
static_assert(kythira::future_continuation<Future<void>, void>,
              "boost_backend::Future<void> must satisfy future_continuation concept");
static_assert(kythira::future_transformable<Future<int>, int>,
              "boost_backend::Future<int> must satisfy future_transformable concept");
// No future_transformable<Future<void>, void> assertion: neither the Folly
// nor stdexec backend asserts this either. The concept's own
// `f.thenValue(std::declval<std::function<int(T)>>())` clause substitutes
// T into a function-type parameter list - for T=void this is
// `std::function<int(void)>`, which (unlike literally writing `int(void)`
// in source) is ill-formed via template substitution, a pre-existing
// C++ language-level limitation of this concept clause itself, not
// specific to any one backend.

// ── FutureFactory (Requirement 5) ───────────────────────────────────────

class FutureFactory {
public:
    FutureFactory() = delete;
    FutureFactory(const FutureFactory&) = delete;
    auto operator=(const FutureFactory&) -> FutureFactory& = delete;
    FutureFactory(FutureFactory&&) = delete;
    auto operator=(FutureFactory&&) -> FutureFactory& = delete;

    template<typename T> static auto makeFuture(T value) -> Future<T> {
        return Future<T>(boost::make_ready_future(std::move(value)));
    }

    // Zero-arg overload returning a ready Future<void> - parity with the
    // Folly and stdexec backends' own zero-arg makeFuture(), both of which
    // return Future<void> rather than Future<kythira::unit> (that's what
    // makeReadyFuture() below is for). Distinct from makeReadyFuture():
    // this exists so generic test/call-site code that already does
    // `FutureFactory::makeFuture()` for "a ready void future" doesn't need
    // a backend-specific branch.
    static auto makeFuture() -> Future<void> {
        boost::promise<void> p;
        p.set_value();
        return Future<void>(p.get_future());
    }

    template<typename T> static auto makeExceptionalFuture(std::exception_ptr ex) -> Future<T> {
        boost::promise<T> p;
        detail::set_exception_from_std(p, ex);
        return Future<T>(p.get_future());
    }

    static auto makeReadyFuture() -> Future<kythira::unit> {
        return Future<kythira::unit>(boost::make_ready_future(kythira::unit{}));
    }

    // Value-taking overload - parity with the Folly and stdexec backends'
    // own makeReadyFuture(T), both of which accept a value directly rather
    // than requiring callers to go through makeFuture() for this case.
    template<typename T> static auto makeReadyFuture(T value) -> Future<T> {
        return Future<T>(boost::make_ready_future(std::move(value)));
    }
};

static_assert(kythira::future_factory<FutureFactory>,
              "boost_backend::FutureFactory must satisfy future_factory concept");

namespace detail {
// Result type for FutureCollector::collectAnyWithoutException: index-only
// for T = void (std::tuple<size_t, void> is ill-formed), (index, value)
// otherwise - matching Folly's and stdexec_backend's own void/non-void
// split for this method.
template<typename T>
using collect_any_without_exception_result_t =
    std::conditional_t<std::is_void_v<T>, std::size_t, std::tuple<std::size_t, T>>;
}  // namespace detail

// ── FutureCollector (Requirement 7) ─────────────────────────────────────

class FutureCollector {
public:
    FutureCollector() = delete;
    FutureCollector(const FutureCollector&) = delete;
    auto operator=(const FutureCollector&) -> FutureCollector& = delete;
    FutureCollector(FutureCollector&&) = delete;
    auto operator=(FutureCollector&&) -> FutureCollector& = delete;

    // collectAll (Requirement 7.1): boost::when_all's iterator-pair
    // overload (confirmed present in the Phase 0 spike) over the input's
    // extracted boost::future<T>s, converted to std::vector<Try<T>> in
    // original order - matches Folly's collectAll semantics exactly
    // (whole operation succeeds regardless of individual failures).
    template<typename T>
    static auto collectAll(std::vector<Future<T>> futures) -> Future<std::vector<Try<T>>> {
        std::vector<boost::future<T>> boost_futures;
        boost_futures.reserve(futures.size());
        for (auto& fut : futures) {
            boost_futures.push_back(std::move(fut).extract_boost_future());
        }
        auto composed =
            boost::when_all(boost_futures.begin(), boost_futures.end())
                .then(
                    [](boost::future<std::vector<boost::future<T>>> ready) -> std::vector<Try<T>> {
                        std::vector<boost::future<T>> inner = ready.get();
                        std::vector<Try<T>> results;
                        results.reserve(inner.size());
                        for (auto& f : inner) {
                            results.emplace_back(std::move(f));
                        }
                        return results;
                    });
        return Future<std::vector<Try<T>>>(std::move(composed));
    }

    // collectAny (Requirement 7.2): boost::when_any's iterator-pair
    // overload returns a future to a vector of ALL the original futures
    // (confirmed in the Phase 0/5 spikes, matching the Boost source itself
    // - detail::future_when_any_vector_shared_state), not just the one
    // that completed first - scan for is_ready() to find which one.
    template<typename T>
    static auto collectAny(std::vector<Future<T>> futures)
        -> Future<std::tuple<std::size_t, Try<T>>> {
        if (futures.empty()) {
            return FutureFactory::makeExceptionalFuture<std::tuple<std::size_t, Try<T>>>(
                std::make_exception_ptr(
                    std::invalid_argument("collectAny requires at least one future")));
        }
        std::vector<boost::future<T>> boost_futures;
        boost_futures.reserve(futures.size());
        for (auto& fut : futures) {
            boost_futures.push_back(std::move(fut).extract_boost_future());
        }
        auto composed =
            boost::when_any(boost_futures.begin(), boost_futures.end())
                .then([](boost::future<std::vector<boost::future<T>>> ready)
                          -> std::tuple<std::size_t, Try<T>> {
                    std::vector<boost::future<T>> inner = ready.get();
                    for (std::size_t i = 0; i < inner.size(); ++i) {
                        if (inner[i].is_ready()) {
                            return std::make_tuple(i, Try<T>(std::move(inner[i])));
                        }
                    }
                    // Unreachable: when_any only completes once at least
                    // one member is ready.
                    throw std::logic_error(
                        "kythira::boost_backend::collectAny: when_any completed with none ready");
                });
        return Future<std::tuple<std::size_t, Try<T>>>(std::move(composed));
    }

    // collectAnyWithoutException (Requirement 7.3): no single Boost
    // primitive distinguishes "first to complete" from "first to
    // succeed" - built on collectAny plus a retry-past-failures loop.
    // Recursive via an explicit helper (not a self-capturing lambda,
    // which can't reference itself without an extra indirection anyway)
    // operating on a shared, shrinking pool of (original_index, Future<T>)
    // pairs so the returned index always refers to the caller's original
    // vector, not the shrinking pool's own position.
    //
    // Result type is index-only for T = void (std::tuple<size_t, void> is
    // ill-formed) and (index, value) otherwise - matching Folly's and
    // stdexec_backend's own void/non-void split for this method.
    template<typename T>
    static auto collectAnyWithoutException(std::vector<Future<T>> futures)
        -> Future<detail::collect_any_without_exception_result_t<T>> {
        if (futures.empty()) {
            return FutureFactory::makeExceptionalFuture<
                detail::collect_any_without_exception_result_t<T>>(std::make_exception_ptr(
                std::invalid_argument("collectAnyWithoutException requires at least one future")));
        }
        auto bridge =
            std::make_shared<Promise<detail::collect_any_without_exception_result_t<T>>>();
        Future<detail::collect_any_without_exception_result_t<T>> result = bridge->getFuture();

        using pool_entry = std::pair<std::size_t, Future<T>>;
        auto pool = std::make_shared<std::vector<pool_entry>>();
        pool->reserve(futures.size());
        for (std::size_t i = 0; i < futures.size(); ++i) {
            pool->emplace_back(i, std::move(futures[i]));
        }
        try_next_without_exception(bridge, pool);
        return result;
    }

    // collectN (Requirement 7.4): same shrinking-pool shape as
    // collectAnyWithoutException, selecting the next-to-complete
    // (regardless of success/failure, matching Folly's collectN including
    // failed Trys in its result) until N have been collected.
    template<typename T>
    static auto collectN(std::vector<Future<T>> futures, std::size_t n)
        -> Future<std::vector<std::tuple<std::size_t, Try<T>>>> {
        if (n > futures.size()) {
            return FutureFactory::makeExceptionalFuture<
                std::vector<std::tuple<std::size_t, Try<T>>>>(std::make_exception_ptr(
                std::invalid_argument("collectN: n cannot be greater than futures.size()")));
        }
        if (n == 0) {
            return FutureFactory::makeFuture(std::vector<std::tuple<std::size_t, Try<T>>>{});
        }

        auto bridge = std::make_shared<Promise<std::vector<std::tuple<std::size_t, Try<T>>>>>();
        Future<std::vector<std::tuple<std::size_t, Try<T>>>> result = bridge->getFuture();

        using pool_entry = std::pair<std::size_t, Future<T>>;
        auto pool = std::make_shared<std::vector<pool_entry>>();
        pool->reserve(futures.size());
        for (std::size_t i = 0; i < futures.size(); ++i) {
            pool->emplace_back(i, std::move(futures[i]));
        }
        auto collected = std::make_shared<std::vector<std::tuple<std::size_t, Try<T>>>>();
        collect_next_n(bridge, pool, collected, n);
        return result;
    }

private:
    template<typename T> using pool_t = std::vector<std::pair<std::size_t, Future<T>>>;

    template<typename T>
    static auto try_next_without_exception(
        std::shared_ptr<Promise<detail::collect_any_without_exception_result_t<T>>> bridge,
        std::shared_ptr<pool_t<T>> pool) -> void {
        if (pool->empty()) {
            bridge->setException(std::make_exception_ptr(
                std::runtime_error("kythira::boost_backend::collectAnyWithoutException: "
                                   "every future failed")));
            return;
        }
        // select_one() consumes every pool member's underlying
        // boost::future<T> (via extract_boost_future()) - capture the
        // original indices before that happens so the winner/loser split
        // below can still report the caller's original vector index.
        std::vector<std::size_t> orig_indices;
        orig_indices.reserve(pool->size());
        for (auto& [orig_index, fut] : *pool) {
            orig_indices.push_back(orig_index);
        }
        std::vector<boost::future<T>> boost_futures;
        boost_futures.reserve(pool->size());
        for (auto& [orig_index, fut] : *pool) {
            boost_futures.push_back(std::move(fut).extract_boost_future());
        }
        auto shared_futures =
            std::make_shared<std::vector<boost::future<T>>>(std::move(boost_futures));
        boost::when_any(shared_futures->begin(), shared_futures->end())
            .then([bridge, pool, orig_indices,
                   shared_futures](boost::future<std::vector<boost::future<T>>> ready) mutable {
                std::vector<boost::future<T>> inner = ready.get();
                std::size_t winner_pos = inner.size();
                for (std::size_t i = 0; i < inner.size(); ++i) {
                    if (inner[i].is_ready()) {
                        winner_pos = i;
                        break;
                    }
                }
                pool_t<T> next;
                next.reserve(inner.size() - 1);
                for (std::size_t i = 0; i < inner.size(); ++i) {
                    if (i == winner_pos) continue;
                    next.emplace_back(orig_indices[i], Future<T>(std::move(inner[i])));
                }
                *pool = std::move(next);

                if (!inner[winner_pos].has_exception()) {
                    try {
                        if constexpr (std::is_void_v<T>) {
                            inner[winner_pos].get();
                            bridge->setValue(orig_indices[winner_pos]);
                        } else {
                            bridge->setValue(
                                std::make_tuple(orig_indices[winner_pos], inner[winner_pos].get()));
                        }
                    } catch (const boost::promise_already_satisfied&) {
                    }
                    return;
                }
                try_next_without_exception(bridge, pool);
            });
    }

    template<typename T>
    static auto collect_next_n(
        std::shared_ptr<Promise<std::vector<std::tuple<std::size_t, Try<T>>>>> bridge,
        std::shared_ptr<pool_t<T>> pool,
        std::shared_ptr<std::vector<std::tuple<std::size_t, Try<T>>>> collected, std::size_t n)
        -> void {
        if (collected->size() == n) {
            try {
                bridge->setValue(*collected);
            } catch (const boost::promise_already_satisfied&) {
            }
            return;
        }
        std::vector<std::size_t> orig_indices;
        orig_indices.reserve(pool->size());
        for (auto& [orig_index, fut] : *pool) {
            orig_indices.push_back(orig_index);
        }
        std::vector<boost::future<T>> boost_futures;
        boost_futures.reserve(pool->size());
        for (auto& [orig_index, fut] : *pool) {
            boost_futures.push_back(std::move(fut).extract_boost_future());
        }
        auto shared_futures =
            std::make_shared<std::vector<boost::future<T>>>(std::move(boost_futures));
        boost::when_any(shared_futures->begin(), shared_futures->end())
            .then([bridge, pool, collected, n, orig_indices,
                   shared_futures](boost::future<std::vector<boost::future<T>>> ready) mutable {
                std::vector<boost::future<T>> inner = ready.get();
                std::size_t winner_pos = inner.size();
                for (std::size_t i = 0; i < inner.size(); ++i) {
                    if (inner[i].is_ready()) {
                        winner_pos = i;
                        break;
                    }
                }
                pool_t<T> next;
                next.reserve(inner.size() - 1);
                for (std::size_t i = 0; i < inner.size(); ++i) {
                    if (i == winner_pos) continue;
                    next.emplace_back(orig_indices[i], Future<T>(std::move(inner[i])));
                }
                *pool = std::move(next);
                collected->emplace_back(orig_indices[winner_pos],
                                        Try<T>(std::move(inner[winner_pos])));
                collect_next_n(bridge, pool, collected, n);
            });
    }
};

static_assert(kythira::future_collector<FutureCollector, Future<int>>,
              "boost_backend::FutureCollector must satisfy future_collector concept");

// ── executor_shim (Requirement 3 analogue for the boost backend) ────────
//
// Satisfies the existing (Folly-shaped) executor/keep_alive concepts by
// wrapping a boost::executors::executor's submit() inside .add() - a
// compatibility path with real overhead (bridges a callback-submission
// call into Boost's own executor interface), not the primary way to use
// this backend's executors (via() above takes a boost::executors::executor
// directly, no shim needed there).
class executor_shim {
public:
    explicit executor_shim(boost::executors::executor& ex) : _ex(&ex) {}

    auto add(std::function<void()> func) -> void { _ex->submit(std::move(func)); }
    [[nodiscard]] auto get() const -> void* { return static_cast<void*>(_ex); }

private:
    boost::executors::executor* _ex;
};

static_assert(kythira::executor<executor_shim>,
              "boost_backend::executor_shim must satisfy executor concept");
static_assert(kythira::keep_alive<executor_shim>,
              "boost_backend::executor_shim must satisfy keep_alive concept");

}  // namespace kythira::boost_backend

#endif  // KYTHIRA_HAS_BOOST_FUTURE
