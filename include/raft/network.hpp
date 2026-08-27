// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/types.hpp>
#include <concepts/future.hpp>
#include <functional>
#include <chrono>

namespace kythira {

// Network client concept - defines interface for sending RPC requests
// Each RPC method returns its own specific future type
template<typename C>
concept network_client =
    requires(C client, std::uint64_t target, const kythira::request_vote_request<>& rvr,
             const kythira::append_entries_request<>& aer,
             const kythira::install_snapshot_request<>& isr, std::chrono::milliseconds timeout) {
        // Send RequestVote RPC - returns Future<request_vote_response<>>
        {
            client.send_request_vote(target, rvr, timeout)
        } -> kythira::future<kythira::request_vote_response<>>;

        // Send AppendEntries RPC - returns Future<append_entries_response<>>
        {
            client.send_append_entries(target, aer, timeout)
        } -> kythira::future<kythira::append_entries_response<>>;

        // Send InstallSnapshot RPC - returns Future<install_snapshot_response<>>
        {
            client.send_install_snapshot(target, isr, timeout)
        } -> kythira::future<kythira::install_snapshot_response<>>;
    };

// Network server concept - defines interface for receiving RPC requests
template<typename S>
concept network_server = requires(
    S server,
    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
        rv_handler,
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
        ae_handler,
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)>
        is_handler) {
    // Register RPC handlers
    { server.register_request_vote_handler(rv_handler) } -> std::same_as<void>;
    { server.register_append_entries_handler(ae_handler) } -> std::same_as<void>;
    { server.register_install_snapshot_handler(is_handler) } -> std::same_as<void>;

    // Server lifecycle
    { server.start() } -> std::convertible_to<void>;
    { server.stop() } -> std::convertible_to<void>;
    { server.is_running() } -> std::convertible_to<bool>;
};

// ============================================================================
// Optional bootstrap extensions (NOT required by the base network concepts so
// that existing mock / test implementations remain unaffected)
// ============================================================================

// Satisfied by a network_client that additionally supports ClusterJoin RPCs.
// The RPC is routed by address_type (std::string) because the joining node
// does not yet know the target's node_id.
template<typename C>
concept network_client_with_cluster_join =
    requires(C client, const std::string& addr, const kythira::cluster_join_request<>& req,
             std::chrono::milliseconds timeout) {
        {
            client.send_cluster_join_request(addr, req, timeout)
        } -> kythira::future<kythira::cluster_join_response<>>;
    };

// Satisfied by a network_server that can register a ClusterJoin handler.
template<typename S>
concept network_server_with_cluster_join =
    requires(S server,
             std::function<kythira::cluster_join_response<>(const kythira::cluster_join_request<>&)>
                 handler) {
        { server.register_cluster_join_handler(handler) } -> std::same_as<void>;
    };

// Satisfied by a network_client that can send ClusterLeave RPCs.
template<typename C>
concept network_client_with_cluster_leave =
    requires(C client, const std::string& addr, const kythira::cluster_leave_request<>& req,
             std::chrono::milliseconds timeout) {
        {
            client.send_cluster_leave_request(addr, req, timeout)
        } -> kythira::future<kythira::cluster_leave_response<>>;
    };

// Satisfied by a network_server that can register a ClusterLeave handler.
template<typename S>
concept network_server_with_cluster_leave = requires(
    S server,
    std::function<kythira::cluster_leave_response<>(const kythira::cluster_leave_request<>&)>
        handler) {
    { server.register_cluster_leave_handler(handler) } -> std::same_as<void>;
};

// ============================================================================
// Optional RequestPreVote extension (Raft "disruptive server" fix, Ongaro's
// dissertation §9.6) — NOT required by the base network concepts so existing transports
// (simulator_network_*, tls_tcp_rpc_*) are unaffected until each one opts
// in. node<Types>::check_election_timeout() checks
// network_client_with_pre_vote<network_client_type> via if constexpr and
// falls back to going straight to a real election (today's behavior,
// byte-for-byte) for any Types bundle whose transport doesn't implement it.
// ============================================================================

// Satisfied by a network_client that can send RequestPreVote RPCs.
template<typename C>
concept network_client_with_pre_vote =
    requires(C client, std::uint64_t target, const kythira::request_pre_vote_request<>& pvr,
             std::chrono::milliseconds timeout) {
        {
            client.send_request_pre_vote(target, pvr, timeout)
        } -> kythira::future<kythira::request_pre_vote_response<>>;
    };

// Satisfied by a network_server that can register a RequestPreVote handler.
template<typename S>
concept network_server_with_pre_vote = requires(
    S server,
    std::function<kythira::request_pre_vote_response<>(const kythira::request_pre_vote_request<>&)>
        handler) {
    { server.register_request_pre_vote_handler(handler) } -> std::same_as<void>;
};

// ============================================================================
// Optional peer-to-peer catch-up extension (.kiro/specs/peer2peer-log-replication/,
// Requirement 5.2) — NOT required by the base network concepts so existing
// transports (simulator_network_*, tcp_rpc_*, tls_tcp_rpc_*) are unaffected
// until each one opts in.
// ============================================================================

// Satisfied by a network_client that can send fetch_log_entries RPCs.
template<typename C>
concept network_client_with_log_fetch =
    requires(C client, std::uint64_t target, const kythira::fetch_log_entries_request<>& req,
             std::chrono::milliseconds timeout) {
        {
            client.send_fetch_log_entries(target, req, timeout)
        } -> kythira::future<kythira::fetch_log_entries_response<>>;
    };

// Satisfied by a network_server that can register a fetch_log_entries handler.
template<typename S>
concept network_server_with_log_fetch =
    requires(S server, std::function<kythira::fetch_log_entries_response<>(
                           const kythira::fetch_log_entries_request<>&)>
                           handler) {
        { server.register_fetch_log_entries_handler(handler) } -> std::same_as<void>;
    };

// ============================================================================
// Optional leadership-transfer extension (Ongaro's dissertation §3.10) — NOT
// required by the base network concepts, so a transport that has not
// implemented TimeoutNow keeps working and `node<Types>::transfer_leadership()`
// fails with `leader_transfer_unsupported_exception` on that bundle rather than
// failing to compile.
//
// The multi-Raft placement driver's `transfer_leader` operator and the
// load-split `scatter` both need this. It is the cheapest rebalance in the
// system — no data moves, and the target already holds the log — which is why
// it is worth a wire message of its own rather than being approximated by
// stopping the leader and letting an election happen.
// ============================================================================

// Satisfied by a network_client that can send TimeoutNow RPCs.
template<typename C>
concept network_client_with_timeout_now =
    requires(C client, std::uint64_t target, const kythira::timeout_now_request<>& req,
             std::chrono::milliseconds timeout) {
        {
            client.send_timeout_now(target, req, timeout)
        } -> kythira::future<kythira::timeout_now_response<>>;
    };

// Satisfied by a network_server that can register a TimeoutNow handler.
template<typename S>
concept network_server_with_timeout_now = requires(
    S server,
    std::function<kythira::timeout_now_response<>(const kythira::timeout_now_request<>&)> handler) {
    { server.register_timeout_now_handler(handler) } -> std::same_as<void>;
};

}  // namespace kythira
