// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file config.hpp
/// @brief `kinesis_gateway_node`'s configuration, read from the environment
///        (`.kiro/specs/kinesis-compatible-streams/` task 6, Requirement
///        16.1: "following redis_gateway_node for flags, TLS, forwarding and
///        health").
///
/// The Raft-side variables are `redis_gateway_node`'s, with the same names
/// and defaults. The Kinesis side is `KINESIS_*`. Secrets come from the
/// environment or a file, never from `argv`. A missing required variable or
/// a malformed one is an `std::invalid_argument` naming it.

#include <raft/host_node_id.hpp>
#include <raft/logger.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace kythira::kinesis_node {

struct node_options {
    // ── the Raft side (redis_gateway_node's variables) ───────────────────────
    kythira::host_node_id _node_id{kythira::host_node_id_from_number(1)};
    std::string _bind_address{"0.0.0.0"};
    std::uint16_t _raft_port{7000};
    bool _allow_plaintext_raft{false};
    /// Every voter's Raft base URL, this node included.
    std::map<kythira::host_node_id, std::string> _peers;
    std::vector<std::string> _shard_cuts;
    std::chrono::milliseconds _tick_interval{2};
    std::chrono::milliseconds _election_timeout_min{150};
    std::chrono::milliseconds _election_timeout_max{300};
    std::chrono::milliseconds _heartbeat_interval{50};
    std::chrono::milliseconds _rpc_timeout{1000};
    std::chrono::milliseconds _policy_interval{10000};
    std::size_t _executor_stripes{4};
    log_level _log_level{log_level::info};

    // ── the Kinesis side ─────────────────────────────────────────────────────
    /// `KINESIS_LISTEN`: host:port of the API (and internal) listener.
    std::string _listen_host{"0.0.0.0"};
    std::uint16_t _listen_port{4567};
    std::string _tls_cert_path;
    std::string _tls_key_path;
    /// `KINESIS_ALLOW_PLAINTEXT`: serve HTTP, and forward to peers over HTTP.
    bool _allow_plaintext{false};
    std::string _region{"us-east-1"};
    std::string _partition{"aws"};
    /// `KINESIS_STATIC_KEYS_FILE` (kinesis_static_keys.hpp).
    std::string _static_keys_file;
    /// `KINESIS_FORWARD_SECRET` or `KINESIS_FORWARD_SECRET_FILE`: what the
    /// gateways sign forwarded writes and segment fetches with. Required
    /// with more than one voter.
    std::string _forward_secret;
    /// `KINESIS_ITERATOR_KEYS_FILE` (kinesis_iterators.hpp); empty derives
    /// the iterator key from the forwarding secret, or, on a lone node
    /// without one, makes a random key per process.
    std::string _iterator_keys_file;
    /// Every voter's gateway `host:port`. Defaults to the Raft URL's host
    /// with this node's own listen port.
    std::map<kythira::host_node_id, std::string> _peer_gateways;
    /// CA bundle the peers' gateway certificates chain to; the system store
    /// when empty.
    std::string _peer_ca_path;
    /// Raft state under `raft/<group>`, record segments under `segments/`.
    std::filesystem::path _data_dir;
    /// `KINESIS_COMPAT`: cluster compatibility settings, `k=v,k=v`
    /// (kinesis_compat_profile.hpp).
    std::vector<std::pair<std::string, std::string>> _compat;
    std::uint64_t _max_body_bytes{std::uint64_t{8} * 1024 * 1024};
    std::chrono::milliseconds _command_timeout{5000};
};

/// Looks a variable up; nullptr when unset. `std::getenv` in the daemon.
using env_lookup = std::function<const char*(const char*)>;

/// @throws std::invalid_argument naming the variable that is missing or
///         malformed.
[[nodiscard]] auto from_env(const env_lookup& env) -> node_options;

[[nodiscard]] auto is_loopback_address(const std::string& address) -> bool;

/// One line per setting, secrets elided: what `--check-config` prints.
[[nodiscard]] auto describe(const node_options& o) -> std::string;

[[nodiscard]] auto usage() -> std::string;

}  // namespace kythira::kinesis_node
