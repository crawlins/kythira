// Property 14: Backend Non-Interference (.kiro/specs/stdexec-future-backend/,
// Requirement 11.4; also .kiro/specs/boost-future-backend/, Requirement
// 8.5/Property 11, extended into this file per that spec's own Task 16
// note to reuse this test rather than add a new one). This is an ordinary
// test file, not a build target that is expected to fail to compile: every
// check below is a `static_assert(!some_concept<...>)` (or
// `!std::is_convertible_v<...>`) verifying that a particular pairing of
// backends is rejected at compile time. The assertions passing is what
// makes this file compile and this test "pass" — a regression that
// accidentally made one of these combinations well-formed would show up as
// a compile error here, not a runtime failure.
//
// Each check is expressed as a templated `concept` rather than a bare
// `static_assert(!requires(...) {...})`: a requires-expression only gets
// SFINAE immunity (evaluating to `false` instead of raising a hard
// compile error) for failures that occur during *template argument
// substitution*. Calling a concrete, non-template member function (every
// via()/collectAll() overload here is non-template for a fixed T) with a
// mismatched argument type is an ordinary overload-resolution failure with
// no template substitution involved, so GCC treats it as a hard error even
// inside a bare requires-expression at namespace scope — confirmed
// directly: `static_assert(!requires(A a, B b) { a.foo(b); })` fails to
// compile with "cannot convert 'B' to ...", not a graceful `false`, when
// `A::foo` is non-template. Wrapping the same check in a `concept`
// (introducing real template parameters `T`/`Arg` for the expression to
// depend on) restores the expected SFINAE behavior.
//
// Always compiled (see tests/CMakeLists.txt): raft/future_stdexec.hpp and
// raft/future_boost.hpp are both safe no-op headers (their entire body,
// including their respective third-party #includes, sits behind
// `#ifdef KYTHIRA_HAS_STDEXEC` / `#ifdef KYTHIRA_HAS_BOOST_FUTURE`) when
// that backend was not enabled at configure time — so each backend's own
// checks below are wrapped in the matching #if guard and simply compile
// away to nothing when there is no second backend to check non-interference
// against.

#include <raft/future.hpp>
#include <raft/future_stdexec.hpp>
#include <raft/future_boost.hpp>

#include <vector>

namespace {

// via(void*) exists on both backends purely for future_continuation
// concept compliance (`f.via(std::declval<void*>())` must be well-formed —
// see include/concepts/future.hpp) and, like any void* parameter, silently
// accepts a pointer to *anything* via implicit conversion — this is a
// pre-existing property of the Folly backend's own via(void*) overload,
// not something this spec introduces, so it is not treated as an
// interference bug here. The checks below instead target combinations
// that do *not* go through that catch-all: passing a backend-specific
// type *by value* (no implicit void* conversion exists for a non-pointer),
// and passing a Future/collector across backends where the parameter type
// requires an exact match.

template<typename F, typename Arg>
concept can_via = requires(F f, Arg arg) { f.via(arg); };

// FutureCollector is parameterized on the concrete Future type it operates
// over (Requirement 10, include/concepts/future.hpp's future_collector
// concept) specifically so each backend's own collector only ever accepts
// its own Future type — passing the other backend's futures must not
// silently compile via some implicit conversion path.
template<typename FutureVec>
concept can_folly_collect_all =
    requires(FutureVec futures) { kythira::FutureCollector::collectAll(std::move(futures)); };

#ifdef KYTHIRA_HAS_STDEXEC

static_assert(!can_via<kythira::stdexec_backend::Future<int>, kythira::Executor>,
              "stdexec_backend::Future::via must not accept a Folly Executor by value");
static_assert(!can_via<kythira::stdexec_backend::Future<int>, kythira::KeepAlive>,
              "stdexec_backend::Future::via must not accept a Folly KeepAlive");
static_assert(
    !can_via<kythira::Future<int>, kythira::stdexec_backend::scheduler_handle>,
    "Folly kythira::Future::via must not accept a stdexec_backend::scheduler_handle by value");

template<typename FutureVec>
concept can_stdexec_collect_all = requires(FutureVec futures) {
    kythira::stdexec_backend::FutureCollector::collectAll(std::move(futures));
};

static_assert(!can_stdexec_collect_all<std::vector<kythira::Future<int>>>,
              "stdexec_backend::FutureCollector::collectAll must not accept kythira::Future<int>");
static_assert(
    !can_folly_collect_all<std::vector<kythira::stdexec_backend::Future<int>>>,
    "kythira::FutureCollector::collectAll must not accept kythira::stdexec_backend::Future<int>");

// The two Future<int> types are never implicitly convertible to one
// another — each wraps a completely different underlying representation
// (folly::Future vs. an exec::any_sender-erased stdexec sender) and mixing
// them silently would defeat the whole point of keeping the backends in
// separate namespaces (design.md's "Why a new namespace" rationale).
// std::is_convertible_v is itself a class template, so this doesn't need
// the concept wrapper above — the substitution already happens through a
// real template.
static_assert(!std::is_convertible_v<kythira::stdexec_backend::Future<int>, kythira::Future<int>>,
              "stdexec_backend::Future<int> must not be convertible to kythira::Future<int>");
static_assert(
    !std::is_convertible_v<kythira::Future<int>, kythira::stdexec_backend::Future<int>>,
    "kythira::Future<int> must not be convertible to kythira::stdexec_backend::Future<int>");

// Same non-interference property for Promise/SemiPromise.
static_assert(!std::is_convertible_v<kythira::stdexec_backend::Promise<int>, kythira::Promise<int>>,
              "stdexec_backend::Promise<int> must not be convertible to kythira::Promise<int>");
static_assert(
    !std::is_convertible_v<kythira::Promise<int>, kythira::stdexec_backend::Promise<int>>,
    "kythira::Promise<int> must not be convertible to kythira::stdexec_backend::Promise<int>");

#endif  // KYTHIRA_HAS_STDEXEC

// boost_backend vs. Folly — same non-interference properties, applied to
// the third backend (.kiro/specs/boost-future-backend/, Requirement 8.5).
// No can_via check here, unlike the stdexec section above:
// boost_backend::Future::via() is deliberately templated on the executor
// type (template<typename Ex> auto via(Ex& ex), future_boost.hpp's own
// comment explains why — Boost.Thread's concrete executors don't share a
// common base to fix the parameter type to), so f.via(arg) type-checks for
// *any* reference argument at the requires-expression's immediate-context
// check — the actual mismatch only hard-errors once the function body
// (_f.then(ex, ...)) is instantiated, which an unevaluated requires-clause
// never triggers. That's not a real interference hole: calling it with a
// Folly Executor still fails to compile if anyone actually does it, just
// not detectably via this SFINAE-based check.
#ifdef KYTHIRA_HAS_BOOST_FUTURE

template<typename FutureVec>
concept can_boost_collect_all = requires(FutureVec futures) {
    kythira::boost_backend::FutureCollector::collectAll(std::move(futures));
};

static_assert(!can_boost_collect_all<std::vector<kythira::Future<int>>>,
              "boost_backend::FutureCollector::collectAll must not accept kythira::Future<int>");
static_assert(
    !can_folly_collect_all<std::vector<kythira::boost_backend::Future<int>>>,
    "kythira::FutureCollector::collectAll must not accept kythira::boost_backend::Future<int>");

static_assert(!std::is_convertible_v<kythira::boost_backend::Future<int>, kythira::Future<int>>,
              "boost_backend::Future<int> must not be convertible to kythira::Future<int>");
static_assert(
    !std::is_convertible_v<kythira::Future<int>, kythira::boost_backend::Future<int>>,
    "kythira::Future<int> must not be convertible to kythira::boost_backend::Future<int>");

static_assert(!std::is_convertible_v<kythira::boost_backend::Promise<int>, kythira::Promise<int>>,
              "boost_backend::Promise<int> must not be convertible to kythira::Promise<int>");
static_assert(
    !std::is_convertible_v<kythira::Promise<int>, kythira::boost_backend::Promise<int>>,
    "kythira::Promise<int> must not be convertible to kythira::boost_backend::Promise<int>");

#endif  // KYTHIRA_HAS_BOOST_FUTURE

// boost_backend vs. stdexec_backend — only meaningful (and only compiled)
// when both optional backends are enabled simultaneously. No can_via check
// here either, for the same templated-via() reason as the Folly section
// above.
#if defined(KYTHIRA_HAS_BOOST_FUTURE) && defined(KYTHIRA_HAS_STDEXEC)

template<typename FutureVec>
concept can_boost_collect_stdexec = requires(FutureVec futures) {
    kythira::boost_backend::FutureCollector::collectAll(std::move(futures));
};

static_assert(!can_boost_collect_stdexec<std::vector<kythira::stdexec_backend::Future<int>>>,
              "boost_backend::FutureCollector::collectAll must not accept "
              "kythira::stdexec_backend::Future<int>");
static_assert(!can_stdexec_collect_all<std::vector<kythira::boost_backend::Future<int>>>,
              "stdexec_backend::FutureCollector::collectAll must not accept "
              "kythira::boost_backend::Future<int>");

static_assert(
    !std::is_convertible_v<kythira::boost_backend::Future<int>,
                           kythira::stdexec_backend::Future<int>>,
    "boost_backend::Future<int> must not be convertible to kythira::stdexec_backend::Future<int>");
static_assert(
    !std::is_convertible_v<kythira::stdexec_backend::Future<int>,
                           kythira::boost_backend::Future<int>>,
    "kythira::stdexec_backend::Future<int> must not be convertible to boost_backend::Future<int>");

#endif  // KYTHIRA_HAS_BOOST_FUTURE && KYTHIRA_HAS_STDEXEC

}  // namespace

auto main() -> int {
    // All the checks above run at compile time via static_assert; nothing
    // left to verify at runtime.
    return 0;
}
