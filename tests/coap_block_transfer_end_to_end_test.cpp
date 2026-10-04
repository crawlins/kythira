// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Block-wise transfer over a real libcoap socket (coap-transport Requirement
 * 12.1/12.3).
 *
 * Every other block test in the tree exercises kythira::block_option or the
 * split/reassemble helpers in isolation; none of them ever put more than one
 * block on the wire. That is how the Block1 option shipped written as the
 * host-order bytes of a uint32_t: on a little-endian box block 0 with
 * SZX=6 went out as 06 00 00 00, which the receiver decodes as block number
 * 0x60000 -- and nothing noticed, because nothing ever sent a request larger
 * than one block between a real client and a real server.
 *
 * These cases do exactly that: a real coap_server bound to localhost, a real
 * coap_client pointed at it, and InstallSnapshot payloads sized to need
 * anything from one block to dozens. The server handler compares the bytes it
 * receives against what was sent, so a reassembly that drops, duplicates or
 * reorders a block fails here rather than corrupting a snapshot in a cluster.
 */
#define BOOST_TEST_MODULE coap_block_transfer_end_to_end_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/serializer_registry.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_timeout_scale.hpp"

using namespace kythira;

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint64_t test_node_id = 1;
// Scaled for the same reason coap_cbor_end_to_end_test scales its own: this
// is handed into the code under test as the RPC deadline, which a per-case
// timeout() decorator never reaches.
constexpr auto test_timeout = kythira::testing::scaled_deadline(10000);

using test_serializer = kythira::cbor_rpc_serializer<std::vector<std::byte>>;

struct test_transport_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = int;  // named, never invoked; see coap_conformance_types.hpp

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};

// A payload whose every byte depends on its position, so a dropped,
// duplicated or reordered block changes the content and not just the length.
auto make_payload(std::size_t size) -> std::vector<std::byte> {
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>((i * 31U + (i >> 8U)) & 0xFFU);
    }
    return data;
}

// Sends one InstallSnapshot of `size` bytes and checks the server saw exactly
// those bytes.
auto round_trip_snapshot(std::size_t size, std::size_t max_block_size) -> void {
    BOOST_TEST_CONTEXT("snapshot size " << size << ", max_block_size " << max_block_size) {
        const auto sent = make_payload(size);

        kythira::coap_server_config server_config;
        server_config.enable_dtls = false;
        server_config.max_block_size = max_block_size;
        // Above the largest payload any case sends, so the size limit is not
        // what is being tested here.
        server_config.max_request_size = 1024 * 1024;
        kythira::noop_metrics server_metrics;
        coap_server<test_transport_types> server(test_bind_address, 0, server_config,
                                                 server_metrics);

        std::atomic<bool> handler_called{false};
        std::atomic<bool> payload_matched{false};
        std::atomic<std::size_t> received_size{0};
        server.register_install_snapshot_handler([&](const kythira::install_snapshot_request<>& req)
                                                     -> kythira::install_snapshot_response<> {
            received_size = req.data().size();
            payload_matched = (req.data() == sent);
            handler_called = true;
            return kythira::install_snapshot_response<>{req.term()};
        });
        server.start();

        kythira::coap_client_config client_config;
        client_config.enable_dtls = false;
        client_config.max_block_size = max_block_size;
        std::unordered_map<std::uint64_t, std::string> endpoints;
        endpoints[test_node_id] =
            std::format("coap://{}:{}", test_bind_address, server.bound_port());
        kythira::noop_metrics client_metrics;
        coap_client<test_transport_types> client(std::move(endpoints), client_config,
                                                 client_metrics);

        kythira::install_snapshot_request<> request{9, 1, 100, 8, 0, sent, true};
        auto future = client.send_install_snapshot(test_node_id, request, test_timeout);

        BOOST_REQUIRE(future.wait(test_timeout));
        auto response = std::move(future).get();
        BOOST_TEST(response.term() == 9U);
        BOOST_TEST(handler_called.load());
        BOOST_TEST(received_size.load() == size);
        BOOST_TEST(payload_matched.load());

        server.stop();
    }
}
}  // namespace

BOOST_AUTO_TEST_SUITE(coap_block_transfer_end_to_end_tests)

// One block: the baseline, which already worked before block-wise transfer
// did. If this fails the problem is not in block handling.
BOOST_AUTO_TEST_CASE(single_block_snapshot_round_trips,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    round_trip_snapshot(200, 1024);
}

// Just over one block, so the request needs exactly a second Block1.
BOOST_AUTO_TEST_CASE(two_block_snapshot_round_trips,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    round_trip_snapshot(1500, 1024);
}

// Many blocks, at a block size other than the default, so the SZX field on
// the wire has to agree with the configured size on both ends.
BOOST_AUTO_TEST_CASE(many_block_snapshot_round_trips,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    round_trip_snapshot(40 * 1024, 512);
}

// Sizes on and around block boundaries, where an off-by-one in the "more
// blocks" bit or the final block's length shows up.
BOOST_AUTO_TEST_CASE(boundary_sized_snapshots_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    for (std::size_t size : {1023U, 1024U, 1025U, 2048U, 4095U, 8192U}) {
        round_trip_snapshot(size, 1024);
    }
}

// Several large requests back to back on one client, so per-transfer state
// left behind by one transfer cannot be mistaken for the next one's.
BOOST_AUTO_TEST_CASE(consecutive_multi_block_requests_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    kythira::coap_server_config server_config;
    server_config.enable_dtls = false;
    server_config.max_request_size = 1024 * 1024;
    kythira::noop_metrics server_metrics;
    coap_server<test_transport_types> server(test_bind_address, 0, server_config, server_metrics);

    std::atomic<std::size_t> matched{0};
    std::vector<std::vector<std::byte>> payloads;
    for (std::size_t i = 0; i < 4; ++i) {
        auto payload = make_payload(5000 + i * 777);
        payload[0] = static_cast<std::byte>(i);  // distinct first block per request
        payloads.push_back(std::move(payload));
    }
    server.register_install_snapshot_handler([&](const kythira::install_snapshot_request<>& req)
                                                 -> kythira::install_snapshot_response<> {
        if (req.term() < payloads.size() && req.data() == payloads[req.term()]) {
            ++matched;
        }
        return kythira::install_snapshot_response<>{req.term()};
    });
    server.start();

    kythira::coap_client_config client_config;
    client_config.enable_dtls = false;
    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[test_node_id] = std::format("coap://{}:{}", test_bind_address, server.bound_port());
    kythira::noop_metrics client_metrics;
    coap_client<test_transport_types> client(std::move(endpoints), client_config, client_metrics);

    for (std::size_t i = 0; i < payloads.size(); ++i) {
        kythira::install_snapshot_request<> request{i, 1, 100, 8, 0, payloads[i], true};
        auto future = client.send_install_snapshot(test_node_id, request, test_timeout);
        BOOST_REQUIRE(future.wait(test_timeout));
        BOOST_TEST(std::move(future).get().term() == i);
    }
    BOOST_TEST(matched.load() == payloads.size());

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
