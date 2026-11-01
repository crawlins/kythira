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

#include <raft/host_node_id.hpp>
#include <raft/logger.hpp>
#include <raft/redis_gateway.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace kythira::redis_node {

enum class wire_serializer : std::uint8_t {
    json = 0,
    cbor = 1,
};

/// `KYTHIRA_CACHE_PERSISTENCE`: where values and Raft state live.
enum class cache_persistence : std::uint8_t {
    /// Values in RAM, Raft state in RAM: the gateway as it was before the
    /// disk-backed cache, for deployments that want exactly that.
    memory = 0,
    /// Values in the chunk store under `KYTHIRA_CACHE_DATA_DIR`, Raft state
    /// in the binary segmented engine beside it.
    file = 1,
};

/// The disk-backed cache's knobs (.kiro/specs/disk-backed-build-cache/
/// design "Configuration"). Only `_persistence` applies in memory mode;
/// the rest are refused there.
struct cache_options {
    cache_persistence _persistence{cache_persistence::file};
    std::filesystem::path _data_dir;
    std::size_t _chunk_bytes{std::size_t{4} * 1024 * 1024};
    std::uint64_t _max_value_bytes{std::uint64_t{2} * 1024 * 1024 * 1024};
    std::uint64_t _max_disk_bytes{0};
    std::uint64_t _min_free_bytes{std::uint64_t{1} * 1024 * 1024 * 1024};
    std::uint64_t _min_free_inodes{10000};
    std::size_t _pack_threshold_bytes{std::size_t{64} * 1024};
    std::uint64_t _orphan_grace_entries{100000};
    std::uint64_t _fetch_bytes_per_sec{50000000};
    /// Raft log bytes between snapshots. The log carries chunk bytes, so
    /// Raft's 10 MB default would snapshot every few uploads.
    std::size_t _snapshot_threshold_bytes{std::size_t{256} * 1024 * 1024};
    std::string _http_listen;
    std::string _https_listen;
    bool _allow_plaintext_http{false};
    std::string _tls_cert_path;
    std::string _tls_key_path;
    std::chrono::seconds _auth_cache_ttl{300};
    /// Every voter's HTTP front end `host:port`, for chunk fetches and
    /// relayed writes. Defaults to the peer's host with this node's own
    /// HTTPS port when forwarding over TLS, else its HTTP port.
    std::map<kythira::host_node_id, std::string> _peer_http;
};

struct node_options {
    // ── the Raft side ────────────────────────────────────────────────────────
    /// `kythira::host_node_id`: a number, or canonical composite text in a
    /// `KYTHIRA_NODE_ID_KIND=text` build.
    kythira::host_node_id _node_id{kythira::host_node_id_from_number(1)};
    std::string _bind_address{"0.0.0.0"};
    std::uint16_t _raft_port{7000};
    /// Raft RPC here is plain, unauthenticated HTTP. A non-loopback
    /// `_bind_address` needs this explicit opt-in (KYTHIRA_ALLOW_PLAINTEXT_RAFT).
    bool _allow_plaintext_raft{false};
    /// Every voter's Raft base URL, this node included: `id=http://host:port`.
    std::map<kythira::host_node_id, std::string> _peers;
    /// Every voter's gateway `host:port`, for forwarding. Defaults to the
    /// peer's host with this node's own plaintext listen port.
    std::map<kythira::host_node_id, std::string> _peer_gateways;
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

    // ── the disk-backed cache ────────────────────────────────────────────────
    cache_options _cache;
};

/// Read every variable in design Component 9's table, plus the Raft ones.
/// @throws std::invalid_argument naming the variable that is missing or malformed.
[[nodiscard]] auto from_env() -> node_options;

/// True when every address `address` binds is loopback: 127.0.0.0/8, ::1, or a
/// name such as "localhost" that /etc/hosts maps only to those.
[[nodiscard]] auto is_loopback_address(const std::string& address) -> bool;

[[nodiscard]] auto usage() -> std::string;

}  // namespace kythira::redis_node
