// **Feature: stdexec-future-backend, Property 1: Concept Regenericization
// Preserves Folly Compliance**
// For any wrapper type in the existing Folly backend that satisfied a
// concept in include/concepts/future.hpp before this feature, it should
// continue to satisfy the regenericized version of that concept (now
// expressed in terms of std::exception_ptr and kythira::unit rather than
// folly::exception_wrapper/folly::Unit), with identical runtime behavior.
// **Validates: Requirements 1.4, 1.5, 2.4**
#define BOOST_TEST_MODULE folly_backend_concept_regenericization_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future.hpp>
#include <concepts/future.hpp>

#include <folly/executors/InlineExecutor.h>

#include <stdexcept>
#include <string>

namespace {
constexpr int property_test_iterations = 100;
}

BOOST_AUTO_TEST_SUITE(folly_backend_concept_regenericization_tests)

// Every wrapper type this backend exposes, checked against every concept
// it claimed to satisfy before Phase 1's regenericization — the exact
// static_assert block at the end of include/raft/future.hpp itself,
// re-asserted here as this property's own dedicated test rather than
// relying only on that file compiling.
BOOST_AUTO_TEST_CASE(static_assert_folly_wrappers_satisfy_regenericized_concepts,
                     *boost::unit_test::timeout(10)) {
    static_assert(kythira::try_type<kythira::Try<int>, int>);
    static_assert(kythira::try_type<kythira::Try<void>, void>);
    static_assert(kythira::try_type<kythira::Try<std::string>, std::string>);

    static_assert(kythira::semi_promise<kythira::SemiPromise<int>, int>);
    static_assert(kythira::semi_promise<kythira::SemiPromise<void>, void>);

    static_assert(kythira::promise<kythira::Promise<int>, int>);
    static_assert(kythira::promise<kythira::Promise<void>, void>);

    static_assert(kythira::future<kythira::Future<int>, int>);
    static_assert(kythira::future<kythira::Future<void>, void>);
    static_assert(kythira::future_continuation<kythira::Future<int>, int>);
    static_assert(kythira::future_transformable<kythira::Future<int>, int>);

    static_assert(kythira::future_factory<kythira::FutureFactory>);
    static_assert(kythira::future_collector<kythira::FutureCollector, kythira::Future<int>>);

    static_assert(kythira::executor<kythira::Executor>);
    static_assert(kythira::keep_alive<kythira::KeepAlive>);

    BOOST_TEST(true);
}

// Requirement 1.5: the existing setException(const std::exception_ptr&)
// overload already satisfied the new concept requirement with no wrapper
// change needed (per design.md's analysis, confirmed by reading the
// implementation before committing to the design) — this exercises that
// overload directly, repeatedly, to rule out a regression reintroducing a
// folly::exception_wrapper-only overload set.
BOOST_AUTO_TEST_CASE(property_set_exception_via_std_exception_ptr_overload,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        kythira::Promise<int> promise;
        auto future = promise.getFuture();
        std::string msg = "regenericization-" + std::to_string(i);
        promise.setException(std::make_exception_ptr(std::runtime_error(msg)));
        bool threw = false;
        try {
            future.get();
        } catch (const std::runtime_error& e) {
            threw = true;
            BOOST_CHECK_EQUAL(std::string(e.what()), msg);
        }
        BOOST_CHECK(threw);
    }
}

// Requirement 2.1/2.4: setValue(kythira::unit{}) (the regenericized
// void-specialization signature, replacing folly::Unit{}) is a genuinely
// new overload added during Phase 1 — confirm it fulfills the promise
// exactly like the pre-existing folly::Unit{}/no-arg overloads.
BOOST_AUTO_TEST_CASE(property_set_value_via_unit_overload, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        kythira::Promise<void> promise;
        auto future = promise.getFuture();
        promise.setValue(kythira::unit{});
        BOOST_CHECK(promise.isFulfilled());
        BOOST_CHECK_NO_THROW(future.get());
    }
}

// Runtime behavior sanity check across the full wrapper surface named in
// the static_assert block above, not just the two overloads Phase 1 added
// or touched — thenValue/thenError/via/collectAll, matching this
// property's "with identical runtime behavior" clause.
BOOST_AUTO_TEST_CASE(property_full_wrapper_surface_runtime_behavior_unchanged,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        auto value_future =
            kythira::FutureFactory::makeFuture(i).thenValue([](int v) { return v + 1; });
        BOOST_CHECK_EQUAL(value_future.get(), i + 1);

        auto error_future = kythira::FutureFactory::makeExceptionalFuture<int>(
                                std::make_exception_ptr(std::runtime_error("x")))
                                .thenError([](std::exception_ptr) { return -1; });
        BOOST_CHECK_EQUAL(error_future.get(), -1);

        folly::InlineExecutor inline_executor;
        kythira::Executor executor(&inline_executor);
        int called = 0;
        executor.add([&called] { ++called; });
        BOOST_CHECK_EQUAL(called, 1);
    }

    std::vector<kythira::Future<int>> futures;
    futures.push_back(kythira::FutureFactory::makeFuture(1));
    futures.push_back(kythira::FutureFactory::makeFuture(2));
    auto results = kythira::FutureCollector::collectAll(std::move(futures)).get();
    BOOST_REQUIRE_EQUAL(results.size(), 2U);
    BOOST_CHECK_EQUAL(results[0].value(), 1);
    BOOST_CHECK_EQUAL(results[1].value(), 2);
}

BOOST_AUTO_TEST_SUITE_END()
