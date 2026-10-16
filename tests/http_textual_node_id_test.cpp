// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// **Feature: cloud-composite-node-ids, tasks 6.2 and 6.5 (cpp-httplib)**
// cpp_httplib_client/server instantiated on std::string and on a composite
// id: every RPC routed to a textual target, the sender's id intact on
// arrival, the responder's id intact on return, and the not-implemented
// error naming a textual peer. Over TLS the client still binds the
// connection to the URL's host, whatever the target's id type.
// **Validates: Requirements 9.2, 14.5**
#define BOOST_TEST_MODULE http_textual_node_id_test
#include <boost/test/unit_test.hpp>

#include <raft/executor_default.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/network.hpp>

#include "http_textual_node_id_rpc_checks.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace {

using namespace kythira::testing::textual_ids;

using transport_types =
    kythira::http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                  kythira::executor_default>;

template<typename NodeId> using client_t = kythira::cpp_httplib_client<transport_types, NodeId>;
template<typename NodeId> using server_t = kythira::cpp_httplib_server<transport_types, NodeId>;

// The old names are the numeric instantiations.
static_assert(
    std::is_same_v<kythira::cpp_httplib_client<transport_types>, client_t<std::uint64_t>>);
static_assert(
    std::is_same_v<kythira::cpp_httplib_server<transport_types>, server_t<std::uint64_t>>);
static_assert(kythira::network_client<kythira::cpp_httplib_client<transport_types>>);

static_assert(kythira::network_client<client_t<std::string>, std::string>);
static_assert(kythira::network_client_with_pre_vote<client_t<std::string>, std::string>);
static_assert(kythira::network_client_with_timeout_now<client_t<std::string>, std::string>);
static_assert(kythira::network_client_with_log_fetch<client_t<std::string>, std::string>);
static_assert(kythira::network_server<server_t<std::string>, std::string>);
static_assert(kythira::network_server_with_pre_vote<server_t<std::string>, std::string>);
static_assert(kythira::network_server_with_log_fetch<server_t<std::string>, std::string>);
static_assert(kythira::network_client_with_log_fetch<client_t<kythira::aws_ec2_node_id>,
                                                     kythira::aws_ec2_node_id>);
static_assert(kythira::network_server_with_log_fetch<server_t<kythira::aws_ec2_node_id>,
                                                     kythira::aws_ec2_node_id>);

template<typename NodeId>
auto every_rpc_round_trips(const NodeId& server_id, const NodeId& client_id) -> void {
    seen_senders<NodeId> seen;
    server_t<NodeId> server("127.0.0.1", 0, {}, kythira::noop_metrics{});
    register_recording_handlers(server, server_id, seen);
    server.start();
    std::unordered_map<NodeId, std::string> urls{
        {server_id, std::format("http://127.0.0.1:{}", server.bound_port())}};
    client_t<NodeId> client(urls, {}, kythira::noop_metrics{});
    send_every_rpc(client, server_id, client_id, seen);
    server.stop();
}

}  // namespace

BOOST_AUTO_TEST_CASE(string_target_carries_every_rpc, *boost::unit_test::timeout(60)) {
    every_rpc_round_trips(string_server, string_client);
}

BOOST_AUTO_TEST_CASE(composite_target_carries_every_rpc, *boost::unit_test::timeout(60)) {
    every_rpc_round_trips(aws_server(), aws_client());
}

// An unregistered extension answers 501, which the client reports as the
// not-implemented error; for a textual peer it names the peer by its text.
BOOST_AUTO_TEST_CASE(not_implemented_names_a_textual_peer, *boost::unit_test::timeout(60)) {
    server_t<std::string> server("127.0.0.1", 0, {}, kythira::noop_metrics{});
    server.start();
    std::unordered_map<std::string, std::string> urls{
        {string_server, std::format("http://127.0.0.1:{}", server.bound_port())}};
    client_t<std::string> client(urls, {}, kythira::noop_metrics{});
    bool reported = false;
    try {
        static_cast<void>(
            client
                .send_timeout_now(string_server,
                                  kythira::timeout_now_request<std::string>{1, string_client, 0},
                                  rpc_timeout)
                .get());
    } catch (const kythira::rpc_not_implemented_exception& e) {
        reported = true;
        BOOST_TEST(e.target_text() == string_server);
        BOOST_TEST(std::string(e.what()).find(string_server) != std::string::npos);
    }
    BOOST_TEST(reported);
    server.stop();
}

// A target with no URL is refused, not routed anywhere.
BOOST_AUTO_TEST_CASE(unknown_textual_target_is_refused, *boost::unit_test::timeout(60)) {
    std::unordered_map<std::string, std::string> urls{{string_server, "http://127.0.0.1:1"}};
    client_t<std::string> client(urls, {}, kythira::noop_metrics{});
    bool refused = false;
    try {
        static_cast<void>(
            client
                .send_request_vote(
                    string_client,
                    kythira::request_vote_request<std::string>{1, string_server, 0, 0}, rpc_timeout)
                .get());
    } catch (const std::exception&) {
        // send_rpc's catch-all hands the future a sliced std::exception, so
        // the "No URL mapping found for node <id>" text does not survive to
        // here; that the call fails rather than dialling anything is the
        // property.
        refused = true;
    }
    BOOST_TEST(refused);
}

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT

namespace {

// RequestVote to a textual target over HTTPS, with `files` as the server's
// certificate. True iff the vote came back.
auto vote_over_tls(server_tls_files& files) -> bool {
    kythira::cpp_httplib_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = files.cert_path;
    server_config.ssl_key_path = files.key_path;
    server_config.ca_cert_path = files.ca_path;
    server_t<std::string> server("127.0.0.1", 0, server_config, kythira::noop_metrics{});
    server.register_request_vote_handler([](const kythira::request_vote_request<std::string>& req) {
        return kythira::request_vote_response<>{req.term(), req.candidate_id() == string_client};
    });
    server.start();

    kythira::cpp_httplib_client_config client_config;
    client_config.ca_cert_path = files.ca_path;
    client_config.enable_ssl_verification = true;
    std::unordered_map<std::string, std::string> urls{
        {string_server, std::format("https://127.0.0.1:{}", server.bound_port())}};
    client_t<std::string> client(urls, client_config, kythira::noop_metrics{});
    bool granted = false;
    try {
        granted =
            client
                .send_request_vote(
                    string_server,
                    kythira::request_vote_request<std::string>{3, string_client, 0, 0}, rpc_timeout)
                .get()
                .vote_granted();
    } catch (const std::exception&) {
        granted = false;
    }
    server.stop();
    return granted;
}

}  // namespace

BOOST_AUTO_TEST_CASE(tls_textual_target_reached_by_its_host, *boost::unit_test::timeout(60)) {
    server_tls_files files({"127.0.0.1"}, {raft::testing::peer_identity_name(string_server)});
    BOOST_TEST(vote_over_tls(files));
}

// The certificate names the target's node id but not the address dialled:
// the client refuses it before any request is written.
BOOST_AUTO_TEST_CASE(tls_node_id_san_alone_does_not_name_the_host, *boost::unit_test::timeout(60)) {
    server_tls_files files({}, {raft::testing::peer_identity_name(string_server)});
    BOOST_TEST(!vote_over_tls(files));
}

// The certificate names some other address.
BOOST_AUTO_TEST_CASE(tls_wrong_host_is_refused, *boost::unit_test::timeout(60)) {
    server_tls_files files({"127.0.0.2"}, {raft::testing::peer_identity_name(string_server)});
    BOOST_TEST(!vote_over_tls(files));
}

#endif  // CPPHTTPLIB_OPENSSL_SUPPORT
