// **Feature: stdexec-future-backend, Property 10: Cross-Backend Concept
// Compliance** (SemiPromise<T> slice — see design.md's Correctness
// Properties). Exercises SemiPromise<T>/SemiPromise<void> at the wrapper
// level (setValue/setException/isFulfilled), distinct from
// single_shot_channel_property_test.cpp's channel-internals coverage
// (Properties 7-8) which this wrapper delegates to.
// **Validates: Requirements 5.5, 6.6, 7.4, 7.5**
#define BOOST_TEST_MODULE stdexec_semi_promise_concept_compliance_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_stdexec.hpp>

#include <stdexcept>

using namespace kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 200;
}

BOOST_AUTO_TEST_SUITE(stdexec_semi_promise_concept_compliance_tests)

BOOST_AUTO_TEST_CASE(static_assert_concept_compliance, *boost::unit_test::timeout(10)) {
    static_assert(kythira::semi_promise<SemiPromise<int>, int>);
    static_assert(kythira::semi_promise<SemiPromise<void>, void>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(property_set_value_fulfills_and_is_reflected_by_future,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        Promise<int> p;
        BOOST_CHECK(!p.isFulfilled());
        auto f = p.getFuture();
        p.setValue(i);
        BOOST_CHECK(p.isFulfilled());
        BOOST_CHECK_EQUAL(std::move(f).get(), i);
    }
}

BOOST_AUTO_TEST_CASE(property_set_exception_fulfills_with_error, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        Promise<int> p;
        auto f = p.getFuture();
        p.setException(std::make_exception_ptr(std::runtime_error("boom")));
        BOOST_CHECK(p.isFulfilled());
        BOOST_CHECK_THROW(std::move(f).get(), std::runtime_error);
    }
}

BOOST_AUTO_TEST_CASE(set_value_twice_throws_on_the_loser, *boost::unit_test::timeout(10)) {
    Promise<int> p;
    p.setValue(1);
    BOOST_CHECK_THROW(p.setValue(2), std::logic_error);
}

BOOST_AUTO_TEST_CASE(set_exception_with_null_pointer_throws, *boost::unit_test::timeout(10)) {
    Promise<int> p;
    BOOST_CHECK_THROW(p.setException(std::exception_ptr{}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(unfulfilled_promise_destruction_breaks_the_promise,
                     *boost::unit_test::timeout(10)) {
    Future<int> f = [] {
        Promise<int> p;
        return p.getFuture();
    }();
    BOOST_CHECK_THROW(std::move(f).get(), broken_promise);
}

BOOST_AUTO_TEST_CASE(fulfilled_promise_destruction_does_not_break_the_promise,
                     *boost::unit_test::timeout(10)) {
    Future<int> f = [] {
        Promise<int> p;
        p.setValue(11);
        return p.getFuture();
    }();
    BOOST_CHECK_EQUAL(std::move(f).get(), 11);
}

BOOST_AUTO_TEST_CASE(void_specialization_set_value_variants, *boost::unit_test::timeout(10)) {
    {
        Promise<void> p;
        auto f = p.getFuture();
        p.setValue();
        BOOST_CHECK_NO_THROW(std::move(f).get());
    }
    {
        Promise<void> p;
        auto f = p.getFuture();
        p.setValue(kythira::unit{});
        BOOST_CHECK_NO_THROW(std::move(f).get());
    }
}

BOOST_AUTO_TEST_SUITE_END()
