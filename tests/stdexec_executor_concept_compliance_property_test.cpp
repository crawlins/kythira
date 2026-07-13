// **Feature: stdexec-future-backend, Property 4: Executor/Scheduler Shim
// Correctness**
// For any function submitted through scheduler_executor_shim::add(), the
// function should execute exactly once, on the wrapped scheduler's
// execution context, before add() returns.
// **Validates: Requirements 3.2, 3.4**
#define BOOST_TEST_MODULE stdexec_executor_concept_compliance_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_stdexec.hpp>

#include <exec/single_thread_context.hpp>

#include <atomic>
#include <thread>

using namespace kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 200;
}

BOOST_AUTO_TEST_SUITE(stdexec_executor_concept_compliance_tests)

BOOST_AUTO_TEST_CASE(static_assert_concept_compliance, *boost::unit_test::timeout(10)) {
    static_assert(kythira::executor<scheduler_executor_shim>);
    static_assert(kythira::keep_alive<scheduler_executor_shim>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(property_add_runs_function_exactly_once_before_returning,
                     *boost::unit_test::timeout(60)) {
    exec::single_thread_context ctx;
    scheduler_executor_shim shim(ctx.get_scheduler());
    for (int i = 0; i < property_test_iterations; ++i) {
        int calls = 0;
        shim.add([&calls] { ++calls; });
        // add() blocks until func has run (documented shim overhead), so
        // this check is valid immediately after the call returns.
        BOOST_CHECK_EQUAL(calls, 1);
    }
}

BOOST_AUTO_TEST_CASE(add_runs_on_the_wrapped_scheduler_execution_context,
                     *boost::unit_test::timeout(10)) {
    exec::single_thread_context ctx;
    scheduler_executor_shim shim(ctx.get_scheduler());
    std::thread::id observed{};
    shim.add([&observed] { observed = std::this_thread::get_id(); });
    BOOST_CHECK(observed != std::this_thread::get_id());
}

BOOST_AUTO_TEST_CASE(get_returns_a_stable_non_null_identity, *boost::unit_test::timeout(10)) {
    exec::single_thread_context ctx;
    scheduler_executor_shim shim(ctx.get_scheduler());
    BOOST_CHECK(shim.get() != nullptr);
    BOOST_CHECK_EQUAL(shim.get(), shim.get());
}

BOOST_AUTO_TEST_CASE(shim_is_copyable_matching_keep_alive_concept, *boost::unit_test::timeout(10)) {
    exec::single_thread_context ctx;
    scheduler_executor_shim shim(ctx.get_scheduler());
    scheduler_executor_shim copy = shim;  // NOLINT(performance-unnecessary-copy-initialization)
    int calls = 0;
    copy.add([&calls] { ++calls; });
    BOOST_CHECK_EQUAL(calls, 1);
}

BOOST_AUTO_TEST_SUITE_END()
