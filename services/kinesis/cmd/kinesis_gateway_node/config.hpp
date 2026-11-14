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
#include <raft/service_module.hpp>
#include <raft/static_or_iam_authorizer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
    /// `KINESIS_AUTH`: `static` keys (the default), Kythira `iam`, or
    /// `both` (static_or_iam_authorizer.hpp).
    kythira::aws_auth::auth_mode _auth{kythira::aws_auth::auth_mode::static_keys};
    /// `KINESIS_STATIC_KEYS_FILE` (kinesis_static_keys.hpp): required with
    /// `static` and `both`, refused with `iam`.
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
    /// Concurrent API connections, each served on its own thread: the cap
    /// on Kinesis's request threads (service-colocation Requirement 6.1).
    std::size_t _max_connections{256};
    std::chrono::milliseconds _command_timeout{5000};

    // ── retention and tiering (task 10) ──────────────────────────────────────
    /// `KINESIS_DISK_FLOOR_BYTES`: refuse writes below this much free space
    /// on the segment disk; 0 turns the guard off.
    std::uint64_t _disk_floor_bytes{std::uint64_t{1} << 30U};
    /// `KINESIS_METRICS`: serve per-stream and per-shard metrics on
    /// `GET /metrics` (unauthenticated; it names streams and accounts).
    bool _metrics{false};
    /// `KINESIS_KMS`: the key provider encrypted streams use (task 14):
    /// `off` (StartStreamEncryption refused), `local` (data keys wrapped
    /// under `KINESIS_KMS_PASSPHRASE[_FILE]`, the same on every node) or
    /// `aws` (AWS KMS in `KINESIS_KMS_REGION`, default the region above, at
    /// `KINESIS_KMS_ENDPOINT` host:port, default AWS's; credentials from
    /// `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN`).
    std::string _kms{"off"};
    std::string _kms_passphrase;
    std::string _kms_region;
    std::string _kms_endpoint;
    /// `KINESIS_MAINTENANCE_INTERVAL_MS`: how often a node runs the
    /// retention and tiering pass.
    std::chrono::milliseconds _maintenance_interval{60000};
    /// `KINESIS_TIER_AFTER_HOURS`, or `KINESIS_TIER_AFTER_MS` when set
    /// (seconds-scale tiering for tests and demos): tier sealed segments
    /// whose last record is this old; 0 never tiers. Needs the S3 settings
    /// below.
    std::int64_t _tier_after_ms{0};
    /// `KINESIS_TIER_S3_BUCKET`, `_PREFIX`, `_REGION` (default: the
    /// region above), `_ENDPOINT` (any S3-compatible store; AWS when empty).
    /// Credentials come from the AWS SDK's default chain.
    std::string _tier_bucket;
    std::string _tier_prefix;
    std::string _tier_region;
    std::string _tier_endpoint;
};

/// Whether this build can tier to S3 (the AWS SDK's s3 component was found).
[[nodiscard]] auto s3_tier_available() -> bool;

/// Looks a variable up; nullptr when unset. `std::getenv` in the daemon.
using env_lookup = std::function<const char*(const char*)>;

/// @throws std::invalid_argument naming the variable that is missing or
///         malformed.
[[nodiscard]] auto from_env(const env_lookup& env) -> node_options;

/// `from_env` over the module interface's lookup, which says nothing for
/// an unset or empty variable.
[[nodiscard]] auto from_env(const service_node::env_lookup& env) -> node_options;

/// The process environment, as the module interface reads it.
[[nodiscard]] auto process_env() -> service_node::env_lookup;

/// The variables the Kinesis module reads in `kythira_node`
/// (.kiro/specs/service-colocation/ task 5, Requirement 8.1), over `env`:
///
/// - `KINESIS_DATA_DIR` is always the node's data directory's `kinesis/`.
/// - Any other variable set in `env` is taken as it is.
/// - Otherwise the node's settings fill in: `KYTHIRA_PEERS` from
///   `KYTHIRA_NODE_PEERS` (`id=host:port` with the scheme added),
///   `KYTHIRA_RAFT_BIND` and `_PORT` from the node's, the API's
///   certificate and key from `KYTHIRA_NODE_TLS_CERT` and `_KEY`, the peer
///   gateways' CA from `KYTHIRA_NODE_TLS_CA`, and
///   `KINESIS_ALLOW_PLAINTEXT` from `KYTHIRA_NODE_INSECURE_PLAINTEXT=1`.
/// - `KYTHIRA_ALLOW_PLAINTEXT_RAFT` is true: Raft is the node's shared
///   listener, which speaks mutual TLS unless the node is plaintext, so the
///   stand-alone daemon's plaintext-Raft guard does not apply.
[[nodiscard]] auto composite_env(service_node::env_lookup env) -> service_node::env_lookup;

[[nodiscard]] auto is_loopback_address(const std::string& address) -> bool;

/// One line per setting, secrets elided: what `--check-config` prints.
[[nodiscard]] auto describe(const node_options& o) -> std::string;

[[nodiscard]] auto usage() -> std::string;

}  // namespace kythira::kinesis_node
