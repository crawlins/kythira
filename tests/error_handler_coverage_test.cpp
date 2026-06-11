#define BOOST_TEST_MODULE error_handler_coverage_test
#include <boost/test/unit_test.hpp>
#include <folly/init/Init.h>

#include <raft/error_handler.hpp>
#include <raft/future.hpp>
#include <raft/types.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("error_handler_coverage_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);

BOOST_AUTO_TEST_SUITE(classify_error_suite)

BOOST_AUTO_TEST_CASE(timeout_network_delay) {
    kythira::error_handler<int> h;
    // Message must contain a timeout keyword AND a delay/slow keyword
    auto r = h.classify_error(std::runtime_error("timed out slow response delay"));
    BOOST_CHECK(r.type == kythira::error_type::network_timeout);
    BOOST_CHECK(r.should_retry);
    BOOST_REQUIRE(r.timeout_classification.has_value());
    BOOST_CHECK(
        r.timeout_classification.value() ==
        kythira::timeout_type::network_delay);  // NOLINT(bugprone-unchecked-optional-access)
}

BOOST_AUTO_TEST_CASE(timeout_connection_failure) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("timed out connection dropped"));
    BOOST_CHECK(r.type == kythira::error_type::network_timeout);
    BOOST_REQUIRE(r.timeout_classification.has_value());
    BOOST_CHECK(
        r.timeout_classification.value() ==
        kythira::timeout_type::connection_failure);  // NOLINT(bugprone-unchecked-optional-access)
}

BOOST_AUTO_TEST_CASE(timeout_serialization) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("timed out deserialization"));
    BOOST_CHECK(r.type == kythira::error_type::network_timeout);
    BOOST_REQUIRE(r.timeout_classification.has_value());
    BOOST_CHECK(r.timeout_classification.value() ==
                kythira::timeout_type::
                    serialization_timeout);  // NOLINT(bugprone-unchecked-optional-access)
}

BOOST_AUTO_TEST_CASE(timeout_no_response) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("rpc timeout no response"));
    BOOST_CHECK(r.type == kythira::error_type::network_timeout);
    BOOST_REQUIRE(r.timeout_classification.has_value());
    BOOST_CHECK(
        r.timeout_classification.value() ==
        kythira::timeout_type::network_timeout);  // NOLINT(bugprone-unchecked-optional-access)
}

BOOST_AUTO_TEST_CASE(timeout_timed_out_keyword) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("operation timed out"));
    BOOST_CHECK(r.type == kythira::error_type::network_timeout);
    BOOST_CHECK(r.should_retry);
}

BOOST_AUTO_TEST_CASE(timeout_time_out_space) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("time out occurred"));
    BOOST_CHECK(r.type == kythira::error_type::network_timeout);
}

BOOST_AUTO_TEST_CASE(timeout_excluded_config_context) {
    kythira::error_handler<int> h;
    // "set timeout" should be excluded from timeout classification
    auto r = h.classify_error(std::runtime_error("please set timeout value"));
    BOOST_CHECK(r.type != kythira::error_type::network_timeout);
}

BOOST_AUTO_TEST_CASE(network_unreachable) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("network is unreachable"));
    BOOST_CHECK(r.type == kythira::error_type::network_unreachable);
    BOOST_CHECK(r.should_retry);
}

BOOST_AUTO_TEST_CASE(no_route_to_host) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("no route to host"));
    BOOST_CHECK(r.type == kythira::error_type::network_unreachable);
}

BOOST_AUTO_TEST_CASE(connection_refused) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("connection refused by peer"));
    BOOST_CHECK(r.type == kythira::error_type::connection_refused);
    BOOST_CHECK(r.should_retry);
}

BOOST_AUTO_TEST_CASE(serialization_error) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("serialization failed"));
    BOOST_CHECK(r.type == kythira::error_type::serialization_error);
    BOOST_CHECK(!r.should_retry);
}

BOOST_AUTO_TEST_CASE(deserialization_error) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("deserialization error"));
    BOOST_CHECK(r.type == kythira::error_type::serialization_error);
}

BOOST_AUTO_TEST_CASE(parse_error) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("parse error in message"));
    BOOST_CHECK(r.type == kythira::error_type::serialization_error);
}

BOOST_AUTO_TEST_CASE(invalid_format) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("invalid format"));
    BOOST_CHECK(r.type == kythira::error_type::serialization_error);
}

BOOST_AUTO_TEST_CASE(data_corruption) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("checksum mismatch"));
    BOOST_CHECK(r.type == kythira::error_type::serialization_error);
    BOOST_CHECK(!r.should_retry);
}

BOOST_AUTO_TEST_CASE(validation_failed) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("validation failed"));
    BOOST_CHECK(r.type == kythira::error_type::serialization_error);
}

BOOST_AUTO_TEST_CASE(corruption_keyword) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("data corruption detected"));
    BOOST_CHECK(r.type == kythira::error_type::serialization_error);
}

BOOST_AUTO_TEST_CASE(invalid_data) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("invalid data received"));
    BOOST_CHECK(r.type == kythira::error_type::serialization_error);
}

BOOST_AUTO_TEST_CASE(protocol_error) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("protocol violation"));
    BOOST_CHECK(r.type == kythira::error_type::protocol_error);
    BOOST_CHECK(!r.should_retry);
}

BOOST_AUTO_TEST_CASE(invalid_term) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("invalid term in response"));
    BOOST_CHECK(r.type == kythira::error_type::protocol_error);
}

BOOST_AUTO_TEST_CASE(malformed_message) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("malformed rpc message"));
    BOOST_CHECK(r.type == kythira::error_type::protocol_error);
}

BOOST_AUTO_TEST_CASE(invalid_request) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("invalid request format"));
    BOOST_CHECK(r.type == kythira::error_type::protocol_error);
}

BOOST_AUTO_TEST_CASE(disk_full) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("disk full"));
    BOOST_CHECK(r.type == kythira::error_type::permanent_failure);
    BOOST_CHECK(!r.should_retry);
}

BOOST_AUTO_TEST_CASE(out_of_memory) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("out of memory"));
    BOOST_CHECK(r.type == kythira::error_type::permanent_failure);
}

BOOST_AUTO_TEST_CASE(no_space_left) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("no space left on device"));
    BOOST_CHECK(r.type == kythira::error_type::permanent_failure);
}

BOOST_AUTO_TEST_CASE(auth_failed) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("authentication failed"));
    BOOST_CHECK(r.type == kythira::error_type::permanent_failure);
}

BOOST_AUTO_TEST_CASE(permission_denied) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("permission denied"));
    BOOST_CHECK(r.type == kythira::error_type::permanent_failure);
}

BOOST_AUTO_TEST_CASE(access_denied) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("access denied"));
    BOOST_CHECK(r.type == kythira::error_type::permanent_failure);
}

BOOST_AUTO_TEST_CASE(unauthorized) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("unauthorized request"));
    BOOST_CHECK(r.type == kythira::error_type::permanent_failure);
}

BOOST_AUTO_TEST_CASE(forbidden) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("forbidden resource"));
    BOOST_CHECK(r.type == kythira::error_type::permanent_failure);
}

BOOST_AUTO_TEST_CASE(temporary_failure) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("temporary failure occurred"));
    BOOST_CHECK(r.type == kythira::error_type::temporary_failure);
    BOOST_CHECK(r.should_retry);
}

BOOST_AUTO_TEST_CASE(try_again) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("try again later"));
    BOOST_CHECK(r.type == kythira::error_type::temporary_failure);
}

BOOST_AUTO_TEST_CASE(busy_error) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("server is busy"));
    BOOST_CHECK(r.type == kythira::error_type::temporary_failure);
}

BOOST_AUTO_TEST_CASE(unknown_error) {
    kythira::error_handler<int> h;
    auto r = h.classify_error(std::runtime_error("something completely unexpected"));
    BOOST_CHECK(r.type == kythira::error_type::unknown_error);
    BOOST_CHECK(r.should_retry);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(handle_methods_suite)

BOOST_AUTO_TEST_CASE(handle_network_timeout_true) {
    kythira::error_handler<int> h;
    auto e = std::runtime_error("rpc timeout");
    BOOST_CHECK(h.handle_network_timeout(e));
}

BOOST_AUTO_TEST_CASE(handle_network_timeout_false_for_protocol) {
    kythira::error_handler<int> h;
    auto e = std::runtime_error("protocol violation");
    BOOST_CHECK(!h.handle_network_timeout(e));
}

BOOST_AUTO_TEST_CASE(handle_network_error_unreachable) {
    kythira::error_handler<int> h;
    auto e = std::runtime_error("unreachable host");
    BOOST_CHECK(h.handle_network_error(e));
}

BOOST_AUTO_TEST_CASE(handle_network_error_refused) {
    kythira::error_handler<int> h;
    auto e = std::runtime_error("connection refused");
    BOOST_CHECK(h.handle_network_error(e));
}

BOOST_AUTO_TEST_CASE(handle_network_error_temporary) {
    kythira::error_handler<int> h;
    auto e = std::runtime_error("temporary failure");
    BOOST_CHECK(h.handle_network_error(e));
}

BOOST_AUTO_TEST_CASE(handle_network_error_false_for_permanent) {
    kythira::error_handler<int> h;
    auto e = std::runtime_error("disk full");
    BOOST_CHECK(!h.handle_network_error(e));
}

BOOST_AUTO_TEST_CASE(handle_serialization_error_returns_false) {
    kythira::error_handler<int> h;
    // Serialization errors should NOT retry (should_retry=false), so handle_serialization_error
    // returns false
    auto e = std::runtime_error("serialization failed");
    BOOST_CHECK(!h.handle_serialization_error(e));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(detect_partition_suite)

BOOST_AUTO_TEST_CASE(too_few_errors) {
    kythira::error_handler<int> h;
    std::vector<kythira::error_classification> errors = {
        {kythira::error_type::network_timeout, true, "timeout", std::nullopt},
        {kythira::error_type::network_unreachable, true, "unreachable", std::nullopt}};
    BOOST_CHECK(!h.detect_network_partition(errors));
}

BOOST_AUTO_TEST_CASE(majority_network_errors) {
    kythira::error_handler<int> h;
    std::vector<kythira::error_classification> errors = {
        {kythira::error_type::network_timeout, true, "timeout", std::nullopt},
        {kythira::error_type::network_unreachable, true, "unreachable", std::nullopt},
        {kythira::error_type::connection_refused, true, "refused", std::nullopt}};
    BOOST_CHECK(h.detect_network_partition(errors));
}

BOOST_AUTO_TEST_CASE(minority_network_errors) {
    kythira::error_handler<int> h;
    std::vector<kythira::error_classification> errors = {
        {kythira::error_type::network_timeout, true, "timeout", std::nullopt},
        {kythira::error_type::permanent_failure, false, "disk", std::nullopt},
        {kythira::error_type::permanent_failure, false, "disk", std::nullopt}};
    BOOST_CHECK(!h.detect_network_partition(errors));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(retry_policy_suite)

BOOST_AUTO_TEST_CASE(invalid_policy_throws) {
    kythira::error_handler<int> h;
    kythira::error_handler<int>::retry_policy bad_policy{
        .initial_delay = std::chrono::milliseconds{0},  // invalid: must be > 0
        .max_delay = std::chrono::milliseconds{1000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.1,
        .max_attempts = 5};
    BOOST_CHECK_THROW(h.set_retry_policy("test", bad_policy), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(invalid_policy_backoff_too_low) {
    kythira::error_handler<int> h;
    kythira::error_handler<int>::retry_policy bad_policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{1000},
        .backoff_multiplier = 0.5,  // invalid: must be > 1.0
        .jitter_factor = 0.1,
        .max_attempts = 5};
    BOOST_CHECK_THROW(h.set_retry_policy("test", bad_policy), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(get_unknown_policy_returns_default) {
    kythira::error_handler<int> h;
    auto policy = h.get_retry_policy("nonexistent_operation");
    BOOST_CHECK(policy.max_attempts > 0);
    BOOST_CHECK(policy.initial_delay > std::chrono::milliseconds{0});
}

BOOST_AUTO_TEST_CASE(set_and_get_policy) {
    kythira::error_handler<int> h;
    kythira::error_handler<int>::retry_policy p{.initial_delay = std::chrono::milliseconds{200},
                                                .max_delay = std::chrono::milliseconds{4000},
                                                .backoff_multiplier = 3.0,
                                                .jitter_factor = 0.05,
                                                .max_attempts = 7};
    h.set_retry_policy("custom", p);
    auto retrieved = h.get_retry_policy("custom");
    BOOST_CHECK_EQUAL(retrieved.max_attempts, 7u);
    BOOST_CHECK_EQUAL(retrieved.initial_delay.count(), 200);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(execute_with_retry_suite)

BOOST_AUTO_TEST_CASE(success_on_first_attempt, *boost::unit_test::timeout(30)) {
    kythira::error_handler<int> h;
    int call_count = 0;
    auto future = h.execute_with_retry("heartbeat", [&call_count]() -> kythira::Future<int> {
        ++call_count;
        return kythira::FutureFactory::makeFuture<int>(42);
    });
    auto result = std::move(future).get();
    BOOST_CHECK_EQUAL(result, 42);
    BOOST_CHECK_EQUAL(call_count, 1);
}

BOOST_AUTO_TEST_CASE(invalid_policy_returns_exceptional_future, *boost::unit_test::timeout(30)) {
    kythira::error_handler<int> h;
    kythira::error_handler<int>::retry_policy bad{.initial_delay = std::chrono::milliseconds{0},
                                                  .max_delay = std::chrono::milliseconds{1000},
                                                  .backoff_multiplier = 2.0,
                                                  .jitter_factor = 0.1,
                                                  .max_attempts = 5};
    auto future = h.execute_with_retry(
        "test", []() -> kythira::Future<int> { return kythira::FutureFactory::makeFuture<int>(1); },
        bad);
    BOOST_CHECK_THROW(std::move(future).get(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(permanent_failure_no_retry, *boost::unit_test::timeout(30)) {
    kythira::error_handler<int> h;
    // Set a policy with many attempts
    kythira::error_handler<int>::retry_policy p{.initial_delay = std::chrono::milliseconds{10},
                                                .max_delay = std::chrono::milliseconds{100},
                                                .backoff_multiplier = 2.0,
                                                .jitter_factor = 0.0,
                                                .max_attempts = 10};
    h.set_retry_policy("test_permanent", p);

    int call_count = 0;
    auto future = h.execute_with_retry("test_permanent", [&call_count]() -> kythira::Future<int> {
        ++call_count;
        return kythira::FutureFactory::makeExceptionalFuture<int>(std::runtime_error("disk full"));
    });
    BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
    // Permanent failure → should not retry
    BOOST_CHECK_EQUAL(call_count, 1);
}

BOOST_AUTO_TEST_CASE(exhausted_retries, *boost::unit_test::timeout(30)) {
    kythira::error_handler<int> h;
    kythira::error_handler<int>::retry_policy p{.initial_delay = std::chrono::milliseconds{1},
                                                .max_delay = std::chrono::milliseconds{5},
                                                .backoff_multiplier = 1.5,
                                                .jitter_factor = 0.0,
                                                .max_attempts = 2};
    h.set_retry_policy("exhausted_op", p);

    int call_count = 0;
    auto future = h.execute_with_retry("exhausted_op", [&call_count]() -> kythira::Future<int> {
        ++call_count;
        return kythira::FutureFactory::makeExceptionalFuture<int>(
            std::runtime_error("unreachable host"));
    });
    BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
    BOOST_CHECK_EQUAL(call_count, 2);  // initial + 1 retry = 2 attempts
}

BOOST_AUTO_TEST_CASE(serialization_timeout_no_retry, *boost::unit_test::timeout(30)) {
    kythira::error_handler<int> h;
    kythira::error_handler<int>::retry_policy p{.initial_delay = std::chrono::milliseconds{1},
                                                .max_delay = std::chrono::milliseconds{5},
                                                .backoff_multiplier = 1.5,
                                                .jitter_factor = 0.0,
                                                .max_attempts = 10};
    h.set_retry_policy("ser_timeout_op", p);

    int call_count = 0;
    auto future = h.execute_with_retry("ser_timeout_op", [&call_count]() -> kythira::Future<int> {
        ++call_count;
        return kythira::FutureFactory::makeExceptionalFuture<int>(
            std::runtime_error("timed out deserialization"));  // serialization_timeout → no retry
    });
    BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
    BOOST_CHECK_EQUAL(call_count, 1);
}

BOOST_AUTO_TEST_CASE(network_timeout_retries_with_backoff, *boost::unit_test::timeout(30)) {
    kythira::error_handler<int> h;
    kythira::error_handler<int>::retry_policy p{.initial_delay = std::chrono::milliseconds{1},
                                                .max_delay = std::chrono::milliseconds{5},
                                                .backoff_multiplier = 1.5,
                                                .jitter_factor = 0.0,
                                                .max_attempts = 3};
    h.set_retry_policy("net_timeout_op", p);

    int call_count = 0;
    auto future = h.execute_with_retry("net_timeout_op", [&call_count]() -> kythira::Future<int> {
        ++call_count;
        if (call_count < 3) {
            return kythira::FutureFactory::makeExceptionalFuture<int>(
                std::runtime_error("rpc timeout no response"));
        }
        return kythira::FutureFactory::makeFuture<int>(99);
    });
    auto result = std::move(future).get();
    BOOST_CHECK_EQUAL(result, 99);
    BOOST_CHECK_EQUAL(call_count, 3);
}

BOOST_AUTO_TEST_CASE(connection_failure_timeout_retries, *boost::unit_test::timeout(30)) {
    kythira::error_handler<int> h;
    kythira::error_handler<int>::retry_policy p{.initial_delay = std::chrono::milliseconds{1},
                                                .max_delay = std::chrono::milliseconds{5},
                                                .backoff_multiplier = 1.5,
                                                .jitter_factor = 0.0,
                                                .max_attempts = 3};
    h.set_retry_policy("conn_fail_op", p);

    int call_count = 0;
    auto future = h.execute_with_retry("conn_fail_op", [&call_count]() -> kythira::Future<int> {
        ++call_count;
        if (call_count < 2) {
            return kythira::FutureFactory::makeExceptionalFuture<int>(
                std::runtime_error("timeout connection dropped"));
        }
        return kythira::FutureFactory::makeFuture<int>(77);
    });
    auto result = std::move(future).get();
    BOOST_CHECK_EQUAL(result, 77);
}

BOOST_AUTO_TEST_CASE(network_delay_timeout_retries, *boost::unit_test::timeout(30)) {
    kythira::error_handler<int> h;
    kythira::error_handler<int>::retry_policy p{.initial_delay = std::chrono::milliseconds{1},
                                                .max_delay = std::chrono::milliseconds{5},
                                                .backoff_multiplier = 1.5,
                                                .jitter_factor = 0.0,
                                                .max_attempts = 3};
    h.set_retry_policy("delay_op", p);

    int call_count = 0;
    auto future = h.execute_with_retry("delay_op", [&call_count]() -> kythira::Future<int> {
        ++call_count;
        if (call_count < 2) {
            return kythira::FutureFactory::makeExceptionalFuture<int>(
                std::runtime_error("slow delay timeout"));
        }
        return kythira::FutureFactory::makeFuture<int>(55);
    });
    auto result = std::move(future).get();
    BOOST_CHECK_EQUAL(result, 55);
}

BOOST_AUTO_TEST_CASE(unknown_timeout_retries, *boost::unit_test::timeout(30)) {
    kythira::error_handler<int> h;
    kythira::error_handler<int>::retry_policy p{.initial_delay = std::chrono::milliseconds{1},
                                                .max_delay = std::chrono::milliseconds{5},
                                                .backoff_multiplier = 1.5,
                                                .jitter_factor = 0.0,
                                                .max_attempts = 3};
    h.set_retry_policy("unk_timeout_op", p);

    int call_count = 0;
    auto future = h.execute_with_retry("unk_timeout_op", [&call_count]() -> kythira::Future<int> {
        ++call_count;
        if (call_count < 2) {
            // "time_out" keyword → network_timeout classification → unknown_timeout type (no
            // matching sub-keyword)
            return kythira::FutureFactory::makeExceptionalFuture<int>(
                std::runtime_error("time_out error occurred"));
        }
        return kythira::FutureFactory::makeFuture<int>(33);
    });
    auto result = std::move(future).get();
    BOOST_CHECK_EQUAL(result, 33);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(stream_operators_suite)

BOOST_AUTO_TEST_CASE(error_type_stream) {
    std::ostringstream oss;
    oss << kythira::error_type::network_timeout;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::error_type::network_unreachable;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::error_type::connection_refused;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::error_type::serialization_error;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::error_type::protocol_error;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::error_type::temporary_failure;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::error_type::permanent_failure;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::error_type::unknown_error;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << static_cast<kythira::error_type>(999);
    BOOST_CHECK(!oss.str().empty());
}

BOOST_AUTO_TEST_CASE(timeout_type_stream) {
    std::ostringstream oss;
    oss << kythira::timeout_type::network_delay;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::timeout_type::network_timeout;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::timeout_type::connection_failure;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::timeout_type::serialization_timeout;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << kythira::timeout_type::unknown_timeout;
    BOOST_CHECK(!oss.str().empty());
    oss.str("");
    oss << static_cast<kythira::timeout_type>(999);
    BOOST_CHECK(!oss.str().empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(raft_error_handler_suite)

BOOST_AUTO_TEST_CASE(get_append_entries_handler) {
    auto& h = kythira::raft_error_handler<>::get_append_entries_handler();
    auto policy = h.get_retry_policy("append_entries");
    BOOST_CHECK(policy.max_attempts > 0);
}

BOOST_AUTO_TEST_CASE(get_request_vote_handler) {
    auto& h = kythira::raft_error_handler<>::get_request_vote_handler();
    auto policy = h.get_retry_policy("request_vote");
    BOOST_CHECK(policy.max_attempts > 0);
}

BOOST_AUTO_TEST_CASE(get_install_snapshot_handler) {
    auto& h = kythira::raft_error_handler<>::get_install_snapshot_handler();
    auto policy = h.get_retry_policy("install_snapshot");
    BOOST_CHECK(policy.max_attempts > 0);
}

BOOST_AUTO_TEST_CASE(configure_all_handlers) {
    kythira::error_handler<int>::retry_policy p{.initial_delay = std::chrono::milliseconds{50},
                                                .max_delay = std::chrono::milliseconds{500},
                                                .backoff_multiplier = 2.0,
                                                .jitter_factor = 0.0,
                                                .max_attempts = 3};
    // Should not throw
    BOOST_CHECK_NO_THROW(kythira::raft_error_handler<>::configure_all_handlers(p, p, p, p));
}

BOOST_AUTO_TEST_SUITE_END()
