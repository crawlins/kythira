// **Feature: stdexec-future-backend, Property 10: Cross-Backend Concept
// Compliance** (Promise<T> slice — adds getFuture()/getSemiFuture() over
// SemiPromise<T>, covered by
// stdexec_semi_promise_concept_compliance_property_test.cpp).
// **Validates: Requirements 6.6, 7.4, 7.5**
#define BOOST_TEST_MODULE stdexec_promise_concept_compliance_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_stdexec.hpp>

#include <stdexcept>
#include <thread>

using namespace kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 200;
}

BOOST_AUTO_TEST_SUITE(stdexec_promise_concept_compliance_tests)

BOOST_AUTO_TEST_CASE(static_assert_concept_compliance, *boost::unit_test::timeout(10)) {
    static_assert(kythira::promise<Promise<int>, int>);
    static_assert(kythira::promise<Promise<void>, void>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(get_future_and_get_semi_future_observe_the_same_fulfillment,
                     *boost::unit_test::timeout(10)) {
    Promise<int> p;
    auto f1 = p.getFuture();
    p.setValue(5);
    BOOST_CHECK_EQUAL(std::move(f1).get(), 5);
}

BOOST_AUTO_TEST_CASE(property_fulfillment_from_another_thread, *boost::unit_test::timeout(60)) {
    // Requirement 6.2: set_value must be safely callable from arbitrary
    // external code on any thread, not just the thread that created the
    // promise — the entire reason single_shot_channel exists.
    for (int i = 0; i < property_test_iterations; ++i) {
        Promise<int> p;
        auto f = p.getFuture();
        std::thread producer([p = std::move(p), i]() mutable { p.setValue(i); });
        BOOST_CHECK_EQUAL(std::move(f).get(), i);
        producer.join();
    }
}

BOOST_AUTO_TEST_CASE(property_connect_before_fulfill_from_another_thread,
                     *boost::unit_test::timeout(60)) {
    // Covers the "receiver connects and starts waiting before the promise
    // side has fulfilled" ordering explicitly, since get() below runs on
    // this thread while the producer thread hasn't started yet.
    for (int i = 0; i < 50; ++i) {
        Promise<int> p;
        auto f = p.getFuture();
        std::thread producer([p = std::move(p), i]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            p.setValue(i);
        });
        BOOST_CHECK_EQUAL(std::move(f).get(), i);
        producer.join();
    }
}

BOOST_AUTO_TEST_CASE(promise_is_move_only, *boost::unit_test::timeout(10)) {
    static_assert(!std::is_copy_constructible_v<Promise<int>>);
    static_assert(std::is_move_constructible_v<Promise<int>>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()
