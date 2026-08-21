// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file grpc_transport_example.cpp
/// @brief End-to-end demonstration of the gRPC Raft transport
/// (.kiro/specs/grpc-transport/): server/client setup with
/// `grpc_kythira_transport_types`, every core and optional RPC, mutual-TLS
/// configuration sourced from `certificate_authority`, error handling, and
/// metrics collection.
///
/// Exit code 0 if every scenario passes, 1 otherwise — the same
/// comprehensive-scenario/exit-code convention the other example programs use.

#include <raft/certificate_authority.hpp>
#include <raft/grpc_exceptions.hpp>
#include <raft/grpc_transport_impl.hpp>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {
using types = kythira::grpc_kythira_transport_types;
using namespace std::chrono_literals;

// Bind port 0 and read the kernel's choice back with server.bound_port() after
// start(), rather than naming a port up front. 51701/51702 were the previous
// literals and both sit inside Linux's default ephemeral range (32768-60999),
// so the kernel could hand either to an unrelated process as a source port --
// which is how this example failed on main at beb5c94, "Address already in use"
// on 51702 for all three --repeat until-pass attempts. Any port picked before
// the bind can be taken before the bind happens; port 0 has no such window.
constexpr std::uint16_t ephemeral_port = 0;

// Register handlers for every RPC the transport supports, so the example can
// exercise both the base concept and each optional extension.
auto register_all_handlers(kythira::grpc_server<types>& server) -> void {
    server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = true};
    });
    server.register_append_entries_handler([](const kythira::append_entries_request<>& req) {
        return kythira::append_entries_response<>{._term = req.term(),
                                                  ._success = true,
                                                  ._conflict_index = std::nullopt,
                                                  ._conflict_term = std::nullopt};
    });
    server.register_install_snapshot_handler([](const kythira::install_snapshot_request<>& req) {
        return kythira::install_snapshot_response<>{._term = req.term()};
    });
    server.register_request_pre_vote_handler([](const kythira::request_pre_vote_request<>& req) {
        return kythira::request_pre_vote_response<>{._term = req.term(), ._vote_granted = true};
    });
    server.register_cluster_join_handler([](const kythira::cluster_join_request<>& req) {
        return kythira::cluster_join_response<>{.accepted = true, .redirect = std::nullopt};
    });
    server.register_cluster_leave_handler([](const kythira::cluster_leave_request<>& req) {
        return kythira::cluster_leave_response<>{.accepted = true, .redirect = std::nullopt};
    });
    server.register_fetch_log_entries_handler([](const kythira::fetch_log_entries_request<>& req) {
        return kythira::fetch_log_entries_response<>{._responder_id = 1,
                                                     ._available = true,
                                                     ._prev_log_term = req.from_index(),
                                                     ._entries = {}};
    });
}

auto demonstrate_core_rpcs(folly::CPUThreadPoolExecutor& exec) -> bool {
    std::cout << "\n[1] Core RPCs over insecure gRPC\n";
    try {
        auto server = kythira::grpc_server<types>("127.0.0.1", ephemeral_port, {},
                                                  kythira::noop_metrics{}, exec);
        register_all_handlers(server);
        server.start();

        std::unordered_map<std::uint64_t, std::string> book{
            {1, "127.0.0.1:" + std::to_string(server.bound_port())}};
        auto client = kythira::grpc_client<types>(book, {}, kythira::noop_metrics{}, exec);

        auto vote =
            client
                .send_request_vote(
                    1,
                    kythira::request_vote_request<>{
                        ._term = 3, ._candidate_id = 2, ._last_log_index = 10, ._last_log_term = 2},
                    2000ms)
                .get();
        std::cout << "  RequestVote → granted=" << std::boolalpha << vote.vote_granted() << "\n";

        auto append =
            client
                .send_append_entries(1,
                                     kythira::append_entries_request<>{._term = 3,
                                                                       ._leader_id = 1,
                                                                       ._prev_log_index = 10,
                                                                       ._prev_log_term = 2,
                                                                       ._entries = {},
                                                                       ._leader_commit = 10},
                                     2000ms)
                .get();
        std::cout << "  AppendEntries → success=" << append.success() << "\n";

        auto snap = client
                        .send_install_snapshot(
                            1,
                            kythira::install_snapshot_request<>{._term = 3,
                                                                ._leader_id = 1,
                                                                ._last_included_index = 5,
                                                                ._last_included_term = 2,
                                                                ._offset = 0,
                                                                ._data = {},
                                                                ._done = true},
                            2000ms)
                        .get();
        std::cout << "  InstallSnapshot → term=" << snap.term() << "\n";

        server.stop();
        return vote.vote_granted() && append.success();
    } catch (const std::exception& e) {
        std::cerr << "  ✗ core RPC scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_optional_extensions(folly::CPUThreadPoolExecutor& exec) -> bool {
    std::cout << "\n[2] Optional extension RPCs (pre-vote, bootstrap, log-fetch)\n";
    try {
        auto server = kythira::grpc_server<types>("127.0.0.1", ephemeral_port, {},
                                                  kythira::noop_metrics{}, exec);
        register_all_handlers(server);
        server.start();

        std::unordered_map<std::uint64_t, std::string> book{
            {1, "127.0.0.1:" + std::to_string(server.bound_port())}};
        auto client = kythira::grpc_client<types>(book, {}, kythira::noop_metrics{}, exec);

        auto pre =
            client
                .send_request_pre_vote(
                    1,
                    kythira::request_pre_vote_request<>{
                        ._term = 4, ._candidate_id = 2, ._last_log_index = 10, ._last_log_term = 3},
                    2000ms)
                .get();
        std::cout << "  RequestPreVote → granted=" << std::boolalpha << pre.vote_granted() << "\n";

        const std::string addr = "127.0.0.1:" + std::to_string(server.bound_port());
        auto join =
            client
                .send_cluster_join_request(
                    addr, kythira::cluster_join_request<>{.node_id = 7, .contact_address = addr},
                    2000ms)
                .get();
        std::cout << "  ClusterJoin → accepted=" << join.is_accepted() << "\n";

        auto fetch =
            client
                .send_fetch_log_entries(1,
                                        kythira::fetch_log_entries_request<>{
                                            ._requester_id = 2, ._from_index = 5, ._to_index = 10},
                                        2000ms)
                .get();
        std::cout << "  FetchLogEntries → available=" << fetch.available() << "\n";

        server.stop();
        return pre.vote_granted() && join.is_accepted() && fetch.available();
    } catch (const std::exception& e) {
        std::cerr << "  ✗ optional extension scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_error_handling(folly::CPUThreadPoolExecutor& exec) -> bool {
    std::cout << "\n[3] Error handling: unregistered extension → UNIMPLEMENTED\n";
    try {
        // Only a core handler; the pre-vote service is never registered.
        auto server = kythira::grpc_server<types>("127.0.0.1", ephemeral_port, {},
                                                  kythira::noop_metrics{}, exec);
        server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
            return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = false};
        });
        server.start();

        std::unordered_map<std::uint64_t, std::string> book{
            {1, "127.0.0.1:" + std::to_string(server.bound_port())}};
        auto client = kythira::grpc_client<types>(book, {}, kythira::noop_metrics{}, exec);

        bool caught = false;
        try {
            client
                .send_request_pre_vote(
                    1,
                    kythira::request_pre_vote_request<>{
                        ._term = 1, ._candidate_id = 1, ._last_log_index = 0, ._last_log_term = 0},
                    2000ms)
                .get();
        } catch (const kythira::grpc_client_error& e) {
            caught = true;
            std::cout << "  Caught grpc_client_error, status_code="
                      << static_cast<int>(e.status_code()) << " (" << e.what() << ")\n";
        }
        server.stop();
        return caught;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ error-handling scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_mutual_tls(folly::CPUThreadPoolExecutor& exec) -> bool {
    std::cout << "\n[4] Mutual TLS with certificate_authority-issued certificates\n";
    try {
        raft::testing::certificate_authority ca;

        raft::testing::leaf_certificate_options server_opts;
        server_opts.subject.common_name = "localhost";
        server_opts.dns_names = {"localhost"};
        server_opts.ip_addresses = {"127.0.0.1"};
        auto server_cert = ca.issue(server_opts);

        raft::testing::leaf_certificate_options client_opts;
        client_opts.subject.common_name = "raft-client";
        client_opts.dns_names = {"raft-client"};
        client_opts.server_auth = false;
        client_opts.client_auth = true;
        auto client_cert = ca.issue(client_opts);

        kythira::grpc_server_config server_cfg;
        server_cfg.enable_tls = true;
        server_cfg.server_cert_pem = server_cert.certificate_pem;
        server_cfg.server_key_pem = server_cert.private_key_pem;
        server_cfg.ca_cert_pem = ca.root_certificate_pem();
        server_cfg.require_client_cert = true;

        auto server = kythira::grpc_server<types>("127.0.0.1", ephemeral_port, server_cfg,
                                                  kythira::noop_metrics{}, exec);
        register_all_handlers(server);
        server.start();

        kythira::grpc_client_config client_cfg;
        client_cfg.enable_tls = true;
        client_cfg.ca_cert_pem = ca.root_certificate_pem();
        client_cfg.client_cert_pem = client_cert.certificate_pem;
        client_cfg.client_key_pem = client_cert.private_key_pem;
        client_cfg.target_name_override = "localhost";

        std::unordered_map<std::uint64_t, std::string> book{
            {1, "127.0.0.1:" + std::to_string(server.bound_port())}};
        auto client = kythira::grpc_client<types>(book, client_cfg, kythira::noop_metrics{}, exec);

        auto vote =
            client
                .send_request_vote(
                    1,
                    kythira::request_vote_request<>{
                        ._term = 5, ._candidate_id = 2, ._last_log_index = 0, ._last_log_term = 0},
                    3000ms)
                .get();
        std::cout << "  mTLS RequestVote → granted=" << std::boolalpha << vote.vote_granted()
                  << "\n";

        server.stop();
        return vote.vote_granted();
    } catch (const std::exception& e) {
        std::cerr << "  ✗ mutual-TLS scenario failed: " << e.what() << "\n";
        return false;
    }
}
}  // namespace

int main() {
    std::cout << "Kythira gRPC Transport Example\n";
    std::cout << "==============================\n";

    // A caller-owned executor onto which every promise fulfillment and every
    // registered handler is posted (Requirement 8) — shared by client + server.
    folly::CPUThreadPoolExecutor exec(4);

    int failed = 0;
    if (!demonstrate_core_rpcs(exec)) {
        ++failed;
    }
    if (!demonstrate_optional_extensions(exec)) {
        ++failed;
    }
    if (!demonstrate_error_handling(exec)) {
        ++failed;
    }
    if (!demonstrate_mutual_tls(exec)) {
        ++failed;
    }

    std::cout << "\n=== Summary ===\n";
    if (failed > 0) {
        std::cerr << failed << " scenario(s) failed\n";
        return 1;
    }
    std::cout << "All gRPC transport scenarios passed!\n";
    return 0;
}
