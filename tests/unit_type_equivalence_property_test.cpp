// **Feature: stdexec-future-backend, Property 3: Unit Type Equivalence**
// For any void-valued operation, results obtained through kythira::unit on
// either backend should be observably equivalent to the pre-existing
// folly::Unit-based behavior of the Folly backend.
// **Validates: Requirements 2.1, 2.2, 2.3, 2.4**
#define BOOST_TEST_MODULE unit_type_equivalence_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future.hpp>
#include <raft/future_stdexec.hpp>

#include <folly/Unit.h>

namespace {
constexpr int property_test_iterations = 200;
}

BOOST_AUTO_TEST_SUITE(unit_type_equivalence_tests)

// kythira::unit itself: default-constructible, equality-comparable,
// trivial — the properties every void-stand-in call site relies on
// (design.md's Data Models section).
BOOST_AUTO_TEST_CASE(unit_is_default_constructible_and_equality_comparable,
                     *boost::unit_test::timeout(10)) {
    static_assert(std::is_default_constructible_v<kythira::unit>);
    static_assert(std::is_trivially_copyable_v<kythira::unit>);
    kythira::unit a;
    kythira::unit b;
    BOOST_CHECK(a == b);
}

// Requirement 2.1/2.4: the regenericized void-specialization
// setValue(kythira::unit{}) fulfills a Promise<void> exactly like the
// pre-existing folly::Unit{}-based path did — same isFulfilled()/get()
// observable outcome.
BOOST_AUTO_TEST_CASE(property_folly_backend_unit_setValue_matches_pre_existing_behavior,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        // Pre-existing path: folly::Unit{}-shaped completion, reached via
        // the concept-required no-arg setValue() the Folly wrapper already
        // exposed before this spec.
        kythira::Promise<void> legacy_promise;
        auto legacy_future = legacy_promise.getFuture();
        legacy_promise.setValue();
        BOOST_CHECK(legacy_promise.isFulfilled());
        BOOST_CHECK_NO_THROW(legacy_future.get());

        // New path: the regenericized concept's setValue(kythira::unit{})
        // overload, added during Phase 1.
        kythira::Promise<void> unit_promise;
        auto unit_future = unit_promise.getFuture();
        unit_promise.setValue(kythira::unit{});
        BOOST_CHECK(unit_promise.isFulfilled());
        BOOST_CHECK_NO_THROW(unit_future.get());
    }
}

// makeReadyFuture() returns Future<folly::Unit> on the Folly backend
// (pre-existing) — confirm this is unaffected by kythira::unit's
// introduction, since the two are deliberately distinct types
// (design.md: "not std::monostate ... a named kythira::unit ... without
// touching std::monostate's shared meaning elsewhere"), not aliases of
// each other.
BOOST_AUTO_TEST_CASE(folly_backend_make_ready_future_unaffected_by_kythira_unit,
                     *boost::unit_test::timeout(10)) {
    auto future = kythira::FutureFactory::makeReadyFuture();
    static_assert(std::is_same_v<decltype(future), kythira::Future<folly::Unit>>);
    BOOST_CHECK(future.isReady());
    BOOST_CHECK_NO_THROW(future.get());
}

// stdexec backend cross-check: kythira::stdexec_backend::Future<void> is
// internally backed by kythira::unit throughout (never stdexec's own
// zero-arg set_value_t() convention — see into_try's doc comment in
// future_stdexec.hpp) and observably matches the Folly backend's
// void-future behavior for the same logical operation.
BOOST_AUTO_TEST_CASE(property_stdexec_backend_unit_matches_folly_void_behavior,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        auto folly_future = kythira::FutureFactory::makeFuture();
        bool folly_threw = false;
        try {
            folly_future.get();
        } catch (...) {
            folly_threw = true;
        }

        auto stdexec_future = kythira::stdexec_backend::FutureFactory::makeFuture();
        bool stdexec_threw = false;
        try {
            std::move(stdexec_future).get();
        } catch (...) {
            stdexec_threw = true;
        }

        BOOST_CHECK_EQUAL(folly_threw, stdexec_threw);
        BOOST_CHECK(!folly_threw);
    }

    // makeReadyFuture() on the stdexec backend returns
    // Future<kythira::unit> directly (no folly::Unit involved at all) —
    // still observably "succeeds with no value to inspect," matching the
    // Folly backend's Future<folly::Unit> for the same call.
    auto stdexec_ready = kythira::stdexec_backend::FutureFactory::makeReadyFuture();
    static_assert(
        std::is_same_v<decltype(stdexec_ready), kythira::stdexec_backend::Future<kythira::unit>>);
    BOOST_CHECK(stdexec_ready.isReady());
    BOOST_CHECK(std::move(stdexec_ready).get() == kythira::unit{});
}

BOOST_AUTO_TEST_SUITE_END()
