// **Feature: stdexec-future-backend** — cross-backend fidelity. Runs the
// same value/exception/collection scenario through both the Folly backend
// (kythira::Future/Promise/FutureFactory/FutureCollector,
// include/raft/future.hpp) and the stdexec backend
// (kythira::stdexec_backend::..., include/raft/future_stdexec.hpp) and
// asserts the externally observable result is equivalent — same value,
// same exception type/message, same ordering/index guarantees — even
// though the two backends' internal scheduling mechanics are unrelated.
// **Feature: stdexec-future-backend, Property 6 (stdexec Try Fidelity),
// Property 9 (Blocking Get Correctness), Property 11 (Factory Operation
// Fidelity), Property 12 (Continuation and Transformation Fidelity),
// Property 13 (Collective Operation Fidelity)**
// **Validates: Requirements 5.5, 7.4, 7.5, 8.1, 8.2, 8.3, 9.1, 9.2, 9.3,
// 10.1, 10.2, 10.3, 10.4, 12.4**
#define BOOST_TEST_MODULE stdexec_concept_wrappers_interoperability_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future.hpp>
#include <raft/future_stdexec.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace folly_backend = kythira;
namespace stdexec_side = kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 100;
}

BOOST_AUTO_TEST_SUITE(stdexec_concept_wrappers_interoperability_tests)

BOOST_AUTO_TEST_CASE(property_ready_value_fidelity, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        auto folly_f = folly_backend::FutureFactory::makeFuture(i);
        auto stdexec_f = stdexec_side::FutureFactory::makeFuture(i);
        BOOST_CHECK_EQUAL(folly_f.get(), std::move(stdexec_f).get());
    }
}

BOOST_AUTO_TEST_CASE(property_exceptional_value_fidelity, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        auto folly_f = folly_backend::FutureFactory::makeExceptionalFuture<int>(
            std::make_exception_ptr(std::runtime_error("boom")));
        auto stdexec_f = stdexec_side::FutureFactory::makeExceptionalFuture<int>(
            std::make_exception_ptr(std::runtime_error("boom")));

        bool folly_threw = false;
        try {
            folly_f.get();
        } catch (const std::runtime_error& e) {
            folly_threw = true;
            BOOST_CHECK_EQUAL(std::string(e.what()), "boom");
        }
        BOOST_CHECK(folly_threw);

        bool stdexec_threw = false;
        try {
            std::move(stdexec_f).get();
        } catch (const std::runtime_error& e) {
            stdexec_threw = true;
            BOOST_CHECK_EQUAL(std::string(e.what()), "boom");
        }
        BOOST_CHECK(stdexec_threw);
    }
}

BOOST_AUTO_TEST_CASE(then_value_chain_fidelity, *boost::unit_test::timeout(10)) {
    auto folly_result =
        folly_backend::FutureFactory::makeFuture(3).thenValue([](int x) { return x * 3; }).get();
    auto stdexec_result = std::move(stdexec_side::FutureFactory::makeFuture(3).thenValue([](int x) {
                              return x * 3;
                          })).get();
    BOOST_CHECK_EQUAL(folly_result, stdexec_result);
}

BOOST_AUTO_TEST_CASE(then_error_recovery_fidelity, *boost::unit_test::timeout(10)) {
    auto folly_result = folly_backend::FutureFactory::makeExceptionalFuture<int>(
                            std::make_exception_ptr(std::runtime_error("x")))
                            .thenError([](std::exception_ptr) { return -1; })
                            .get();
    auto stdexec_result = std::move(stdexec_side::FutureFactory::makeExceptionalFuture<int>(
                                        std::make_exception_ptr(std::runtime_error("x")))
                                        .thenError([](std::exception_ptr) { return -1; }))
                              .get();
    BOOST_CHECK_EQUAL(folly_result, stdexec_result);
}

BOOST_AUTO_TEST_CASE(promise_future_pair_fidelity, *boost::unit_test::timeout(10)) {
    folly_backend::Promise<int> folly_p;
    auto folly_f = folly_p.getFuture();
    folly_p.setValue(21);

    stdexec_side::Promise<int> stdexec_p;
    auto stdexec_f = stdexec_p.getFuture();
    stdexec_p.setValue(21);

    BOOST_CHECK_EQUAL(folly_f.get(), std::move(stdexec_f).get());
}

BOOST_AUTO_TEST_CASE(broken_promise_fidelity_both_backends_throw, *boost::unit_test::timeout(10)) {
    auto folly_f = [] {
        folly_backend::Promise<int> p;
        return p.getFuture();
    }();
    BOOST_CHECK_THROW(folly_f.get(), std::exception);

    auto stdexec_f = [] {
        stdexec_side::Promise<int> p;
        return p.getFuture();
    }();
    BOOST_CHECK_THROW(std::move(stdexec_f).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(collect_all_ordering_and_mixed_outcome_fidelity,
                     *boost::unit_test::timeout(10)) {
    {
        std::vector<folly_backend::Future<int>> futures;
        futures.push_back(folly_backend::FutureFactory::makeFuture(1));
        futures.push_back(folly_backend::FutureFactory::makeExceptionalFuture<int>(
            std::make_exception_ptr(std::runtime_error("x"))));
        futures.push_back(folly_backend::FutureFactory::makeFuture(3));
        auto results = folly_backend::FutureCollector::collectAll(std::move(futures)).get();
        BOOST_REQUIRE_EQUAL(results.size(), 3U);
        BOOST_CHECK_EQUAL(results[0].value(), 1);
        BOOST_CHECK(results[1].hasException());
        BOOST_CHECK_EQUAL(results[2].value(), 3);
    }
    {
        std::vector<stdexec_side::Future<int>> futures;
        futures.push_back(stdexec_side::FutureFactory::makeFuture(1));
        futures.push_back(stdexec_side::FutureFactory::makeExceptionalFuture<int>(
            std::make_exception_ptr(std::runtime_error("x"))));
        futures.push_back(stdexec_side::FutureFactory::makeFuture(3));
        auto results =
            std::move(stdexec_side::FutureCollector::collectAll(std::move(futures))).get();
        BOOST_REQUIRE_EQUAL(results.size(), 3U);
        BOOST_CHECK_EQUAL(results[0].value(), 1);
        BOOST_CHECK(results[1].hasException());
        BOOST_CHECK_EQUAL(results[2].value(), 3);
    }
}

BOOST_AUTO_TEST_CASE(collect_any_index_fidelity, *boost::unit_test::timeout(10)) {
    {
        std::vector<folly_backend::Future<int>> futures;
        futures.push_back(folly_backend::FutureFactory::makeFuture(77));
        auto [index, result] = folly_backend::FutureCollector::collectAny(std::move(futures)).get();
        BOOST_CHECK_EQUAL(index, 0U);
        BOOST_CHECK_EQUAL(result.value(), 77);
    }
    {
        std::vector<stdexec_side::Future<int>> futures;
        futures.push_back(stdexec_side::FutureFactory::makeFuture(77));
        auto [index, result] =
            std::move(stdexec_side::FutureCollector::collectAny(std::move(futures))).get();
        BOOST_CHECK_EQUAL(index, 0U);
        BOOST_CHECK_EQUAL(result.value(), 77);
    }
}

BOOST_AUTO_TEST_CASE(void_future_fidelity, *boost::unit_test::timeout(10)) {
    BOOST_CHECK_NO_THROW(folly_backend::FutureFactory::makeFuture().get());
    BOOST_CHECK_NO_THROW(std::move(stdexec_side::FutureFactory::makeFuture()).get());
}

BOOST_AUTO_TEST_SUITE_END()
