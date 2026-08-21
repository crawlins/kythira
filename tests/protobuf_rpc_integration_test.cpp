// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE protobuf_rpc_integration_test
#include <boost/test/unit_test.hpp>

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/protobuf_serializer.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <thread>
#include <chrono>
#include <unordered_map>

// End-to-end integration test proving protobuf_rpc_serializer is a transparent
// drop-in for json_rpc_serializer (Property 5, Requirements 1.2, 10.4): the ONLY
// change from tests/http_integration_test.cpp is the serializer_type alias.
//
// Feature: protobuf-rpc-serializer, Property 5: transparent drop-in for
// json_rpc_serializer.

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint64_t test_node_id = 1;

// The one and only difference from the JSON integration test: serializer_type.
using test_transport_types =
    kythira::http_transport_types<kythira::protobuf_serializer, kythira::noop_metrics,
                                  folly::CPUThreadPoolExecutor>;
}

BOOST_AUTO_TEST_SUITE(protobuf_rpc_integration_tests)

BOOST_AUTO_TEST_CASE(test_full_rpc_exchange, *boost::unit_test::timeout(60)) {
    constexpr std::uint16_t unique_port = 8097;
    constexpr const char* server_url = "http://127.0.0.1:8097";

    kythira::cpp_httplib_server_config server_config;
    server_config.max_concurrent_connections = 10;
    server_config.request_timeout = std::chrono::seconds{5};

    kythira::cpp_httplib_client_config client_config;
    client_config.connection_timeout = std::chrono::milliseconds{1000};
    client_config.request_timeout = std::chrono::milliseconds{2000};

    typename test_transport_types::metrics_type metrics;

    kythira::cpp_httplib_server<test_transport_types> server(test_bind_address, unique_port,
                                                             server_config, metrics);

    bool request_vote_called = false;
    bool append_entries_called = false;
    bool install_snapshot_called = false;

    server.register_request_vote_handler([&](const kythira::request_vote_request<>& req) {
        request_vote_called = true;
        kythira::request_vote_response<> resp;
        resp._term = req.term() + 1;
        resp._vote_granted = true;
        return resp;
    });

    server.register_append_entries_handler([&](const kythira::append_entries_request<>& req) {
        append_entries_called = true;
        // Echo the received entry count back through the response term so the
        // client can confirm the repeated LogEntry field survived the round-trip.
        kythira::append_entries_response<> resp;
        resp._term = req.term();
        resp._success = req.entries().size() == 2;
        return resp;
    });

    server.register_install_snapshot_handler([&](const kythira::install_snapshot_request<>& req) {
        install_snapshot_called = true;
        kythira::install_snapshot_response<> resp;
        // Confirm the binary snapshot data (incl. a 0x00 byte) arrived intact.
        resp._term = (req.data().size() == 4 && req.data()[1] == std::byte{0x00}) ? req.term() : 0;
        return resp;
    });

    server.start();
    BOOST_TEST(server.is_running());
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    try {
        std::unordered_map<std::uint64_t, std::string> node_urls;
        node_urls[test_node_id] = server_url;
        kythira::cpp_httplib_client<test_transport_types> client(std::move(node_urls),
                                                                 client_config, metrics);

        // RequestVote
        {
            kythira::request_vote_request<> request;
            request._term = 5;
            request._candidate_id = 42;
            request._last_log_index = 10;
            request._last_log_term = 4;
            auto response =
                client.send_request_vote(test_node_id, request, std::chrono::milliseconds{1000})
                    .get();
            BOOST_TEST(request_vote_called);
            BOOST_TEST(response.term() == 6);
            BOOST_TEST(response.vote_granted() == true);
        }

        // AppendEntries with two entries (exercises the repeated LogEntry field).
        {
            kythira::append_entries_request<> request;
            request._term = 6;
            request._leader_id = 42;
            request._prev_log_index = 10;
            request._prev_log_term = 5;
            request._leader_commit = 9;
            kythira::log_entry<> e0;
            e0._term = 6;
            e0._index = 11;
            e0._command = {std::byte{'a'}, std::byte{0x00}, std::byte{'b'}};
            e0._type = kythira::entry_type::normal;
            kythira::log_entry<> e1;
            e1._term = 6;
            e1._index = 12;
            e1._command = {};
            e1._type = kythira::entry_type::no_op;
            request._entries = {e0, e1};
            auto response =
                client.send_append_entries(test_node_id, request, std::chrono::milliseconds{1000})
                    .get();
            BOOST_TEST(append_entries_called);
            BOOST_TEST(response.term() == 6);
            BOOST_TEST(response.success() == true);
        }

        // InstallSnapshot with binary data including a 0x00 byte.
        {
            kythira::install_snapshot_request<> request;
            request._term = 7;
            request._leader_id = 42;
            request._last_included_index = 100;
            request._last_included_term = 6;
            request._offset = 0;
            request._data = {std::byte{'t'}, std::byte{0x00}, std::byte{'s'}, std::byte{'t'}};
            request._done = true;
            auto response =
                client.send_install_snapshot(test_node_id, request, std::chrono::milliseconds{1000})
                    .get();
            BOOST_TEST(install_snapshot_called);
            BOOST_TEST(response.term() == 7);
        }
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("Exception during protobuf client-server exchange: " << e.what());
        BOOST_TEST(false, "protobuf drop-in exchange failed");
    }

    server.stop();
    BOOST_TEST(!server.is_running());
}

BOOST_AUTO_TEST_SUITE_END()
