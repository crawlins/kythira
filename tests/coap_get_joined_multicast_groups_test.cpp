#define BOOST_TEST_MODULE coap_get_joined_multicast_groups_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>
#include <raft/coap_transport.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>  // test_transport_types::executor_type below is folly::Executor directly
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/console_logger.hpp>

#include "test_timeout_scale.hpp"

// Simple test types
struct test_types {
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using rpc_serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<std::vector<std::byte>>;
};

// Every test case here constructs a fresh coap_client, and every
// coap_client construction pays for coap_new_context()'s unconditional
// DTLS/OpenSSL provider initialization (SSL_CTX_new_ex() and friends) even
// though none of these test cases enable DTLS or ever send anything --
// libcoap always sets this up regardless. That's cheap in isolation but
// can get noticeably slower under real CPU contention (many coap_*_test
// binaries doing the same expensive one-time OpenSSL provider scan at
// once via ctest -j), which is what pushed this file's total runtime
// past its previous 30s-per-case budget on a loaded CI runner.
BOOST_AUTO_TEST_CASE(test_get_joined_multicast_groups_empty,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    // Create a CoAP client. This endpoint is a client-only target -- no
    // coap_server exists anywhere in this file -- so a fixed literal well
    // outside the ephemeral range (32768-60999) and distinct from every
    // other coap_*_test.cpp's assigned literal is safe.
    std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coap://127.0.0.1:61120"}};

    kythira::coap_client_config config;
    kythira::noop_metrics metrics;

    kythira::coap_client<test_types> client(endpoints, config, metrics);

    // Initially, no groups should be joined
    auto groups = client.get_joined_multicast_groups();
    BOOST_CHECK_EQUAL(groups.size(), 0);
}

BOOST_AUTO_TEST_CASE(test_get_joined_multicast_groups_after_join,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    // Create a CoAP client
    std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coap://127.0.0.1:61120"}};

    kythira::coap_client_config config;
    kythira::noop_metrics metrics;

    kythira::coap_client<test_types> client(endpoints, config, metrics);

    // Join a multicast group
    const std::string multicast_address = "224.0.1.187";
    bool joined = client.join_multicast_group(multicast_address);
    BOOST_CHECK(joined);

    // Verify the group is in the list
    auto groups = client.get_joined_multicast_groups();
    BOOST_CHECK_EQUAL(groups.size(), 1);
    BOOST_CHECK(std::find(groups.begin(), groups.end(), multicast_address) != groups.end());
}

BOOST_AUTO_TEST_CASE(test_get_joined_multicast_groups_multiple,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    // Create a CoAP client
    std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coap://127.0.0.1:61120"}};

    kythira::coap_client_config config;
    kythira::noop_metrics metrics;

    kythira::coap_client<test_types> client(endpoints, config, metrics);

    // Join multiple multicast groups
    const std::vector<std::string> multicast_addresses = {"224.0.1.187", "224.0.1.188",
                                                          "224.0.1.189"};

    for (const auto& address : multicast_addresses) {
        bool joined = client.join_multicast_group(address);
        BOOST_CHECK(joined);
    }

    // Verify all groups are in the list
    auto groups = client.get_joined_multicast_groups();
    BOOST_CHECK_EQUAL(groups.size(), multicast_addresses.size());

    for (const auto& address : multicast_addresses) {
        BOOST_CHECK(std::find(groups.begin(), groups.end(), address) != groups.end());
    }
}

BOOST_AUTO_TEST_CASE(test_get_joined_multicast_groups_after_leave,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    // Create a CoAP client
    std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coap://127.0.0.1:61120"}};

    kythira::coap_client_config config;
    kythira::noop_metrics metrics;

    kythira::coap_client<test_types> client(endpoints, config, metrics);

    // Join a multicast group
    const std::string multicast_address = "224.0.1.187";
    client.join_multicast_group(multicast_address);

    // Verify the group is in the list
    auto groups_before = client.get_joined_multicast_groups();
    BOOST_CHECK_EQUAL(groups_before.size(), 1);

    // Leave the group
    bool left = client.leave_multicast_group(multicast_address);
    BOOST_CHECK(left);

    // Verify the group is no longer in the list
    auto groups_after = client.get_joined_multicast_groups();
    BOOST_CHECK_EQUAL(groups_after.size(), 0);
    BOOST_CHECK(std::find(groups_after.begin(), groups_after.end(), multicast_address) ==
                groups_after.end());
}
