// **Feature: stdexec-future-backend, Property 11: Factory Operation
// Fidelity**
// For any value or exception, stdexec_backend::FutureFactory methods should
// create futures that are immediately ready with the correct value or
// exception, matching the Folly backend's FutureFactory semantics.
// **Validates: Requirements 8.1, 8.2, 8.3**
#define BOOST_TEST_MODULE stdexec_future_factory_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_stdexec.hpp>

#include <random>
#include <stdexcept>
#include <string>

using namespace kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 200;
}

BOOST_AUTO_TEST_SUITE(stdexec_future_factory_tests)

BOOST_AUTO_TEST_CASE(static_assert_concept_compliance, *boost::unit_test::timeout(10)) {
    static_assert(kythira::future_factory<FutureFactory>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(property_make_future_is_ready_with_correct_value,
                     *boost::unit_test::timeout(60)) {
    std::mt19937 gen(1234);
    std::uniform_int_distribution<int> dist(-100000, 100000);
    for (int i = 0; i < property_test_iterations; ++i) {
        int v = dist(gen);
        auto future = FutureFactory::makeFuture(v);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(std::move(future).get(), v);
    }
}

BOOST_AUTO_TEST_CASE(property_make_future_string, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        std::string s = "value-" + std::to_string(i);
        auto future = FutureFactory::makeFuture(s);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(std::move(future).get(), s);
    }
}

BOOST_AUTO_TEST_CASE(make_future_void_is_ready, *boost::unit_test::timeout(10)) {
    auto future = FutureFactory::makeFuture();
    BOOST_CHECK(future.isReady());
    std::move(future).get();
}

BOOST_AUTO_TEST_CASE(property_make_exceptional_future_rethrows, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        std::string msg = "exceptional-" + std::to_string(i);
        auto future = FutureFactory::makeExceptionalFuture<int>(
            std::make_exception_ptr(std::runtime_error(msg)));
        BOOST_CHECK(future.isReady());
        bool threw = false;
        try {
            std::move(future).get();
        } catch (const std::runtime_error& e) {
            threw = true;
            BOOST_CHECK_EQUAL(std::string(e.what()), msg);
        }
        BOOST_CHECK(threw);
    }
}

BOOST_AUTO_TEST_CASE(make_exceptional_future_void, *boost::unit_test::timeout(10)) {
    auto future = FutureFactory::makeExceptionalFuture<void>(
        std::make_exception_ptr(std::runtime_error("void error")));
    BOOST_CHECK(future.isReady());
    BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(make_exceptional_future_rejects_null, *boost::unit_test::timeout(10)) {
    BOOST_CHECK_THROW(FutureFactory::makeExceptionalFuture<int>(std::exception_ptr{}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(make_ready_future_is_ready, *boost::unit_test::timeout(10)) {
    auto future = FutureFactory::makeReadyFuture();
    BOOST_CHECK(future.isReady());
    static_assert(std::is_same_v<decltype(future), Future<kythira::unit>>);
    std::move(future).get();
}

BOOST_AUTO_TEST_CASE(property_make_ready_future_with_value, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        auto future = FutureFactory::makeReadyFuture(i);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(std::move(future).get(), i);
    }
}

BOOST_AUTO_TEST_CASE(creation_does_not_block, *boost::unit_test::timeout(10)) {
    auto start = std::chrono::steady_clock::now();
    auto f1 = FutureFactory::makeFuture(1);
    auto f2 = FutureFactory::makeReadyFuture(2);
    auto f3 =
        FutureFactory::makeExceptionalFuture<int>(std::make_exception_ptr(std::runtime_error("x")));
    auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_CHECK(f1.isReady());
    BOOST_CHECK(f2.isReady());
    BOOST_CHECK(f3.isReady());
    BOOST_CHECK(elapsed < std::chrono::milliseconds(100));
}

BOOST_AUTO_TEST_SUITE_END()
