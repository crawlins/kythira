// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Assembles kythira::tcp_raft_types for use in chaos_node and integration tests.
// Uses:
//   tcp_rpc_client / tcp_rpc_server   — real TCP inter-process transport
//   file_persistence_engine            — durable storage on disk
//   test_key_value_state_machine       — existing key-value state machine
//   json_rpc_serializer                — existing JSON serialiser
//   console_logger / noop_metrics      — standard helpers

#include <raft/console_logger.hpp>
#include <raft/file_persistence.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/membership.hpp>
#include <raft/metrics.hpp>
#include <raft/tcp_rpc.hpp>
#include <raft/test_state_machine.hpp>
#include <raft/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace kythira {

/// The bundle over any `NodeId` the TCP transport carries: `std::uint64_t`
/// (`tcp_raft_types`), or a textual id for a host binary built with
/// `KYTHIRA_NODE_ID_KIND=text` (`include/raft/host_node_id.hpp`).
template<typename NodeId = std::uint64_t> struct basic_tcp_raft_types {
    // ── Future types ─────────────────────────────────────────────────────────
    // future_default (not the hardcoded Folly kythira::Future) so this stays
    // consistent with tcp_rpc_client (raft/tcp_rpc.hpp), which now also uses
    // future_default/promise_default/future_factory_default throughout.
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    // ── Primitive types ──────────────────────────────────────────────────────
    using node_id_type = NodeId;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    // ── Serialiser ───────────────────────────────────────────────────────────
    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = json_rpc_serializer<serialized_data_type>;

    // ── Network ──────────────────────────────────────────────────────────────
    using network_client_type = basic_tcp_rpc_client<NodeId>;
    using network_server_type = basic_tcp_rpc_server<NodeId>;

    // ── Storage ──────────────────────────────────────────────────────────────
    using persistence_engine_type =
        file_persistence_engine<node_id_type, term_id_type, log_index_type>;

    // ── Application ──────────────────────────────────────────────────────────
    using state_machine_type = test_key_value_state_machine<log_index_type>;

    // ── Infrastructure ───────────────────────────────────────────────────────
    using logger_type = console_logger;
    using metrics_type = noop_metrics;
    using membership_manager_type = default_membership_manager<node_id_type>;
    using configuration_type = raft_configuration;

    // ── Compound message types ────────────────────────────────────────────────
    using log_entry_type = log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = cluster_configuration<node_id_type>;
    using snapshot_type = snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = request_vote_response<term_id_type>;

    using append_entries_request_type =
        append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
    using append_entries_response_type = append_entries_response<term_id_type, log_index_type>;

    using install_snapshot_request_type =
        install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = install_snapshot_response<term_id_type>;
};

/// The numeric bundle, as before `NodeId` was a parameter.
using tcp_raft_types = basic_tcp_raft_types<>;

static_assert(raft_types<tcp_raft_types>, "tcp_raft_types must satisfy raft_types");
static_assert(raft_types<basic_tcp_raft_types<std::string>>,
              "basic_tcp_raft_types<std::string> must satisfy raft_types");

}  // namespace kythira
