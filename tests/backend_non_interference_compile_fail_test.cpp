// Property 14: Backend Non-Interference (.kiro/specs/stdexec-future-backend/,
// Requirement 11.4). This is an ordinary test file, not a build target that
// is expected to fail to compile: every check below is a
// `static_assert(!some_concept<...>)` (or `!std::is_convertible_v<...>`)
// verifying that a particular Folly<->stdexec_backend mix is rejected at
// compile time. The assertions passing is what makes this file compile and
// this test "pass" — a regression that accidentally made one of these
// combinations well-formed would show up as a compile error here, not a
// runtime failure.
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
// Only compiled when stdexec is available (see tests/CMakeLists.txt);
// otherwise there is nothing to check non-interference against.

#include <raft/future.hpp>
#include <raft/future_stdexec.hpp>

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

static_assert(!can_via<kythira::stdexec_backend::Future<int>, kythira::Executor>,
              "stdexec_backend::Future::via must not accept a Folly Executor by value");
static_assert(!can_via<kythira::stdexec_backend::Future<int>, kythira::KeepAlive>,
              "stdexec_backend::Future::via must not accept a Folly KeepAlive");
static_assert(
    !can_via<kythira::Future<int>, kythira::stdexec_backend::scheduler_handle>,
    "Folly kythira::Future::via must not accept a stdexec_backend::scheduler_handle by value");

// FutureCollector is parameterized on the concrete Future type it operates
// over (Requirement 10, include/concepts/future.hpp's future_collector
// concept) specifically so each backend's own collector only ever accepts
// its own Future type — passing the other backend's futures must not
// silently compile via some implicit conversion path.
template<typename FutureVec>
concept can_stdexec_collect_all = requires(FutureVec futures) {
    kythira::stdexec_backend::FutureCollector::collectAll(std::move(futures));
};
template<typename FutureVec>
concept can_folly_collect_all =
    requires(FutureVec futures) { kythira::FutureCollector::collectAll(std::move(futures)); };

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

}  // namespace

auto main() -> int {
    // All the checks above run at compile time via static_assert; nothing
    // left to verify at runtime.
    return 0;
}
