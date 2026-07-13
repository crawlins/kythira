// **Feature: stdexec-future-backend, Property 15: Backend Selection
// Isolation**
// For the currently configured KYTHIRA_DEFAULT_FUTURE_BACKEND CMake
// option, kythira::future_default<T> should resolve to exactly the
// selected backend's Future<T> (folly by default), and any templated
// core code already generic over a future-satisfying type should accept
// future_default<T> without modification — the alias exists solely for
// non-templated call sites, per Requirement 11.2/11.5.
//
// The CMake option is fixed at configure time, so a single test binary
// can only observe one setting; this file verifies internal consistency
// (future_default<T> matches whichever backend KYTHIRA_FUTURE_BACKEND_
// STDEXEC does or doesn't define, mirroring future_default.hpp's own
// #if) rather than hardcoding an assumption about which one is active.
// **Validates: Requirements 11.2, 11.5**
#define BOOST_TEST_MODULE backend_selection_isolation_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <concepts/future.hpp>
#include <raft/future_default.hpp>

#include <type_traits>

namespace {

// Requirement 11.2: templated core code generic over a future-satisfying
// type must accept future_default<T> exactly as it would any other
// concept-satisfying future, without needing to know which backend it is.
template<kythira::future<int> F> constexpr auto generic_isready_check(const F& f) -> bool {
    return f.isReady();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(backend_selection_isolation_tests)

BOOST_AUTO_TEST_CASE(future_default_satisfies_the_future_concept, *boost::unit_test::timeout(10)) {
    static_assert(kythira::future<kythira::future_default<int>, int>,
                  "kythira::future_default<int> must satisfy the future concept regardless of "
                  "which backend KYTHIRA_DEFAULT_FUTURE_BACKEND selects");
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(future_default_resolves_to_the_configured_backend,
                     *boost::unit_test::timeout(10)) {
#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
    static_assert(
        std::is_same_v<kythira::future_default<int>, kythira::stdexec_backend::Future<int>>,
        "KYTHIRA_FUTURE_BACKEND_STDEXEC is defined, so future_default<int> must be "
        "stdexec_backend::Future<int>");
#else
    static_assert(std::is_same_v<kythira::future_default<int>, kythira::Future<int>>,
                  "KYTHIRA_FUTURE_BACKEND_STDEXEC is not defined, so future_default<int> must "
                  "default to the Folly backend's Future<int>");
#endif
    BOOST_TEST(true);
}

// Requirement 11.2 in practice: a factory call using whichever backend
// future_default<T> resolves to, fed into generic templated code that
// only knows about the future concept — no backend-specific branching
// needed at this call site.
BOOST_AUTO_TEST_CASE(generic_templated_code_accepts_future_default,
                     *boost::unit_test::timeout(10)) {
#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC)
    auto f = kythira::stdexec_backend::FutureFactory::makeFuture(7);
#else
    auto f = kythira::FutureFactory::makeFuture(7);
#endif
    static_assert(std::is_same_v<decltype(f), kythira::future_default<int>>);
    BOOST_CHECK(generic_isready_check(f));
    BOOST_CHECK_EQUAL(std::move(f).get(), 7);
}

BOOST_AUTO_TEST_SUITE_END()
