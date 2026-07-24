// **Feature: boost-future-backend, Property 2: boost Try Fidelity**
// **Feature: boost-future-backend, Property 3: Promise Exactly-Once Fulfillment**
// **Feature: boost-future-backend, Property 4: Broken Promise Never Hangs**
// **Feature: boost-future-backend, Property 5: boost Future Blocking Get Correctness**
// **Feature: boost-future-backend, Property 9: Cross-Backend Concept Compliance**
//
// Concept-compliance for the boost::thread-backed Try/SemiPromise/Promise/
// Future/FutureFactory/executor_shim wrapper types is already verified via
// static_assert at the end of include/raft/future_boost.hpp itself
// (mirroring include/raft/future.hpp's and future_stdexec.hpp's identical
// pattern) — this file adds runtime behavior coverage: value/exception
// round trips through Try<T>, exactly-once Promise fulfillment (including
// the double-fulfillment and broken-promise cases), and Future<T>::get()
// blocking correctness, for both a value type and void.
//
// Validates: Requirements 2.1-2.4, 3.1-3.5, 4.1-4.4 (.kiro/specs/boost-future-backend/)
#define BOOST_TEST_MODULE boost_future_concept_compliance_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_boost.hpp>

#include <stdexcept>
#include <string>

using namespace kythira::boost_backend;

BOOST_AUTO_TEST_SUITE(boost_future_concept_compliance_tests)

BOOST_AUTO_TEST_CASE(promise_future_value_round_trip, *boost::unit_test::timeout(10)) {
    Promise<int> p;
    BOOST_CHECK(!p.isFulfilled());
    Future<int> f = p.getFuture();
    p.setValue(42);
    BOOST_CHECK(p.isFulfilled());
    BOOST_CHECK_EQUAL(std::move(f).get(), 42);
}

BOOST_AUTO_TEST_CASE(promise_future_exception_round_trip, *boost::unit_test::timeout(10)) {
    Promise<int> p;
    Future<int> f = p.getFuture();
    p.setException(std::make_exception_ptr(std::runtime_error("boom")));
    BOOST_CHECK(p.isFulfilled());
    BOOST_CHECK_THROW(std::move(f).get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(promise_future_exception_message_preserved, *boost::unit_test::timeout(10)) {
    Promise<int> p;
    Future<int> f = p.getFuture();
    p.setException(std::make_exception_ptr(std::runtime_error("specific message")));
    bool caught = false;
    try {
        std::move(f).get();
    } catch (const std::runtime_error& e) {
        caught = true;
        BOOST_CHECK_EQUAL(std::string(e.what()), "specific message");
    }
    BOOST_CHECK(caught);
}

BOOST_AUTO_TEST_CASE(void_promise_future_round_trip, *boost::unit_test::timeout(10)) {
    Promise<void> p;
    BOOST_CHECK(!p.isFulfilled());
    Future<void> f = p.getFuture();
    p.setValue(kythira::unit{});
    BOOST_CHECK(p.isFulfilled());
    BOOST_CHECK_NO_THROW(std::move(f).get());
}

BOOST_AUTO_TEST_CASE(try_type_captures_value, *boost::unit_test::timeout(10)) {
    boost::promise<int> bp;
    boost::future<int> bf = bp.get_future();
    bp.set_value(7);
    Try<int> t(std::move(bf));
    BOOST_CHECK(t.hasValue());
    BOOST_CHECK(!t.hasException());
    BOOST_CHECK_EQUAL(t.value(), 7);
    BOOST_CHECK(t.exception() == nullptr);
}

BOOST_AUTO_TEST_CASE(try_type_captures_exception, *boost::unit_test::timeout(10)) {
    boost::promise<int> bp;
    boost::future<int> bf = bp.get_future();
    bp.set_exception(boost::copy_exception(std::runtime_error("failure")));
    Try<int> t(std::move(bf));
    BOOST_CHECK(!t.hasValue());
    BOOST_CHECK(t.hasException());
    BOOST_CHECK(t.exception() != nullptr);
}

BOOST_AUTO_TEST_CASE(try_type_void_captures_value, *boost::unit_test::timeout(10)) {
    boost::promise<void> bp;
    boost::future<void> bf = bp.get_future();
    bp.set_value();
    Try<void> t(std::move(bf));
    BOOST_CHECK(t.hasValue());
    BOOST_CHECK(!t.hasException());
}

// Property 3: double-fulfillment on the same Promise must not corrupt the
// first result — the second attempt is expected to throw (matching
// boost::promise's own promise_already_satisfied behavior at the concept
// boundary), and the future's original value must remain observable.
BOOST_AUTO_TEST_CASE(double_fulfillment_does_not_corrupt_first_result,
                     *boost::unit_test::timeout(10)) {
    Promise<int> p;
    Future<int> f = p.getFuture();
    p.setValue(1);
    BOOST_CHECK_THROW(p.setValue(2), std::exception);
    BOOST_CHECK_EQUAL(std::move(f).get(), 1);
}

// Property 4: a promise destroyed before fulfillment, with its future
// already retrieved, must complete that future with an error rather than
// hang - boost::promise's own broken_promise behavior.
BOOST_AUTO_TEST_CASE(broken_promise_never_hangs, *boost::unit_test::timeout(10)) {
    Future<int> f = [] {
        Promise<int> p;
        Future<int> inner = p.getFuture();
        return inner;
        // p destroyed here, never fulfilled
    }();
    BOOST_CHECK_THROW(std::move(f).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(future_is_ready_and_wait, *boost::unit_test::timeout(10)) {
    Promise<int> p;
    Future<int> f = p.getFuture();
    BOOST_CHECK(!f.isReady());
    BOOST_CHECK(!f.wait(std::chrono::milliseconds(50)));
    p.setValue(9);
    BOOST_CHECK(f.wait(std::chrono::milliseconds(1000)));
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_EQUAL(std::move(f).get(), 9);
}

BOOST_AUTO_TEST_SUITE_END()
