// **Feature: stdexec-future-backend, Property 6: stdexec Try Fidelity**
// For any sender completing with a value, error, or stopped signal,
// into_try should produce a Try<T> whose hasValue()/hasException()/
// value()/exception() reflect that completion exactly, matching Folly
// Try<T> semantics for the equivalent state.
// **Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5**
#define BOOST_TEST_MODULE stdexec_try_concept_compliance_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_stdexec.hpp>

#include <stdexcept>
#include <string>

using namespace kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 200;
}

BOOST_AUTO_TEST_SUITE(stdexec_try_concept_compliance_tests)

BOOST_AUTO_TEST_CASE(static_assert_concept_compliance, *boost::unit_test::timeout(10)) {
    static_assert(kythira::try_type<Try<int>, int>);
    static_assert(kythira::try_type<Try<void>, void>);
    static_assert(kythira::try_type<Try<std::string>, std::string>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(property_into_try_captures_value, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        auto result = into_try<int>(stdexec::just(i));
        BOOST_CHECK(result.hasValue());
        BOOST_CHECK(!result.hasException());
        BOOST_CHECK_EQUAL(result.value(), i);
        BOOST_CHECK(result.exception() == nullptr);
    }
}

BOOST_AUTO_TEST_CASE(property_into_try_captures_error_from_a_throwing_continuation,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        auto sender =
            stdexec::just(i) | stdexec::then([](int) -> int { throw std::runtime_error("boom"); });
        auto result = into_try<int>(std::move(sender));
        BOOST_CHECK(!result.hasValue());
        BOOST_CHECK(result.hasException());
        BOOST_CHECK(result.exception() != nullptr);
        BOOST_CHECK_THROW(result.value(), std::runtime_error);
    }
}

BOOST_AUTO_TEST_CASE(into_try_captures_stopped_as_operation_cancelled,
                     *boost::unit_test::timeout(10)) {
    auto result = into_try<int>(stdexec::just_stopped());
    BOOST_CHECK(!result.hasValue());
    BOOST_CHECK(result.hasException());
    BOOST_CHECK_THROW(result.value(), operation_cancelled);
}

BOOST_AUTO_TEST_CASE(into_try_void_captures_value, *boost::unit_test::timeout(10)) {
    // stdexec's own zero-arg set_value_t() convention (as bare
    // stdexec::just() produces) is never used for the void case in this
    // backend — Requirement 2.3 uses kythira::unit as a real value
    // throughout instead (see Future<void>::get(), which always calls
    // into_try<kythira::unit>(...)), so the sender here produces unit{},
    // not a bare zero-arg completion.
    auto result = into_try<void>(stdexec::just(kythira::unit{}));
    BOOST_CHECK(result.hasValue());
    BOOST_CHECK(!result.hasException());
    BOOST_CHECK_NO_THROW(result.value());
}

BOOST_AUTO_TEST_CASE(try_default_construction_and_exception_ptr_construction,
                     *boost::unit_test::timeout(10)) {
    Try<int> from_value(5);
    BOOST_CHECK(from_value.hasValue());
    BOOST_CHECK_EQUAL(from_value.value(), 5);

    Try<int> from_ex(std::make_exception_ptr(std::runtime_error("e")));
    BOOST_CHECK(from_ex.hasException());
    BOOST_CHECK_THROW(from_ex.value(), std::runtime_error);

    BOOST_CHECK_THROW(Try<int>(std::exception_ptr{}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
