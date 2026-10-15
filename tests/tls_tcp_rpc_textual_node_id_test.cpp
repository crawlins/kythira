// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// **Feature: cloud-composite-node-ids, tasks 6.2, 6.3 and 6.5 (TLS-TCP)**
// The mutual-TLS RPC transport with textual node ids: peers are named by
// std::string and composite ids, their CA-issued certificates carry the
// "urn:kythira:node:<id>" URI SAN, and the trust policy binds each
// connection to the id that SAN names.
// **Validates: Requirements 9.1, 12.2**
#define BOOST_TEST_MODULE tls_tcp_rpc_textual_node_id_test
#include <boost/test/unit_test.hpp>

#include <raft/ca_http_helpers.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/composite_node_id.hpp>
#include <raft/tls_tcp_rpc.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>

using namespace raft::testing;

namespace {

constexpr auto k_rpc_timeout = std::chrono::milliseconds(5000);

auto find_free_port() -> std::uint16_t {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    auto port = static_cast<std::uint16_t>(ntohs(addr.sin_port));
    ::close(fd);
    return port;
}

// A CA-issued certificate and key on disk, where the transport loads them.
struct peer_files {
    std::filesystem::path dir;
    std::string cert_path;
    std::string key_path;

    peer_files(certificate_authority& ca, leaf_certificate_options opts) {
        opts.server_auth = true;
        opts.client_auth = true;
        auto leaf = ca.issue(std::move(opts));
        dir = std::filesystem::temp_directory_path() /
              ("tls_tcp_rpc_textual_node_id_test_" + std::to_string(::getpid()) + "_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(dir);
        cert_path = (dir / "cert.pem").string();
        key_path = (dir / "key.pem").string();
        std::ofstream(cert_path) << leaf.certificate_pem;
        std::ofstream(key_path) << leaf.private_key_pem;
    }

    ~peer_files() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    peer_files(const peer_files&) = delete;
    auto operator=(const peer_files&) -> peer_files& = delete;
    peer_files(peer_files&&) = delete;
    auto operator=(peer_files&&) -> peer_files& = delete;
};

template<typename NodeId> auto peer_opts(const NodeId& id) -> leaf_certificate_options {
    leaf_certificate_options opts;
    opts.subject.common_name = "kythira-peer";
    opts.uri_names = {peer_identity_name(id)};
    return opts;
}

// Two peers, `server_id` serving and `client_id` dialling, each with a
// certificate naming its own id, under a policy binding all of `members`.
// `client_cert_id` lets the client present someone else's certificate.
template<typename NodeId> struct two_peers {
    certificate_authority ca;
    peer_files server_files;
    peer_files client_files;
    kythira::basic_tls_rpc_trust_policy<NodeId> policy;
    std::uint16_t port{find_free_port()};
    kythira::basic_tls_tcp_rpc_server<NodeId> server;
    kythira::basic_tls_tcp_rpc_client<NodeId> client;

    two_peers(const NodeId& server_id, const NodeId& client_cert_id,
              const std::vector<NodeId>& members)
        : server_files(ca, peer_opts(server_id)),
          client_files(ca, peer_opts(client_cert_id)),
          policy(binding(ca, members)),
          server(port, {server_files.cert_path, server_files.key_path, policy}),
          client({client_files.cert_path, client_files.key_path, policy}) {}

    static auto binding(certificate_authority& ca, const std::vector<NodeId>& members)
        -> kythira::basic_tls_rpc_trust_policy<NodeId> {
        std::map<std::string, NodeId> names;
        for (const auto& id : members) {
            names.emplace(peer_identity_name(id), id);
        }
        return kythira::ca_root_only<NodeId>(ca.root_certificate_pem())
            .binding_peer_node_ids(std::move(names));
    }

    void start() {
        server.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~two_peers() { server.stop(); }

    two_peers(const two_peers&) = delete;
    auto operator=(const two_peers&) -> two_peers& = delete;
    two_peers(two_peers&&) = delete;
    auto operator=(two_peers&&) -> two_peers& = delete;
};

// RequestVote from `claimed_candidate`, dialled as `target`. True iff the
// vote came back granted.
template<typename NodeId>
auto vote(two_peers<NodeId>& peers, const NodeId& target, const NodeId& claimed_candidate) -> bool {
    peers.server.register_request_vote_handler(
        [](const kythira::request_vote_request<NodeId>& req) {
            return kythira::request_vote_response<>{req.term(), true};
        });
    peers.start();
    peers.client.add_peer(target, "127.0.0.1", peers.port);
    kythira::request_vote_request<NodeId> req{7, claimed_candidate, 0, 0};
    try {
        return peers.client.send_request_vote(target, req, k_rpc_timeout).get().vote_granted();
    } catch (const std::exception&) {
        return false;
    }
}

const std::string a = "docker:cluster-1:node-a";
const std::string b = "docker:cluster-1:node-b";
const std::string c = "docker:cluster-1:node-c";

}  // namespace

BOOST_AUTO_TEST_CASE(string_ids_round_trip, *boost::unit_test::timeout(60)) {
    two_peers<std::string> peers(a, b, {a, b, c});
    BOOST_TEST(vote(peers, a, b));
}

// The server drops an RPC whose claimed sender is not the node the client's
// certificate names.
BOOST_AUTO_TEST_CASE(string_peer_may_speak_only_as_itself, *boost::unit_test::timeout(60)) {
    two_peers<std::string> peers(a, b, {a, b, c});
    BOOST_TEST(!vote(peers, a, c));
}

// The client refuses a server that is not the node it dialled.
BOOST_AUTO_TEST_CASE(string_client_refuses_the_wrong_server, *boost::unit_test::timeout(60)) {
    two_peers<std::string> peers(a, b, {a, b, c});
    BOOST_TEST(!vote(peers, c, b));
}

// A certificate naming an id outside the cluster is not a peer.
BOOST_AUTO_TEST_CASE(string_non_member_is_refused, *boost::unit_test::timeout(60)) {
    const std::string outsider = "docker:cluster-2:node-a";
    two_peers<std::string> peers(a, outsider, {a, b, c});
    BOOST_TEST(!vote(peers, a, outsider));
}

// The remaining RPCs carry the textual id through the JSON wire format too.
BOOST_AUTO_TEST_CASE(string_ids_carry_every_rpc, *boost::unit_test::timeout(60)) {
    two_peers<std::string> peers(a, b, {a, b, c});
    std::string seen_leader;
    std::string seen_requester;
    std::string seen_peer;
    peers.server.register_append_entries_handler(
        [&](const kythira::append_entries_request<std::string>& req) {
            seen_leader = req.leader_id();
            return kythira::append_entries_response<>{req.term(), true};
        });
    peers.server.register_fetch_log_entries_handler(
        [&](const kythira::fetch_log_entries_request<std::string>& req) {
            seen_requester = req.requester_id();
            return kythira::fetch_log_entries_response<std::string>{a, true, 0, {}};
        });
    peers.server.set_peer_seen_handler(
        [&](const std::string& id, const sockaddr_storage&, socklen_t) { seen_peer = id; });
    peers.start();
    peers.client.add_peer(a, "127.0.0.1", peers.port);

    kythira::append_entries_request<std::string> ae{3, b, 0, 0, {}, 0};
    BOOST_TEST(peers.client.send_append_entries(a, ae, k_rpc_timeout).get().success());
    BOOST_TEST(seen_leader == b);
    BOOST_TEST(seen_peer == b);

    kythira::fetch_log_entries_request<std::string> fl{b, 1, 1};
    auto resp = peers.client.send_fetch_log_entries(a, fl, k_rpc_timeout).get();
    BOOST_TEST(resp.available());
    BOOST_TEST(resp.responder_id() == a);
    BOOST_TEST(seen_requester == b);
}

// A composite id: canonical text in the URI SAN, the id type end to end.
BOOST_AUTO_TEST_CASE(composite_ids_round_trip, *boost::unit_test::timeout(60)) {
    using id_t = kythira::aws_ec2_node_id;
    auto ida = id_t::parse("aws-ec2:us-east-1:i-0123456789abcdef0").value();
    auto idb = id_t::parse("aws-ec2:us-east-1:i-0fedcba9876543210").value();
    auto idc = id_t::parse("aws-ec2:us-west-2:i-0123456789abcdef0").value();
    {
        two_peers<id_t> peers(ida, idb, {ida, idb, idc});
        BOOST_TEST(vote(peers, ida, idb));
    }
    {
        // Same instance id in another region: a different node.
        two_peers<id_t> peers(ida, idb, {ida, idb, idc});
        BOOST_TEST(!vote(peers, ida, idc));
    }
}

// The numeric transport is unchanged: DNS SAN peer names, and a URI SAN
// carrying the same id's text is not a peer.
BOOST_AUTO_TEST_CASE(numeric_peers_still_use_dns_names, *boost::unit_test::timeout(60)) {
    certificate_authority ca;
    leaf_certificate_options server_opts;
    server_opts.dns_names = {peer_identity_dns_name(1)};
    peer_files server_files(ca, server_opts);
    leaf_certificate_options uri_opts;
    uri_opts.uri_names = {peer_identity_uri("2")};
    peer_files uri_files(ca, uri_opts);

    auto policy = kythira::ca_root_only(ca.root_certificate_pem())
                      .binding_peer_node_ids({{peer_identity_dns_name(1), 1},
                                              {peer_identity_dns_name(2), 2},
                                              {peer_identity_uri("2"), 2}});
    auto port = find_free_port();
    kythira::tls_tcp_rpc_server server(port,
                                       {server_files.cert_path, server_files.key_path, policy});
    server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{req.term(), true};
    });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    kythira::tls_tcp_rpc_client client({uri_files.cert_path, uri_files.key_path, policy});
    client.add_peer(1, "127.0.0.1", port);
    bool granted = false;
    try {
        granted =
            client.send_request_vote(1, kythira::request_vote_request<>{7, 2, 0, 0}, k_rpc_timeout)
                .get()
                .vote_granted();
    } catch (const std::exception&) {
        granted = false;
    }
    server.stop();
    BOOST_TEST(!granted);
}
