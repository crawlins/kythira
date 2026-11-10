// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

// ledger_node's configuration (.kiro/specs/verifiable-ledger/ task 7c),
// read from the environment as redis_gateway_node reads its own: the
// shared KYTHIRA_NODE_ID / KYTHIRA_RAFT_* / KYTHIRA_PEERS names, and
// KYTHIRA_LEDGER_* for the rest. usage_text() documents every variable.

#include <raft/console_logger.hpp>
#include <raft/host_node_id.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace kythira::ledger_node {

struct node_options {
    kythira::host_node_id node_id{kythira::host_node_id_from_number(1)};

    // ── Raft ────────────────────────────────────────────────────────────────
    std::string raft_bind{"0.0.0.0"};
    std::uint16_t raft_port{7000};
    /// Every member's Raft URL, this node included: https:// (mutual TLS
    /// with raft_cert / raft_key / raft_ca) or, when allowed, http://.
    std::map<kythira::host_node_id, std::string> peers;
    bool raft_tls{true};
    bool allow_plaintext_raft{false};
    std::string raft_cert;
    std::string raft_key;
    /// The CA that Raft peers' certificates must chain to. Defaults to the
    /// API's client CA; a separate one keeps API clients off the Raft port.
    std::string raft_ca;

    // ── API ─────────────────────────────────────────────────────────────────
    std::string api_bind{"0.0.0.0"};
    std::uint16_t api_port{8443};
    /// Every member's API base URL, for not_leader redirects.
    std::map<kythira::host_node_id, std::string> api_urls;
    std::string tls_cert;
    std::string tls_key;
    std::string client_ca;
    std::filesystem::path roles_file;
    std::uint64_t max_body_bytes{std::uint64_t{8} << 20U};
    /// Digest signing (Requirement 4.5): both or neither.
    std::string signing_key;
    std::string signing_chain;
    /// Prometheus scrape endpoint (`GET /metrics`, plaintext and
    /// unauthenticated); off while metrics_port is 0.
    std::string metrics_bind{"127.0.0.1"};
    std::uint16_t metrics_port{0};
    /// How often the per-ledger gauges are sampled.
    std::chrono::milliseconds metrics_interval{5000};

    // ── Ledgers and storage ─────────────────────────────────────────────────
    std::filesystem::path data_dir;
    std::size_t replication{3};
    std::uint64_t segment_bytes{std::uint64_t{64} << 20U};
    std::size_t snapshot_threshold_bytes{std::size_t{64} << 20U};
    std::uint64_t snapshot_disk_margin_bytes{std::uint64_t{1} << 30U};

    // ── Timing and logs ─────────────────────────────────────────────────────
    std::chrono::milliseconds tick_interval{5};
    std::chrono::milliseconds election_timeout_min{300};
    std::chrono::milliseconds election_timeout_max{600};
    std::chrono::milliseconds heartbeat_interval{75};
    std::chrono::milliseconds rpc_timeout{2000};
    std::chrono::milliseconds propose_timeout{5000};
    std::chrono::milliseconds maintenance_interval{500};
    kythira::log_level log_level{kythira::log_level::info};
};

/// The options from the environment. Throws std::invalid_argument naming
/// the variable at fault.
[[nodiscard]] auto from_env() -> node_options;

[[nodiscard]] auto usage_text() -> std::string_view;

/// Runs the node until SIGINT or SIGTERM; the process exit status.
auto run(const node_options& opt) -> int;

/// SIGINT and SIGTERM stop run(); SIGPIPE is ignored.
auto install_signal_handlers() -> void;

}  // namespace kythira::ledger_node
