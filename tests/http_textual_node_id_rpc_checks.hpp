// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Shared body of the HTTP transports' textual node id smoke tests
// (.kiro/specs/cloud-composite-node-ids/, tasks 6.2 and 6.5): every RPC an
// HTTP transport carries, sent to a target named by a textual or composite
// id, with the sender's id checked on arrival and the responder's id checked
// on return. cpp-httplib, Boost.Beast and Proxygen each include this and
// supply their own client and server, so the three prove the same thing.

#include <boost/test/unit_test.hpp>

#include <raft/ca_http_helpers.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/composite_node_id.hpp>
#include <raft/types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace kythira::testing::textual_ids {

inline constexpr auto rpc_timeout = std::chrono::milliseconds(5000);

/// Canonical composite text with an escaped ':' (`%3A`) inside a segment, so
/// both characters a URL path or a header would need to treat specially are
/// on the wire.
inline const std::string string_server = "docker:cluster%3A1:node-a";
inline const std::string string_client = "docker:cluster%3A1:node-b";

inline auto aws_server() -> aws_ec2_node_id {
    return aws_ec2_node_id::parse("aws-ec2:us-east-1:i-0123456789abcdef0").value();
}
inline auto aws_client() -> aws_ec2_node_id {
    return aws_ec2_node_id::parse("aws-ec2:us-east-1:i-0fedcba9876543210").value();
}

/// A loopback port nothing is listening on, for the transports that cannot
/// report the port a 0 bind chose.
inline auto find_free_port() -> std::uint16_t {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const auto port = static_cast<std::uint16_t>(ntohs(addr.sin_port));
    ::close(fd);
    return port;
}

/// A CA root and one server certificate issued by it, on disk where the
/// transports load them. `ip_addresses` and `uri_names` are the server
/// certificate's SANs: the HTTP transports bind a connection to the URL's
/// host, so a certificate naming only the target's node id (its URI SAN) is
/// not enough to reach it by address.
struct server_tls_files {
    raft::testing::certificate_authority ca;
    std::filesystem::path dir;
    std::string ca_path;
    std::string cert_path;
    std::string key_path;

    server_tls_files(std::vector<std::string> ip_addresses, std::vector<std::string> uri_names) {
        raft::testing::leaf_certificate_options opts;
        opts.subject.common_name = "kythira-http-peer";
        opts.ip_addresses = std::move(ip_addresses);
        opts.uri_names = std::move(uri_names);
        opts.server_auth = true;
        opts.client_auth = true;
        auto leaf = ca.issue(std::move(opts));
        dir = std::filesystem::temp_directory_path() /
              ("http_textual_node_id_" + std::to_string(::getpid()) + "_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(dir);
        ca_path = (dir / "ca.pem").string();
        cert_path = (dir / "cert.pem").string();
        key_path = (dir / "key.pem").string();
        std::ofstream(ca_path) << ca.root_certificate_pem();
        std::ofstream(cert_path) << leaf.certificate_pem;
        std::ofstream(key_path) << leaf.private_key_pem;
    }

    ~server_tls_files() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    server_tls_files(const server_tls_files&) = delete;
    auto operator=(const server_tls_files&) -> server_tls_files& = delete;
    server_tls_files(server_tls_files&&) = delete;
    auto operator=(server_tls_files&&) -> server_tls_files& = delete;
};

/// The sender id each RPC arrived with, as the server's handlers saw it.
template<typename NodeId> struct seen_senders {
    std::mutex mutex;
    std::optional<NodeId> request_vote;
    std::optional<NodeId> request_pre_vote;
    std::optional<NodeId> timeout_now;
    std::optional<NodeId> append_entries;
    std::optional<NodeId> install_snapshot;
    std::optional<NodeId> fetch_log_entries;
};

/// Registers a handler for every RPC on @p server that records the sender
/// and answers with something derived from the request, so a response that
/// came back unchanged from a default value shows.
template<typename NodeId, typename Server>
auto register_recording_handlers(Server& server, const NodeId& self, seen_senders<NodeId>& seen)
    -> void {
    server.register_request_vote_handler([&seen](const request_vote_request<NodeId>& req) {
        std::lock_guard lock(seen.mutex);
        seen.request_vote = req.candidate_id();
        return request_vote_response<>{req.term(), true};
    });
    server.register_request_pre_vote_handler([&seen](const request_pre_vote_request<NodeId>& req) {
        std::lock_guard lock(seen.mutex);
        seen.request_pre_vote = req.candidate_id();
        return request_pre_vote_response<>{req.term(), true};
    });
    server.register_timeout_now_handler([&seen](const timeout_now_request<NodeId>& req) {
        std::lock_guard lock(seen.mutex);
        seen.timeout_now = req.leader_id();
        return timeout_now_response<>{req.term(), true};
    });
    server.register_append_entries_handler([&seen](const append_entries_request<NodeId>& req) {
        std::lock_guard lock(seen.mutex);
        seen.append_entries = req.leader_id();
        return append_entries_response<>{req.term(), req.entries().size() == 1, {}, {}};
    });
    server.register_install_snapshot_handler([&seen](const install_snapshot_request<NodeId>& req) {
        std::lock_guard lock(seen.mutex);
        seen.install_snapshot = req.leader_id();
        return install_snapshot_response<>{req.term() + req.data().size()};
    });
    server.register_fetch_log_entries_handler(
        [&seen, self](const fetch_log_entries_request<NodeId>& req) {
            std::lock_guard lock(seen.mutex);
            seen.fetch_log_entries = req.requester_id();
            fetch_log_entries_response<NodeId> resp{};
            resp._responder_id = self;
            resp._available = true;
            resp._prev_log_term = 1;
            for (auto idx = req.from_index(); idx <= req.to_index(); ++idx) {
                resp._entries.push_back({2, idx, {static_cast<std::byte>(idx)}});
            }
            return resp;
        });
}

/// Sends every RPC from @p self to @p target over @p client and checks both
/// directions: the sender id the server saw and what came back.
template<typename NodeId, typename Client>
auto send_every_rpc(Client& client, const NodeId& target, const NodeId& self,
                    seen_senders<NodeId>& seen) -> void {
    auto vote =
        client.send_request_vote(target, request_vote_request<NodeId>{5, self, 3, 2}, rpc_timeout);
    auto vote_resp = std::move(vote).get();
    BOOST_TEST(vote_resp.term() == 5U);
    BOOST_TEST(vote_resp.vote_granted());

    auto pre = client.send_request_pre_vote(target, request_pre_vote_request<NodeId>{6, self, 3, 2},
                                            rpc_timeout);
    auto pre_resp = std::move(pre).get();
    BOOST_TEST(pre_resp.term() == 6U);
    BOOST_TEST(pre_resp.vote_granted());

    auto tn = client.send_timeout_now(target, timeout_now_request<NodeId>{7, self, 9}, rpc_timeout);
    auto tn_resp = std::move(tn).get();
    BOOST_TEST(tn_resp.term() == 7U);
    BOOST_TEST(tn_resp.success());

    append_entries_request<NodeId> ae{8, self, 1, 1, {{1, 2, {std::byte{0x2a}}}}, 1};
    auto ae_resp = std::move(client.send_append_entries(target, ae, rpc_timeout)).get();
    BOOST_TEST(ae_resp.term() == 8U);
    BOOST_TEST(ae_resp.success());

    install_snapshot_request<NodeId> is{
        9, self, 4, 2, 0, {std::byte{1}, std::byte{2}, std::byte{3}}, true};
    auto is_resp = std::move(client.send_install_snapshot(target, is, rpc_timeout)).get();
    BOOST_TEST(is_resp.term() == 12U);

    auto fl = client.send_fetch_log_entries(target, fetch_log_entries_request<NodeId>{self, 3, 4},
                                            rpc_timeout);
    auto fl_resp = std::move(fl).get();
    BOOST_TEST(fl_resp.available());
    BOOST_TEST((fl_resp.responder_id() == target));
    BOOST_TEST(fl_resp.entries().size() == 2U);

    std::lock_guard lock(seen.mutex);
    BOOST_TEST((seen.request_vote == self));
    BOOST_TEST((seen.request_pre_vote == self));
    BOOST_TEST((seen.timeout_now == self));
    BOOST_TEST((seen.append_entries == self));
    BOOST_TEST((seen.install_snapshot == self));
    BOOST_TEST((seen.fetch_log_entries == self));
}

}  // namespace kythira::testing::textual_ids
