// **Feature: stdexec-future-backend, Property 10: Cross-Backend Concept
// Compliance**
// For any generic function templated purely on the concepts in
// include/concepts/future.hpp (not on a concrete backend type), both the
// Folly-backed kythira::Future/Promise/Try/FutureFactory/FutureCollector
// and the stdexec-backed kythira::stdexec_backend equivalents should
// satisfy the same concepts and be usable as template arguments
// interchangeably, while remaining distinct, non-interchangeable types
// (Requirement 11.1) — this file includes both backends' headers together
// specifically to prove they coexist in one translation unit without
// collision.
// **Validates: Requirements 5.5, 6.6, 7.4, 7.5, 8.4, 10.5, 12.1**
#define BOOST_TEST_MODULE cross_backend_concept_compliance_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <concepts/future.hpp>
#include <raft/future.hpp>
#include <raft/future_stdexec.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>

BOOST_AUTO_TEST_SUITE(cross_backend_concept_compliance_tests)

// A generic function constrained purely on the try_type concept — no
// mention of folly:: or stdexec_backend:: in its signature.
template<typename Try, typename T>
requires kythira::try_type<Try, T>
auto generic_try_has_value(const Try& t) -> bool {
    return t.hasValue();
}

// Generic over the future concept.
template<typename F, typename T>
requires kythira::future<F, T>
auto generic_future_is_ready(const F& f) -> bool {
    return f.isReady();
}

// Generic over the future_factory concept.
template<typename Factory>
requires kythira::future_factory<Factory>
auto generic_make_and_check_ready(int value) -> bool {
    auto future = Factory::makeFuture(value);
    return future.isReady() && std::move(future).get() == value;
}

BOOST_AUTO_TEST_CASE(both_backends_try_types_satisfy_the_same_generic_function,
                     *boost::unit_test::timeout(10)) {
    kythira::Try<int> folly_try(42);
    BOOST_CHECK((generic_try_has_value<kythira::Try<int>, int>(folly_try)));

    kythira::stdexec_backend::Try<int> stdexec_try(42);
    BOOST_CHECK((generic_try_has_value<kythira::stdexec_backend::Try<int>, int>(stdexec_try)));
}

BOOST_AUTO_TEST_CASE(both_backends_futures_satisfy_the_same_generic_function,
                     *boost::unit_test::timeout(10)) {
    auto folly_future = kythira::FutureFactory::makeFuture(7);
    BOOST_CHECK((generic_future_is_ready<kythira::Future<int>, int>(folly_future)));
    BOOST_CHECK_EQUAL(std::move(folly_future).get(), 7);

    auto stdexec_future = kythira::stdexec_backend::FutureFactory::makeFuture(7);
    BOOST_CHECK(
        (generic_future_is_ready<kythira::stdexec_backend::Future<int>, int>(stdexec_future)));
    BOOST_CHECK_EQUAL(std::move(stdexec_future).get(), 7);
}

BOOST_AUTO_TEST_CASE(both_backends_factories_satisfy_the_same_generic_function,
                     *boost::unit_test::timeout(10)) {
    BOOST_CHECK(generic_make_and_check_ready<kythira::FutureFactory>(99));
    BOOST_CHECK(generic_make_and_check_ready<kythira::stdexec_backend::FutureFactory>(99));
}

// Requirement 11.1: distinct namespaces, distinct (non-interchangeable)
// types — a stdexec_backend::Future<int> is not the same type as, and not
// implicitly convertible to/from, kythira::Future<int>.
BOOST_AUTO_TEST_CASE(backends_remain_distinct_non_interchangeable_types,
                     *boost::unit_test::timeout(10)) {
    static_assert(!std::is_same_v<kythira::Future<int>, kythira::stdexec_backend::Future<int>>);
    static_assert(
        !std::is_convertible_v<kythira::Future<int>, kythira::stdexec_backend::Future<int>>);
    static_assert(
        !std::is_convertible_v<kythira::stdexec_backend::Future<int>, kythira::Future<int>>);
    static_assert(!std::is_same_v<kythira::Promise<int>, kythira::stdexec_backend::Promise<int>>);
    static_assert(!std::is_same_v<kythira::Try<int>, kythira::stdexec_backend::Try<int>>);
    BOOST_TEST(true);
}

// All concept static_asserts from both include/raft/future.hpp and
// include/raft/future_stdexec.hpp already run at header-inclusion time
// (Requirement 12.1's own mandate: static_assert at compile time, mirrored
// across both backends) — re-stating a representative subset here directly
// against the concepts imported from concepts/future.hpp confirms this
// translation unit sees both backends' compliance simultaneously, not just
// each backend's own header in isolation.
BOOST_AUTO_TEST_CASE(representative_concept_static_asserts_hold_for_both_backends,
                     *boost::unit_test::timeout(10)) {
    static_assert(kythira::try_type<kythira::Try<int>, int>);
    static_assert(kythira::try_type<kythira::stdexec_backend::Try<int>, int>);

    static_assert(kythira::future<kythira::Future<int>, int>);
    static_assert(kythira::future<kythira::stdexec_backend::Future<int>, int>);

    static_assert(kythira::semi_promise<kythira::SemiPromise<int>, int>);
    static_assert(kythira::semi_promise<kythira::stdexec_backend::SemiPromise<int>, int>);

    static_assert(kythira::promise<kythira::Promise<int>, int>);
    static_assert(kythira::promise<kythira::stdexec_backend::Promise<int>, int>);

    static_assert(kythira::future_factory<kythira::FutureFactory>);
    static_assert(kythira::future_factory<kythira::stdexec_backend::FutureFactory>);

    static_assert(kythira::future_collector<kythira::FutureCollector, kythira::Future<int>>);
    static_assert(kythira::future_collector<kythira::stdexec_backend::FutureCollector,
                                            kythira::stdexec_backend::Future<int>>);

    static_assert(kythira::future_continuation<kythira::Future<int>, int>);
    static_assert(kythira::future_continuation<kythira::stdexec_backend::Future<int>, int>);

    static_assert(kythira::future_transformable<kythira::Future<int>, int>);
    static_assert(kythira::future_transformable<kythira::stdexec_backend::Future<int>, int>);

    BOOST_TEST(true);
}

// Both backends' equivalent operations (thenValue) produce equivalent
// externally-observable results for equivalent inputs (Requirement 12.4),
// even though internal scheduling differs.
BOOST_AUTO_TEST_CASE(both_backends_then_value_produces_equivalent_results,
                     *boost::unit_test::timeout(10)) {
    auto folly_result =
        kythira::FutureFactory::makeFuture(10).thenValue([](int v) { return v * 3; });
    BOOST_CHECK_EQUAL(std::move(folly_result).get(), 30);

    auto stdexec_result = kythira::stdexec_backend::FutureFactory::makeFuture(10).thenValue(
        [](int v) { return v * 3; });
    BOOST_CHECK_EQUAL(std::move(stdexec_result).get(), 30);
}

// Both backends' makeExceptionalFuture produce equivalent exception
// propagation.
BOOST_AUTO_TEST_CASE(both_backends_exceptional_future_propagates_equivalently,
                     *boost::unit_test::timeout(10)) {
    auto folly_ex = kythira::FutureFactory::makeExceptionalFuture<int>(
        folly::exception_wrapper(std::runtime_error("boom")));
    BOOST_CHECK_THROW(std::move(folly_ex).get(), std::runtime_error);

    auto stdexec_ex = kythira::stdexec_backend::FutureFactory::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error("boom")));
    BOOST_CHECK_THROW(std::move(stdexec_ex).get(), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
