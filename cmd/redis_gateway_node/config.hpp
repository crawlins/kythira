// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file config.hpp
/// @brief `redis_gateway_node`'s configuration, read from the environment
///        (.kiro/specs/redis-compatible-kv/ design Component 9).
///
/// The convention is `cmd/chaos_node/config.hpp`'s: every knob is an
/// environment variable, a missing required one is an `std::invalid_argument`
/// naming it, and the process never reads a secret from `argv` — a command
/// line is world-readable in `ps`, the environment of another user's process
/// is not.

#include <raft/logger.hpp>
#include <raft/redis_gateway.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kythira::redis_node {

enum class wire_serializer : std::uint8_t {
    json = 0,
    cbor = 1,
};

struct node_options {
    // ── the Raft side ────────────────────────────────────────────────────────
    std::uint64_t _node_id{1};
    std::string _bind_address{"0.0.0.0"};
    std::uint16_t _raft_port{7000};
    /// Every voter's Raft base URL, this node included: `id=http://host:port`.
    std::map<std::uint64_t, std::string> _peers;
    /// Every voter's gateway `host:port`, for forwarding. Defaults to the
    /// peer's host with this node's own plaintext listen port.
    std::map<std::uint64_t, std::string> _peer_gateways;
    /// Keys the initial shard map is cut at; empty means one shard covering
    /// the whole key space, from which the split policy takes over.
    std::vector<std::string> _shard_cuts;
    std::chrono::milliseconds _tick_interval{2};
    std::chrono::milliseconds _election_timeout_min{150};
    std::chrono::milliseconds _election_timeout_max{300};
    std::chrono::milliseconds _heartbeat_interval{50};
    std::chrono::milliseconds _rpc_timeout{1000};
    std::chrono::milliseconds _policy_interval{10000};
    std::size_t _executor_stripes{4};
    wire_serializer _serializer{wire_serializer::cbor};
    /// Applied to the host's logger, every group's logger and the gateway's.
    /// `info` by default: a healthy server is quiet, and the Raft core's
    /// debug stream is a heartbeat every 50 ms per follower per group.
    log_level _log_level{log_level::info};

    // ── the gateway side ─────────────────────────────────────────────────────
    std::string _acl_file;
    redis_gateway_config _gateway;
};

/// Read every variable in design Component 9's table, plus the Raft ones.
/// @throws std::invalid_argument naming the variable that is missing or malformed.
[[nodiscard]] auto from_env() -> node_options;

[[nodiscard]] auto usage() -> std::string;

}  // namespace kythira::redis_node
