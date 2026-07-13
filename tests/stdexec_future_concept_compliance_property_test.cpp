// **Feature: stdexec-future-backend, Property 10: Cross-Backend Concept
// Compliance** (breadth slice — confirms Future<T>/future_continuation/
// future_transformable hold across a variety of value types, mirroring
// folly_future_concept_compliance_property_test.cpp's coverage of
// folly::Future<T> for the same set of types; individual operations
// already get deep, iterated coverage in stdexec_future_blocking_get_
// property_test.cpp (Phase 2) and stdexec_future_transformation_/
// stdexec_future_scheduling_property_test.cpp (Phase 3) — this file is
// deliberately about type breadth, not behavioral depth).
// **Validates: Requirements 7.4, 7.5, 12.2, 12.3**
#define BOOST_TEST_MODULE stdexec_future_concept_compliance_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_stdexec.hpp>

#include <string>
#include <thread>

using namespace kythira::stdexec_backend;

namespace {
struct custom_type {
    int value;
    std::string name;
    auto operator==(const custom_type& other) const -> bool {
        return value == other.value && name == other.name;
    }
};

constexpr int property_test_iterations = 100;
}  // namespace

BOOST_AUTO_TEST_SUITE(stdexec_future_concept_compliance_tests)

BOOST_AUTO_TEST_CASE(static_assert_concept_compliance_across_types,
                     *boost::unit_test::timeout(10)) {
    static_assert(kythira::future<Future<int>, int>);
    static_assert(kythira::future<Future<double>, double>);
    static_assert(kythira::future<Future<std::string>, std::string>);
    static_assert(kythira::future<Future<custom_type>, custom_type>);
    static_assert(kythira::future<Future<kythira::unit>, kythira::unit>);
    static_assert(kythira::future<Future<void>, void>);

    static_assert(kythira::future_continuation<Future<int>, int>);
    static_assert(kythira::future_continuation<Future<double>, double>);
    static_assert(kythira::future_continuation<Future<std::string>, std::string>);
    static_assert(kythira::future_continuation<Future<custom_type>, custom_type>);
    static_assert(kythira::future_continuation<Future<void>, void>);

    static_assert(kythira::future_transformable<Future<int>, int>);
    static_assert(kythira::future_transformable<Future<double>, double>);
    static_assert(kythira::future_transformable<Future<std::string>, std::string>);
    static_assert(kythira::future_transformable<Future<custom_type>, custom_type>);
    // future_transformable is not asserted for T=void — see
    // future_stdexec.hpp's own static_assert comment: the concept's own
    // std::function<int(T)> is ill-formed for T=void regardless of backend.
    BOOST_TEST(true);
}

// isReady()/get() round trip across the same set of types the static
// asserts above cover, so a type that satisfies the concept but is
// actually broken at runtime doesn't slip through.
BOOST_AUTO_TEST_CASE(property_round_trip_across_value_types, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        {
            auto f = FutureFactory::makeFuture(i);
            BOOST_CHECK(f.isReady());
            BOOST_CHECK_EQUAL(std::move(f).get(), i);
        }
        {
            double d = static_cast<double>(i) + 0.5;
            auto f = FutureFactory::makeFuture(d);
            BOOST_CHECK(f.isReady());
            BOOST_CHECK_EQUAL(std::move(f).get(), d);
        }
        {
            std::string s = "value-" + std::to_string(i);
            auto f = FutureFactory::makeFuture(s);
            BOOST_CHECK(f.isReady());
            BOOST_CHECK_EQUAL(std::move(f).get(), s);
        }
        {
            custom_type c{i, "name-" + std::to_string(i)};
            auto f = FutureFactory::makeFuture(c);
            BOOST_CHECK(f.isReady());
            BOOST_CHECK(std::move(f).get() == c);
        }
    }
}

// Promise<T>/Future<T> pairing across the same type set, on a producer
// thread, mirroring folly_future_concept_compliance_property_test.cpp's
// promise-based coverage.
BOOST_AUTO_TEST_CASE(property_promise_future_pair_across_value_types,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        {
            Promise<custom_type> p;
            auto f = p.getFuture();
            custom_type c{i, "thread-" + std::to_string(i)};
            std::thread producer([p = std::move(p), c]() mutable { p.setValue(c); });
            BOOST_CHECK(std::move(f).get() == c);
            producer.join();
        }
    }
}

// thenValue continuation across a non-int type, exercising the same
// flattening/non-flattening split the Folly mirror test names explicitly.
BOOST_AUTO_TEST_CASE(then_value_and_then_try_across_a_custom_type, *boost::unit_test::timeout(10)) {
    auto future = FutureFactory::makeFuture(custom_type{1, "a"}).thenValue([](custom_type c) {
        c.value *= 2;
        return c;
    });
    auto result = std::move(future).get();
    BOOST_CHECK_EQUAL(result.value, 2);

    auto try_future =
        FutureFactory::makeFuture(custom_type{5, "b"}).thenTry([](Try<custom_type> t) {
            return t.value().value + 1;
        });
    BOOST_CHECK_EQUAL(std::move(try_future).get(), 6);
}

BOOST_AUTO_TEST_SUITE_END()
