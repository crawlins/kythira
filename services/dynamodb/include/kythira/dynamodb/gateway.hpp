// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file gateway.hpp
/// @brief What `dynamodb_gateway_node` runs: configuration from the
///        environment, and the front end and handlers served over the core
///        HTTPS server (`.kiro/specs/dynamodb-compatible-api/` task 7).
///
/// `gateway<Backend>` owns no storage: it takes a backend, builds the
/// static-key authorizer, the front end and the handlers on it, and serves
/// them through `raft/beast_https_server.hpp`, the Beast host the IAM and
/// ledger daemons use. `GET /` answers as AWS does,
/// `healthy: dynamodb.<region>.amazonaws.com `, which SDK and load-balancer
/// health checks expect.
///
/// The backend is the daemon's choice: `local_backend` on `memory_kv_engine`
/// (one node, no Raft, nothing kept across a restart, like DynamoDB Local's
/// `-inMemory`), or `raft_backend` on a cluster. A gateway over a
/// `raft_backend` also serves the forward endpoint peers send ops to.
///
/// ## Environment
///
/// | Variable | Default | Meaning |
/// |---|---|---|
/// | `KYTHIRA_DYNAMODB_BIND` | `0.0.0.0` | address to listen on, or `*` |
/// | `KYTHIRA_DYNAMODB_PORT` | `8000` | port; 0 picks a free one |
/// | `KYTHIRA_DYNAMODB_TLS_CERT`, `_TLS_KEY` | | PEM chain and key; required unless plaintext |
/// | `KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT` | `0` | `1` serves plain HTTP (tests; warns) |
/// | `KYTHIRA_DYNAMODB_REGION` | `us-east-1` | region in ARNs and the SigV4 scope |
/// | `KYTHIRA_DYNAMODB_PARTITION` | `aws` | partition in ARNs |
/// | `KYTHIRA_DYNAMODB_ADVERTISED_ENDPOINT` | `localhost:<port>` | what `DescribeEndpoints` says |
/// | `KYTHIRA_DYNAMODB_STATIC_KEYS_FILE` | | access keys, one per line (below) |
/// | `KYTHIRA_DYNAMODB_STATIC_KEYS` | | the same, `,`-separated, for tests |
/// | `KYTHIRA_DYNAMODB_DATA_DIR` | | the disk guard's directory, and the database's |
/// | `KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES` | 1 GiB | free space below which writes are refused |
/// | `KYTHIRA_DYNAMODB_ENGINE` | `memory` | `memory`, or `rocksdb` (needs `PEERS`, `DATA_DIR`) |
/// | `KYTHIRA_DYNAMODB_NODE_ID` | | this node's id, with `PEERS` |
/// | `KYTHIRA_DYNAMODB_PEERS` | | `id=http://host:port,...`: every voter's Raft address |
/// | `KYTHIRA_DYNAMODB_RAFT_BIND`, `_RAFT_PORT` | `0.0.0.0`, `7000` | the Raft listener |
/// | `KYTHIRA_DYNAMODB_PEER_GATEWAYS` | | `id=host:port,...`: every other voter's gateway |
/// | `KYTHIRA_DYNAMODB_FORWARD_SECRET` | | the secret gateways forward with (16+ chars) |
/// | `KYTHIRA_DYNAMODB_FORWARD_CA` | system roots | PEM bundle peers' certificates chain to |
/// | `KYTHIRA_DYNAMODB_LOG_LEVEL` | `info` | the Raft groups' log level, `trace` to `critical` |
/// | `KYTHIRA_DYNAMODB_COMPAT` | AWS's | `switch=value,...`: the cluster's compatibility profile,
/// e.g. `throttling=off` |
/// | `KYTHIRA_DYNAMODB_BACKUP_DIR` | | directory backups live in (shared by a cluster's nodes) |
/// | `KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET` | | S3 bucket backups live in (builds with the AWS SDK) |
/// | `KYTHIRA_DYNAMODB_BACKUP_S3_PREFIX`, `_REGION`, `_ENDPOINT` | | key prefix, region, endpoint |
/// | `KYTHIRA_DYNAMODB_BUCKET_DIR` | `<backup dir>/buckets` | export and import buckets, one
/// directory each |
/// | `KYTHIRA_DYNAMODB_REPLICA_PEERS` | | `region=https://host:port,...`: global tables' peer
/// regions |
/// | `KYTHIRA_DYNAMODB_REPLICA_ACCESS_KEY_ID`, `_SECRET_ACCESS_KEY` | | the keys peers know
/// this cluster's replicator by (required with `REPLICA_PEERS`) |
/// | `KYTHIRA_DYNAMODB_REPLICA_CA` | system roots | PEM bundle peers' certificates chain to |
/// | `KYTHIRA_DYNAMODB_STRONG_NODE_ID` | | this node's id in the strong host |
/// | `KYTHIRA_DYNAMODB_STRONG_PEERS` | | `id=region@http://host:port,...`: every strong voter |
/// | `KYTHIRA_DYNAMODB_STRONG_RAFT_BIND`, `_RAFT_PORT` | `0.0.0.0`, `7001` | the strong listener |
/// | `KYTHIRA_DYNAMODB_STRONG_GATEWAYS` | | `id=host:port,...`: every other strong voter's gateway
/// | | `KYTHIRA_DYNAMODB_STRONG_SECRET` | | the secret strong voters forward with (16+ chars) | |
/// `KYTHIRA_DYNAMODB_STRONG_HEARTBEAT_MS` | `250` | strong groups' heartbeat | |
/// `KYTHIRA_DYNAMODB_STRONG_ELECTION_MIN_MS`, `_MAX_MS` | `2500`, `5000` | their election timeouts
/// |
///
/// Without a backup store a single node keeps backups in memory, and a
/// cluster refuses `CreateBackup` (`backup.hpp`) and
/// `UpdateContinuousBackups` (`pitr.hpp`). Point-in-time recovery archives
/// to the same store, and export, import and bulk put jobs keep their
/// records there (`export_import.hpp`, `bulk_put.hpp`).
///
/// The S3 buckets these jobs name are directories under
/// `KYTHIRA_DYNAMODB_BUCKET_DIR`, else under `<backup dir>/buckets`, else,
/// with an S3 backup store, the S3 buckets of those names on its endpoint;
/// a single node on the memory engine keeps them in memory.
///
/// Global tables (`global_tables.hpp`) replicate to the regions
/// `KYTHIRA_DYNAMODB_REPLICA_PEERS` names, each another Kythira cluster
/// whose `KYTHIRA_DYNAMODB_REGION` is that region and whose keys include
/// the replication principal's. Every node of a cluster should have the
/// same peers: the replicator runs wherever the catalog leads.
///
/// Without `PEERS` the daemon is one node on `local_backend`, with no Raft.
/// With it, the node joins a Raft cluster on `raft_backend` (one `PEERS`
/// entry is a one-node cluster), and forwards each request to the group
/// leader's gateway (`forward_link.hpp`).
///
/// A key is `access-key-id:secret:account:name:role`, with role `read-only`,
/// `read-write` or `admin`. Blank lines and lines starting with `#` are
/// skipped. At least one key is required: the gateway never serves
/// unauthenticated requests.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/bulk_put.hpp>
#include <kythira/dynamodb/export_import.hpp>
#include <kythira/dynamodb/pitr.hpp>
#include <kythira/dynamodb/batch_api.hpp>
#include <kythira/dynamodb/compat_profile.hpp>
#include <kythira/dynamodb/disk_guard.hpp>
#include <kythira/dynamodb/dynamodb_actions.hpp>
#include <kythira/dynamodb/forward_link.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/global_tables.hpp>
#include <kythira/dynamodb/insights_api.hpp>
#include <kythira/dynamodb/vector_search.hpp>
#include <kythira/dynamodb/item_api.hpp>
#include <kythira/dynamodb/partiql.hpp>
#include <kythira/dynamodb/query_api.hpp>
#include <kythira/dynamodb/resource_policy.hpp>
#include <kythira/dynamodb/kinesis_destination.hpp>
#include <kythira/dynamodb/streams.hpp>
#include <kythira/dynamodb/table_api.hpp>
#include <kythira/dynamodb/ttl.hpp>
#include <kythira/dynamodb/txn_api.hpp>

#include <raft/beast_https_server.hpp>
#include <raft/logger.hpp>
#include <raft/static_key_authorizer.hpp>

#include <boost/beast/http.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::dynamodb {

/// A Raft cluster of gateways. Empty `peers` means one node with no Raft
/// (`local_backend`).
struct cluster_config {
    /// This node's id, as text (numeric or composite by the build's
    /// `KYTHIRA_NODE_ID_KIND`; the daemon parses it).
    std::string node_id;
    /// Every voter, this node included: id text to Raft URL
    /// (`http://host:port`).
    std::map<std::string, std::string> peers;
    std::string raft_bind = "0.0.0.0";
    std::uint16_t raft_port = 7000;
    /// Every other voter's gateway listener, for forwarded ops.
    std::map<std::string, forward_peer> peer_gateways;
    std::string forward_secret;
    /// PEM bundle the peers' gateway certificates chain to; empty uses the
    /// system's.
    std::string forward_ca_path;
    /// Automatic splits (task 24): a group's leader splits it once it holds
    /// this many bytes, or this many keys; 0 turns that measure off, and
    /// both 0 turns automatic splits off. The defaults are the host's.
    std::uint64_t split_bytes = k_default_split_bytes;
    std::uint64_t split_keys = k_default_split_keys;
    /// The least time between two splits of one group.
    std::chrono::milliseconds split_interval{std::chrono::hours{1}};

    static constexpr std::uint64_t k_default_split_bytes = 96ULL * 1024 * 1024;
    static constexpr std::uint64_t k_default_split_keys = 960'000;

    [[nodiscard]] auto enabled() const -> bool { return !peers.empty(); }
    [[nodiscard]] auto splits_enabled() const -> bool {
        return split_bytes != 0 || split_keys != 0;
    }
};

/// One voter of the strong host (task 27): its region and Raft URL.
struct strong_peer {
    std::string region;
    std::string url;
};

/// The strong host (task 27, design.md "Multi-region strong
/// consistency"): a second multi-raft host whose groups have the same
/// number of voters in each of three regions, for tables made strong.
/// Empty `peers` means no strong host.
struct strong_config {
    /// This node's id in the strong host, unique across the three regions.
    std::string node_id;
    /// Every strong voter of the three regions, this node included.
    std::map<std::string, strong_peer> peers;
    std::string raft_bind = "0.0.0.0";
    std::uint16_t raft_port = 7001;
    /// Every other strong voter's gateway, for ops forwarded to a strong
    /// group's leader, which may be in another region.
    std::map<std::string, forward_peer> gateways;
    /// The secret every strong voter of the three regions forwards with.
    std::string forward_secret;
    /// Raft timings for a wide-area network.
    std::chrono::milliseconds heartbeat{250};
    std::chrono::milliseconds election_min{2500};
    std::chrono::milliseconds election_max{5000};

    static constexpr std::size_t k_regions = 3;

    [[nodiscard]] auto enabled() const -> bool { return !peers.empty(); }
    /// The regions, sorted.
    [[nodiscard]] auto regions() const -> std::vector<std::string> {
        std::vector<std::string> out;
        for (const auto& [id, p] : peers) {
            if (std::find(out.begin(), out.end(), p.region) == out.end()) {
                out.push_back(p.region);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    /// The voters in `region`.
    [[nodiscard]] auto voters_in(std::string_view region) const -> std::vector<std::string> {
        std::vector<std::string> out;
        for (const auto& [id, p] : peers) {
            if (p.region == region) {
                out.push_back(id);
            }
        }
        return out;
    }
};

struct gateway_config {
    https::server_config http{};
    front_end_config front{};
    std::vector<aws_auth::static_key> keys;
    /// Empty: no disk guard.
    std::string data_dir;
    std::uint64_t disk_floor_bytes = disk_guard::k_default_floor_bytes;
    /// `memory`, or `rocksdb` (a Raft cluster's database under the data
    /// directory).
    std::string engine = "memory";
    /// The Raft groups' log level on a cluster.
    kythira::log_level log_level = kythira::log_level::info;
    /// The cluster's compatibility switches (Requirement 3); tables' tags
    /// override them.
    dynamo_compat_profile compat{};
    cluster_config cluster;
    /// The strong host (task 27); off unless `KYTHIRA_DYNAMODB_STRONG_PEERS`
    /// is set.
    strong_config strong;
    /// Where Kinesis streaming destinations publish (task 17); unset is
    /// `kinesis_sink_from_env()`.
    kinesis_sink kinesis{};
    /// Where backups live (task 18): `backups` when set, else a directory
    /// store over `backup_dir`, else none. The S3 settings are read here
    /// and turned into `backups` by the daemon, the one place that links
    /// the AWS SDK.
    std::shared_ptr<backup_store> backups;
    std::string backup_dir;
    std::string backup_s3_bucket;
    std::string backup_s3_prefix;
    std::string backup_s3_region;
    std::string backup_s3_endpoint;
    /// Export and import buckets (task 20): `buckets` when set, else
    /// directories under `bucket_dir`, else under `<backup_dir>/buckets`,
    /// else none. The daemon sets `buckets` for S3 and for memory.
    bucket_resolver buckets;
    std::string bucket_dir;
    /// Global tables' peer regions (task 22): `peers` when set (tests link
    /// clusters in-process), else one HTTP link per `replica_peers` entry.
    std::shared_ptr<peer_set> peers;
    std::vector<replication_peer_config> replica_peers;
};

/// Reads one environment variable; empty optional when unset.
using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

namespace gateway_detail {

[[nodiscard]] inline auto trim(std::string_view s) -> std::string_view {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] inline auto split(std::string_view s, char sep) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    for (;;) {
        const auto at = s.find(sep);
        out.push_back(s.substr(0, at));
        if (at == std::string_view::npos) {
            return out;
        }
        s.remove_prefix(at + 1);
    }
}

template<typename T> [[nodiscard]] auto number(std::string_view s) -> std::optional<T> {
    T v{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

}  // namespace gateway_detail

/// Parses one key, `id:secret:account:name:role`. The secret may not hold a
/// `:`, which AWS-style secrets never do. Returns the error text on failure.
[[nodiscard]] inline auto parse_static_key(std::string_view line)
    -> std::variant<aws_auth::static_key, std::string> {
    const auto parts = gateway_detail::split(line, ':');
    if (parts.size() != 5) {
        return std::string(
            "a key needs five ':'-separated fields "
            "(access-key-id:secret:account:name:role)");
    }
    aws_auth::static_key k;
    k.access_key_id = std::string(parts[0]);
    k.secret = std::string(parts[1]);
    k.account = std::string(parts[2]);
    k.name = std::string(parts[3]);
    const auto role = parts[4];
    if (role == "read-only") {
        k.role = aws_auth::static_role::read_only;
    } else if (role == "read-write") {
        k.role = aws_auth::static_role::read_write;
    } else if (role == "admin") {
        k.role = aws_auth::static_role::admin;
    } else {
        return "unknown role '" + std::string(role) + "' (read-only, read-write or admin)";
    }
    if (k.access_key_id.empty() || k.secret.empty() || k.account.empty() || k.name.empty()) {
        return std::string("a key's id, secret, account and name must not be empty");
    }
    return k;
}

/// Parses a key list: one key per line, or per `,` when `sep` is `,`.
/// Blank entries and `#` comments are skipped. Error text names the entry
/// by its 1-based number and never echoes the secret.
[[nodiscard]] inline auto parse_static_keys(std::string_view text, char sep)
    -> std::variant<std::vector<aws_auth::static_key>, std::string> {
    std::vector<aws_auth::static_key> out;
    std::size_t n = 0;
    for (const auto raw : gateway_detail::split(text, sep)) {
        ++n;
        const auto entry = gateway_detail::trim(raw);
        if (entry.empty() || entry.front() == '#') {
            continue;
        }
        auto k = parse_static_key(entry);
        if (auto* e = std::get_if<std::string>(&k)) {
            return "static key " + std::to_string(n) + ": " + *e;
        }
        out.push_back(std::move(std::get<aws_auth::static_key>(k)));
    }
    return out;
}

/// The environment variables `gateway_config_from_env` reads, for
/// `--help`.
inline constexpr std::string_view k_gateway_env_help =
    "  KYTHIRA_DYNAMODB_BIND                 address to listen on (0.0.0.0)\n"
    "  KYTHIRA_DYNAMODB_PORT                 port (8000; 0 picks a free one)\n"
    "  KYTHIRA_DYNAMODB_TLS_CERT             PEM certificate chain, leaf first\n"
    "  KYTHIRA_DYNAMODB_TLS_KEY              PEM private key\n"
    "  KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT   1 serves plain HTTP (tests only)\n"
    "  KYTHIRA_DYNAMODB_REGION               region (us-east-1)\n"
    "  KYTHIRA_DYNAMODB_PARTITION            ARN partition (aws)\n"
    "  KYTHIRA_DYNAMODB_ADVERTISED_ENDPOINT  host:port DescribeEndpoints answers\n"
    "  KYTHIRA_DYNAMODB_STATIC_KEYS_FILE     access keys, one per line:\n"
    "                                        id:secret:account:name:role\n"
    "  KYTHIRA_DYNAMODB_STATIC_KEYS          the same, ','-separated\n"
    "  KYTHIRA_DYNAMODB_DATA_DIR             directory the disk guard watches\n"
    "  KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES     free-space floor for writes (1 GiB)\n"
    "  KYTHIRA_DYNAMODB_ENGINE               storage engine: memory, or rocksdb\n"
    "                                        (needs PEERS and DATA_DIR) (memory)\n"
    "  KYTHIRA_DYNAMODB_NODE_ID              this node's id (with PEERS)\n"
    "  KYTHIRA_DYNAMODB_PEERS                id=http://host:port,... every voter's\n"
    "                                        Raft address, self included; unset\n"
    "                                        runs one node with no Raft\n"
    "  KYTHIRA_DYNAMODB_RAFT_BIND            Raft listener address (0.0.0.0)\n"
    "  KYTHIRA_DYNAMODB_RAFT_PORT            Raft listener port (7000)\n"
    "  KYTHIRA_DYNAMODB_PEER_GATEWAYS        id=host:port,... every other voter's\n"
    "                                        gateway, for forwarded requests\n"
    "  KYTHIRA_DYNAMODB_FORWARD_SECRET       shared secret between gateways\n"
    "                                        (16 or more characters)\n"
    "  KYTHIRA_DYNAMODB_FORWARD_CA           PEM bundle peers' certificates chain\n"
    "                                        to (system roots)\n"
    "  KYTHIRA_DYNAMODB_SPLIT_BYTES          a group splits at this many bytes;\n"
    "                                        0 never by size (100663296)\n"
    "  KYTHIRA_DYNAMODB_SPLIT_KEYS           or at this many keys; 0 never by\n"
    "                                        count (960000); both 0: no splits\n"
    "  KYTHIRA_DYNAMODB_SPLIT_INTERVAL_MS    least time between two splits of a\n"
    "                                        group (3600000)\n"
    "  KYTHIRA_DYNAMODB_LOG_LEVEL            Raft log level: trace, debug, info,\n"
    "                                        warning, error or critical (info)\n"
    "  KYTHIRA_DYNAMODB_COMPAT               switch=value,... the cluster's\n"
    "                                        compatibility profile, e.g.\n"
    "                                        throttling=off (AWS's behaviour)\n"
    "  KYTHIRA_DYNAMODB_KINESIS_ENDPOINT     host:port Kinesis streaming destinations\n"
    "                                        publish to (AWS's for the stream)\n"
    "  KYTHIRA_DYNAMODB_KINESIS_TLS          0 publishes over plain HTTP (1)\n"
    "  KYTHIRA_DYNAMODB_KINESIS_CA           PEM bundle the Kinesis endpoint chains\n"
    "                                        to (system roots); AWS_ACCESS_KEY_ID,\n"
    "                                        AWS_SECRET_ACCESS_KEY, AWS_SESSION_TOKEN\n"
    "                                        sign the calls\n"
    "  KYTHIRA_DYNAMODB_BACKUP_DIR           directory backups live in (shared by\n"
    "                                        every node of a cluster)\n"
    "  KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET     S3 bucket backups live in\n"
    "  KYTHIRA_DYNAMODB_BACKUP_S3_PREFIX     key prefix in that bucket\n"
    "  KYTHIRA_DYNAMODB_BACKUP_S3_REGION     its region (the AWS SDK default chain)\n"
    "  KYTHIRA_DYNAMODB_BACKUP_S3_ENDPOINT   an S3-compatible endpoint instead\n"
    "  KYTHIRA_DYNAMODB_BUCKET_DIR           export and import buckets, one directory\n"
    "                                        each (default <backup dir>/buckets)\n"
    "  KYTHIRA_DYNAMODB_REPLICA_PEERS        region=https://host:port,... global\n"
    "                                        tables' peer regions\n"
    "  KYTHIRA_DYNAMODB_REPLICA_ACCESS_KEY_ID      keys the peers know this\n"
    "  KYTHIRA_DYNAMODB_REPLICA_SECRET_ACCESS_KEY  cluster's replicator by\n"
    "  KYTHIRA_DYNAMODB_REPLICA_CA           PEM bundle peers' certificates chain to\n"
    "  KYTHIRA_DYNAMODB_STRONG_NODE_ID       this node's id in the strong host\n"
    "  KYTHIRA_DYNAMODB_STRONG_PEERS         id=region@http://host:port,... every\n"
    "                                        strong voter of the three regions;\n"
    "                                        unset: no multi-region strong tables\n"
    "  KYTHIRA_DYNAMODB_STRONG_RAFT_BIND     strong Raft listener address (0.0.0.0)\n"
    "  KYTHIRA_DYNAMODB_STRONG_RAFT_PORT     strong Raft listener port (7001)\n"
    "  KYTHIRA_DYNAMODB_STRONG_GATEWAYS      id=host:port,... every other strong\n"
    "                                        voter's gateway\n"
    "  KYTHIRA_DYNAMODB_STRONG_SECRET        secret strong voters forward with, the\n"
    "                                        same in all three regions (16+ chars)\n"
    "  KYTHIRA_DYNAMODB_STRONG_HEARTBEAT_MS  strong groups' heartbeat (250)\n"
    "  KYTHIRA_DYNAMODB_STRONG_ELECTION_MIN_MS, _MAX_MS  their election timeout\n"
    "                                        range (2500, 5000)\n";

namespace gateway_detail {

/// `id=value,...`. Error text on a malformed or repeated entry.
[[nodiscard]] inline auto parse_id_map(std::string_view name, std::string_view text)
    -> std::variant<std::map<std::string, std::string>, std::string> {
    std::map<std::string, std::string> out;
    for (const auto raw : split(text, ',')) {
        const auto entry = trim(raw);
        if (entry.empty()) {
            continue;
        }
        const auto eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1 == entry.size()) {
            return std::string(name) + ": expected id=value, got '" + std::string(entry) + "'";
        }
        if (!out.emplace(std::string(trim(entry.substr(0, eq))),
                         std::string(trim(entry.substr(eq + 1))))
                 .second) {
            return std::string(name) + ": id '" + std::string(entry.substr(0, eq)) +
                   "' appears twice";
        }
    }
    return out;
}

/// `host:port`, or `[v6]:port`.
[[nodiscard]] inline auto parse_host_port(std::string_view text) -> std::optional<forward_peer> {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    auto host = text.substr(0, colon);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    const auto port = number<std::uint16_t>(text.substr(colon + 1));
    if (!port || *port == 0 || host.empty()) {
        return std::nullopt;
    }
    return forward_peer{std::string(host), *port};
}

/// Reads the cluster settings into `c.cluster`; error text on failure.
[[nodiscard]] inline auto cluster_from_env(const env_lookup& env, gateway_config& c)
    -> std::optional<std::string> {
    auto& k = c.cluster;
    const auto peers = env("KYTHIRA_DYNAMODB_PEERS");
    if (!peers || trim(*peers).empty()) {
        if (c.engine != "memory") {
            return std::string("KYTHIRA_DYNAMODB_ENGINE=") + c.engine +
                   " needs a cluster: set KYTHIRA_DYNAMODB_PEERS (it may list only this node)";
        }
        return std::nullopt;
    }
    auto parsed = parse_id_map("KYTHIRA_DYNAMODB_PEERS", *peers);
    if (auto* e = std::get_if<std::string>(&parsed)) {
        return *e;
    }
    k.peers = std::move(std::get<0>(parsed));
    k.node_id = env("KYTHIRA_DYNAMODB_NODE_ID").value_or("");
    if (k.node_id.empty()) {
        return std::string("KYTHIRA_DYNAMODB_NODE_ID is required with KYTHIRA_DYNAMODB_PEERS");
    }
    if (!k.peers.contains(k.node_id)) {
        return "KYTHIRA_DYNAMODB_PEERS must include this node's own id '" + k.node_id + "'";
    }
    k.raft_bind = env("KYTHIRA_DYNAMODB_RAFT_BIND").value_or("0.0.0.0");
    if (auto v = env("KYTHIRA_DYNAMODB_RAFT_PORT")) {
        const auto p = number<std::uint16_t>(*v);
        if (!p) {
            return "KYTHIRA_DYNAMODB_RAFT_PORT: not a port: '" + *v + "'";
        }
        k.raft_port = *p;
    }
    k.forward_ca_path = env("KYTHIRA_DYNAMODB_FORWARD_CA").value_or("");
    for (const auto& [name, field] : {std::pair{"KYTHIRA_DYNAMODB_SPLIT_BYTES", &k.split_bytes},
                                      std::pair{"KYTHIRA_DYNAMODB_SPLIT_KEYS", &k.split_keys}}) {
        if (auto v = env(name)) {
            const auto n = number<std::uint64_t>(*v);
            if (!n) {
                return std::string(name) + ": not a count: '" + *v + "'";
            }
            *field = *n;
        }
    }
    if (auto v = env("KYTHIRA_DYNAMODB_SPLIT_INTERVAL_MS")) {
        const auto n = number<std::uint64_t>(*v);
        if (!n || *n == 0) {
            return "KYTHIRA_DYNAMODB_SPLIT_INTERVAL_MS: not a positive count: '" + *v + "'";
        }
        k.split_interval = std::chrono::milliseconds{static_cast<std::int64_t>(*n)};
    }
    if (k.peers.size() > 1) {
        k.forward_secret = env("KYTHIRA_DYNAMODB_FORWARD_SECRET").value_or("");
        if (k.forward_secret.size() < k_min_forward_secret) {
            return "KYTHIRA_DYNAMODB_FORWARD_SECRET is required with more than one peer, and "
                   "needs " +
                   std::to_string(k_min_forward_secret) + " or more characters";
        }
        auto gw = parse_id_map("KYTHIRA_DYNAMODB_PEER_GATEWAYS",
                               env("KYTHIRA_DYNAMODB_PEER_GATEWAYS").value_or(""));
        if (auto* e = std::get_if<std::string>(&gw)) {
            return *e;
        }
        for (const auto& [id, text] : std::get<0>(gw)) {
            const auto hp = parse_host_port(text);
            if (!hp) {
                return "KYTHIRA_DYNAMODB_PEER_GATEWAYS: '" + text + "' is not host:port";
            }
            k.peer_gateways.emplace(id, *hp);
        }
        for (const auto& [id, url] : k.peers) {
            if (id != k.node_id && !k.peer_gateways.contains(id)) {
                return "KYTHIRA_DYNAMODB_PEER_GATEWAYS has no gateway for peer '" + id + "'";
            }
        }
    }
    if (c.engine == "rocksdb" && c.data_dir.empty()) {
        return std::string("KYTHIRA_DYNAMODB_ENGINE=rocksdb needs KYTHIRA_DYNAMODB_DATA_DIR");
    }
    return std::nullopt;
}

/// `KYTHIRA_DYNAMODB_STRONG_*` into `c.strong` (task 27); the error text
/// on a bad setting. The strong host needs a cluster, exactly three
/// regions with the same number of voters each, this node among its own
/// region's, and a gateway for every other strong voter.
[[nodiscard]] inline auto strong_from_env(const env_lookup& env, gateway_config& c)
    -> std::optional<std::string> {
    const auto text = env("KYTHIRA_DYNAMODB_STRONG_PEERS").value_or("");
    if (trim(text).empty()) {
        return std::nullopt;
    }
    auto& k = c.strong;
    if (!c.cluster.enabled()) {
        return std::string(
            "KYTHIRA_DYNAMODB_STRONG_PEERS needs a cluster: set "
            "KYTHIRA_DYNAMODB_PEERS");
    }
    auto parsed = parse_id_map("KYTHIRA_DYNAMODB_STRONG_PEERS", text);
    if (auto* e = std::get_if<std::string>(&parsed)) {
        return *e;
    }
    for (const auto& [id, value] : std::get<0>(parsed)) {
        const auto at = value.find('@');
        if (at == std::string::npos || at == 0 || at + 1 == value.size()) {
            return "KYTHIRA_DYNAMODB_STRONG_PEERS: expected id=region@url, got '" + id + "=" +
                   value + "'";
        }
        k.peers.emplace(id, strong_peer{value.substr(0, at), value.substr(at + 1)});
    }
    k.node_id = env("KYTHIRA_DYNAMODB_STRONG_NODE_ID").value_or("");
    if (k.node_id.empty()) {
        return std::string(
            "KYTHIRA_DYNAMODB_STRONG_NODE_ID is required with KYTHIRA_DYNAMODB_STRONG_PEERS");
    }
    const auto self = k.peers.find(k.node_id);
    if (self == k.peers.end()) {
        return "KYTHIRA_DYNAMODB_STRONG_PEERS must include this node's own id '" + k.node_id + "'";
    }
    if (self->second.region != c.front.region) {
        return "KYTHIRA_DYNAMODB_STRONG_PEERS puts this node in region '" + self->second.region +
               "', but KYTHIRA_DYNAMODB_REGION is '" + c.front.region + "'";
    }
    const auto regions = k.regions();
    if (regions.size() != strong_config::k_regions) {
        return "KYTHIRA_DYNAMODB_STRONG_PEERS must name exactly three regions, not " +
               std::to_string(regions.size());
    }
    const auto per_region = k.voters_in(regions.front()).size();
    for (const auto& r : regions) {
        if (k.voters_in(r).size() != per_region) {
            return std::string(
                "KYTHIRA_DYNAMODB_STRONG_PEERS must give every region the same "
                "number of voters, so that no region holds a quorum alone");
        }
    }
    k.raft_bind = env("KYTHIRA_DYNAMODB_STRONG_RAFT_BIND").value_or("0.0.0.0");
    if (auto v = env("KYTHIRA_DYNAMODB_STRONG_RAFT_PORT")) {
        const auto p = number<std::uint16_t>(*v);
        if (!p) {
            return "KYTHIRA_DYNAMODB_STRONG_RAFT_PORT: not a port: '" + *v + "'";
        }
        k.raft_port = *p;
    }
    for (const auto& [name, field] :
         {std::pair{"KYTHIRA_DYNAMODB_STRONG_HEARTBEAT_MS", &k.heartbeat},
          std::pair{"KYTHIRA_DYNAMODB_STRONG_ELECTION_MIN_MS", &k.election_min},
          std::pair{"KYTHIRA_DYNAMODB_STRONG_ELECTION_MAX_MS", &k.election_max}}) {
        if (auto v = env(name)) {
            const auto n = number<std::uint32_t>(*v);
            if (!n || *n == 0) {
                return std::string(name) + ": not a positive count: '" + *v + "'";
            }
            *field = std::chrono::milliseconds{*n};
        }
    }
    if (k.election_max <= k.election_min || k.election_min <= k.heartbeat) {
        return std::string(
            "KYTHIRA_DYNAMODB_STRONG_*_MS: need heartbeat < election min < election max");
    }
    k.forward_secret = env("KYTHIRA_DYNAMODB_STRONG_SECRET").value_or("");
    if (k.forward_secret.size() < k_min_forward_secret) {
        return "KYTHIRA_DYNAMODB_STRONG_SECRET is required with KYTHIRA_DYNAMODB_STRONG_PEERS, "
               "and needs " +
               std::to_string(k_min_forward_secret) + " or more characters";
    }
    auto gw = parse_id_map("KYTHIRA_DYNAMODB_STRONG_GATEWAYS",
                           env("KYTHIRA_DYNAMODB_STRONG_GATEWAYS").value_or(""));
    if (auto* e = std::get_if<std::string>(&gw)) {
        return *e;
    }
    for (const auto& [id, hp_text] : std::get<0>(gw)) {
        const auto hp = parse_host_port(hp_text);
        if (!hp) {
            return "KYTHIRA_DYNAMODB_STRONG_GATEWAYS: '" + hp_text + "' is not host:port";
        }
        k.gateways.emplace(id, *hp);
    }
    for (const auto& [id, p] : k.peers) {
        if (id != k.node_id && !k.gateways.contains(id)) {
            return "KYTHIRA_DYNAMODB_STRONG_GATEWAYS has no gateway for strong peer '" + id + "'";
        }
    }
    return std::nullopt;
}

/// `KYTHIRA_DYNAMODB_REPLICA_*` into `c.replica_peers`; the error text on
/// a bad setting.
[[nodiscard]] inline auto replica_peers_from_env(const env_lookup& env, gateway_config& c)
    -> std::optional<std::string> {
    const auto text = env("KYTHIRA_DYNAMODB_REPLICA_PEERS").value_or("");
    if (trim(text).empty()) {
        return std::nullopt;
    }
    auto peers = parse_id_map("KYTHIRA_DYNAMODB_REPLICA_PEERS", text);
    if (auto* e = std::get_if<std::string>(&peers)) {
        return *e;
    }
    const auto key = env("KYTHIRA_DYNAMODB_REPLICA_ACCESS_KEY_ID").value_or("");
    const auto secret = env("KYTHIRA_DYNAMODB_REPLICA_SECRET_ACCESS_KEY").value_or("");
    if (key.empty() || secret.empty()) {
        return std::string(
            "KYTHIRA_DYNAMODB_REPLICA_PEERS needs KYTHIRA_DYNAMODB_REPLICA_ACCESS_KEY_ID and "
            "KYTHIRA_DYNAMODB_REPLICA_SECRET_ACCESS_KEY");
    }
    for (const auto& [region, url] : std::get<0>(peers)) {
        if (region == c.front.region) {
            return "KYTHIRA_DYNAMODB_REPLICA_PEERS names this cluster's own region '" + region +
                   "'";
        }
        replication_peer_config p;
        p.region = region;
        std::string_view rest = url;
        if (rest.starts_with("https://")) {
            rest.remove_prefix(8);
        } else if (rest.starts_with("http://")) {
            rest.remove_prefix(7);
            p.tls = false;
        } else {
            return "KYTHIRA_DYNAMODB_REPLICA_PEERS: '" + url +
                   "' is not http://host:port or https://host:port";
        }
        if (!parse_host_port(std::string(rest))) {
            return "KYTHIRA_DYNAMODB_REPLICA_PEERS: '" + url +
                   "' is not http://host:port or https://host:port";
        }
        p.endpoint = std::string(rest);
        p.ca_path = env("KYTHIRA_DYNAMODB_REPLICA_CA").value_or("");
        p.access_key_id = key;
        p.secret_access_key = secret;
        c.replica_peers.push_back(std::move(p));
    }
    return std::nullopt;
}

}  // namespace gateway_detail

/// Builds the configuration from the environment. Returns the error text
/// on a bad or missing setting.
[[nodiscard]] inline auto gateway_config_from_env(const env_lookup& env)
    -> std::variant<gateway_config, std::string> {
    using gateway_detail::number;
    gateway_config c;
    c.http.port = 8000;
    if (auto v = env("KYTHIRA_DYNAMODB_BIND")) {
        c.http.bind_address = *v;
    }
    if (auto v = env("KYTHIRA_DYNAMODB_PORT")) {
        const auto p = number<std::uint16_t>(*v);
        if (!p) {
            return "KYTHIRA_DYNAMODB_PORT: not a port: '" + *v + "'";
        }
        c.http.port = *p;
    }
    c.http.tls_cert_path = env("KYTHIRA_DYNAMODB_TLS_CERT").value_or("");
    c.http.tls_key_path = env("KYTHIRA_DYNAMODB_TLS_KEY").value_or("");
    if (auto v = env("KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT")) {
        if (*v != "0" && *v != "1") {
            return "KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT: expected 0 or 1, got '" + *v + "'";
        }
        c.http.insecure_plaintext = *v == "1";
    }
    if (!c.http.insecure_plaintext &&
        (c.http.tls_cert_path.empty() || c.http.tls_key_path.empty())) {
        return std::string(
            "KYTHIRA_DYNAMODB_TLS_CERT and KYTHIRA_DYNAMODB_TLS_KEY are required "
            "(or KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT=1 for tests)");
    }
    // DynamoDB items are up to 400 KB and a request carries one; leave
    // room for the JSON around it and for BatchWriteItem's 25 items later.
    c.http.max_body_bytes = std::uint64_t{16} * 1024 * 1024;
    c.front.region = env("KYTHIRA_DYNAMODB_REGION").value_or("us-east-1");
    c.front.partition = env("KYTHIRA_DYNAMODB_PARTITION").value_or("aws");
    c.front.advertised_endpoint = env("KYTHIRA_DYNAMODB_ADVERTISED_ENDPOINT").value_or("");

    std::string keys_error;
    if (auto path = env("KYTHIRA_DYNAMODB_STATIC_KEYS_FILE")) {
        std::ifstream in(*path);
        if (!in) {
            return "KYTHIRA_DYNAMODB_STATIC_KEYS_FILE: cannot read '" + *path + "'";
        }
        std::ostringstream text;
        text << in.rdbuf();
        auto keys = parse_static_keys(text.str(), '\n');
        if (auto* e = std::get_if<std::string>(&keys)) {
            return "KYTHIRA_DYNAMODB_STATIC_KEYS_FILE: " + *e;
        }
        c.keys = std::move(std::get<0>(keys));
    }
    if (auto text = env("KYTHIRA_DYNAMODB_STATIC_KEYS")) {
        auto keys = parse_static_keys(*text, ',');
        if (auto* e = std::get_if<std::string>(&keys)) {
            return "KYTHIRA_DYNAMODB_STATIC_KEYS: " + *e;
        }
        for (auto& k : std::get<0>(keys)) {
            c.keys.push_back(std::move(k));
        }
    }
    if (c.keys.empty()) {
        return std::string(
            "no access keys: set KYTHIRA_DYNAMODB_STATIC_KEYS_FILE or "
            "KYTHIRA_DYNAMODB_STATIC_KEYS");
    }

    c.data_dir = env("KYTHIRA_DYNAMODB_DATA_DIR").value_or("");
    if (auto v = env("KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES")) {
        const auto f = number<std::uint64_t>(*v);
        if (!f) {
            return "KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES: not a byte count: '" + *v + "'";
        }
        c.disk_floor_bytes = *f;
    }
    c.engine = env("KYTHIRA_DYNAMODB_ENGINE").value_or("memory");
    if (c.engine != "memory" && c.engine != "rocksdb") {
        return "KYTHIRA_DYNAMODB_ENGINE: unknown engine '" + c.engine + "' (memory or rocksdb)";
    }
    if (auto v = env("KYTHIRA_DYNAMODB_LOG_LEVEL")) {
        static constexpr std::array<std::pair<std::string_view, kythira::log_level>, 6> k_levels{{
            {"trace", kythira::log_level::trace},
            {"debug", kythira::log_level::debug},
            {"info", kythira::log_level::info},
            {"warning", kythira::log_level::warning},
            {"error", kythira::log_level::error},
            {"critical", kythira::log_level::critical},
        }};
        const auto* it = std::ranges::find(k_levels, gateway_detail::trim(*v),
                                           &std::pair<std::string_view, kythira::log_level>::first);
        if (it == k_levels.end()) {
            return "KYTHIRA_DYNAMODB_LOG_LEVEL: unknown level '" + *v +
                   "' (trace, debug, info, warning, error or critical)";
        }
        c.log_level = it->second;
    }
    if (auto v = env("KYTHIRA_DYNAMODB_COMPAT")) {
        std::vector<std::pair<std::string, std::string>> settings;
        for (const auto part : gateway_detail::split(*v, ',')) {
            const auto entry = gateway_detail::trim(part);
            if (entry.empty()) {
                continue;
            }
            const auto eq = entry.find('=');
            if (eq == std::string_view::npos) {
                return "KYTHIRA_DYNAMODB_COMPAT: '" + std::string(entry) + "' is not switch=value";
            }
            settings.emplace_back(std::string(gateway_detail::trim(entry.substr(0, eq))),
                                  std::string(gateway_detail::trim(entry.substr(eq + 1))));
        }
        auto profile = cluster_profile(settings);
        if (!profile) {
            return "KYTHIRA_DYNAMODB_COMPAT: " + profile.error().message.value_or("invalid");
        }
        c.compat = *profile;
    }
    if (auto e = gateway_detail::cluster_from_env(env, c)) {
        return *e;
    }
    c.backup_dir = env("KYTHIRA_DYNAMODB_BACKUP_DIR").value_or("");
    c.backup_s3_bucket = env("KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET").value_or("");
    c.backup_s3_prefix = env("KYTHIRA_DYNAMODB_BACKUP_S3_PREFIX").value_or("");
    c.backup_s3_region = env("KYTHIRA_DYNAMODB_BACKUP_S3_REGION").value_or("");
    c.backup_s3_endpoint = env("KYTHIRA_DYNAMODB_BACKUP_S3_ENDPOINT").value_or("");
    c.bucket_dir = env("KYTHIRA_DYNAMODB_BUCKET_DIR").value_or("");
    if (!c.backup_dir.empty() && !c.backup_s3_bucket.empty()) {
        return std::string(
            "KYTHIRA_DYNAMODB_BACKUP_DIR and KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET are both set; "
            "choose one");
    }
    if (auto e = gateway_detail::replica_peers_from_env(env, c)) {
        return *e;
    }
    if (auto e = gateway_detail::strong_from_env(env, c)) {
        return *e;
    }
    return c;
}

/// Adapts the HTTPS server's request to the front end's.
[[nodiscard]] inline auto to_front_end_request(const https::request& req,
                                               const https::request_context& ctx)
    -> front_end_request {
    front_end_request r;
    r.method = std::string(req.method_string());
    const std::string_view target = req.target();
    const auto q = target.find('?');
    r.path = std::string(target.substr(0, q));
    if (q != std::string_view::npos) {
        r.query = std::string(target.substr(q + 1));
    }
    for (const auto& f : req) {
        r.headers.emplace_back(std::string(f.name_string()), std::string(f.value()));
    }
    r.body = req.body();
    r.source_ip = ctx.peer_address;
    r.secure = ctx.tls;
    return r;
}

/// Adapts the front end's response to the HTTPS server's.
[[nodiscard]] inline auto to_https_response(const aws_json::http_response& in, unsigned version)
    -> https::response {
    https::response res{static_cast<boost::beast::http::status>(in.status), version};
    for (const auto& [name, value] : in.headers) {
        res.set(name, value);
    }
    res.body() = in.body;
    res.prepare_payload();
    return res;
}

/// The front end and handlers over one backend, served over HTTPS.
template<dynamo_backend Backend> class gateway {
public:
    using authorizer = aws_auth::static_key_authorizer;

    /// Throws `std::invalid_argument` on a bad key table.
    /// Ops forwarded to this node's strong host (task 27), served at
    /// `k_strong_forward_path` with the strong secret. Set before `start`.
    auto set_strong_ops(std::function<std::string(std::string_view)> serve) -> void {
        _strong_ops = std::move(serve);
    }

    gateway(gateway_config config, Backend& backend,
            std::function<void(bool, std::string_view)> log = {})
        : _config(std::move(config)),
          _auth(_config.keys, _config.front.partition),
          _guard(_config.data_dir.empty()
                     ? nullptr
                     : std::make_unique<disk_guard>(_config.data_dir, _config.disk_floor_bytes)),
          _backend(backend),
          _tables(backend),
          _items(backend, _guard.get()),
          _queries(backend),
          _batches(backend, _guard.get()),
          _transactions(backend, _guard.get()),
          _ttl(backend),
          _partiql(backend, _items, _queries, _transactions),
          _streams(backend),
          _destinations(backend),
          _backups(backend, nullptr),
          _sweeper(backend),
          _publisher(backend, {}),
          _backup_worker(backend, nullptr),
          _pitr(backend, nullptr),
          _pitr_worker(backend, nullptr),
          _jobs(backend, nullptr, nullptr),
          _job_worker(backend, nullptr, nullptr),
          _bulk(backend, nullptr, nullptr),
          _bulk_worker(backend, nullptr, nullptr),
          _policies(backend),
          _insights(backend),
          _vectors(backend),
          _vector_worker(backend, &_vectors.registry()),
          _receiver(backend),
          _replicator(backend, nullptr),
          _log(std::move(log)) {
        _auth.register_service(dynamodb_static_roles());
    }

    gateway(const gateway&) = delete;
    auto operator=(const gateway&) -> gateway& = delete;
    gateway(gateway&&) = delete;
    auto operator=(gateway&&) -> gateway& = delete;
    ~gateway() { stop(); }

    /// Binds and starts serving. Throws when the listener or TLS setup
    /// fails.
    void start() {
        _server = std::make_unique<https::server>(
            _config.http,
            [this](const https::request& req, const https::request_context& ctx) {
                return handle(req, ctx);
            },
            _log);
        _server->start();
        auto front = _config.front;
        if (front.advertised_endpoint.empty()) {
            front.advertised_endpoint = "localhost:" + std::to_string(_server->port());
        }
        front.on_internal_error = [this](std::string_view operation, std::string_view what) {
            if (_log) {
                _log(true, std::string(operation) +
                               " answered InternalServerError: " + std::string(what));
            }
        };
        _front = std::make_unique<front_end<authorizer>>(std::move(front), _auth);
        _items.set_compat_profile(_config.compat);
        _queries.set_compat_profile(_config.compat);
        _batches.set_compat_profile(_config.compat);
        _transactions.set_compat_profile(_config.compat);
        _ttl.set_compat_profile(_config.compat);
        _sweeper.set_compat_profile(_config.compat);
        _tables.register_with(*_front);
        _items.register_with(*_front);
        _queries.register_with(*_front);
        _batches.register_with(*_front);
        _transactions.register_with(*_front);
        _ttl.register_with(*_front);
        _partiql.register_with(*_front);
        _streams.register_with(*_front);
        _destinations.register_with(*_front);
        // The static-key authorizer has no resource policies, so no lookup
        // feeds it them; a root with IAM's sets `resource_lookup_for()`.
        _policies.register_with(*_front);
        _insights.register_with(*_front);
        _vectors.register_with(*_front);
        _peers = _config.peers;
        if (!_peers) {
            _peers = std::make_shared<peer_set>();
            for (const auto& p : _config.replica_peers) {
                _peers->add(p.region, http_replication_send(p));
            }
        }
        _tables.set_peers([peers = _peers](std::string_view r) { return peers->has(r); });
        _receiver.register_with(*_front);
        _replicator.set_peers(_peers.get());
        _replicator.set_log([this](std::string_view line) {
            if (_log) {
                _log(true, line);
            }
        });
        auto store = _config.backups;
        if (!store && !_config.backup_dir.empty()) {
            store = std::make_shared<directory_backup_store>(_config.backup_dir);
        }
        _backups.set_store(store);
        _backups.on_created([this] { _backup_worker.poke(); });
        _backups.register_with(*_front);
        _backup_worker.set_store(store);
        _pitr.set_store(store);
        _pitr.on_changed([this] { _pitr_worker.poke(); });
        _pitr.register_with(*_front);
        _pitr_worker.set_store(store);
        auto buckets = _config.buckets;
        if (!buckets && !_config.bucket_dir.empty()) {
            buckets = directory_buckets(_config.bucket_dir);
        } else if (!buckets && !_config.backup_dir.empty()) {
            buckets = directory_buckets(std::filesystem::path(_config.backup_dir) / "buckets");
        }
        _jobs.set_store(store);
        _jobs.set_buckets(buckets);
        _jobs.on_started([this] { _job_worker.poke(); });
        _jobs.register_with(*_front);
        _job_worker.set_store(store);
        _job_worker.set_buckets(buckets);
        _bulk.set_store(store);
        _bulk.set_buckets(buckets);
        _bulk.on_started([this] { _bulk_worker.poke(); });
        _bulk.register_with(*_front);
        _bulk_worker.set_store(store);
        _bulk_worker.set_buckets(buckets);
        _publisher.set_sink(_config.kinesis.configured() ? _config.kinesis
                                                         : kinesis_sink_from_env());
        _publisher.set_region(_config.front.region);
        _ready.store(true);
        _stopping = false;
        _sweeper_thread = std::thread([this] { sweep_loop(); });
    }

    void stop() {
        {
            const std::lock_guard lock(_sweep_mu);
            _stopping = true;
        }
        _sweep_cv.notify_all();
        if (_sweeper_thread.joinable()) {
            _sweeper_thread.join();
        }
        if (_server) {
            _server->stop();
        }
        _ready.store(false);
    }

    [[nodiscard]] auto port() const -> std::uint16_t { return _server ? _server->port() : 0; }

    /// One request; public so tests can call it without a socket.
    auto handle(const https::request& req, const https::request_context& ctx) -> https::response {
        namespace http = boost::beast::http;
        if (req.method() == http::verb::get && req.target() == "/") {
            https::response res{http::status::ok, req.version()};
            res.set(http::field::content_type, "text/plain");
            res.body() = "healthy: dynamodb." + _config.front.region + ".amazonaws.com ";
            res.prepare_payload();
            return res;
        }
        if (!_ready.load()) {
            https::response res{http::status::service_unavailable, req.version()};
            res.prepare_payload();
            return res;
        }
        if constexpr (serves_ops) {
            if (req.target() == k_forward_path && !_config.cluster.forward_secret.empty()) {
                return forwarded(req, _config.cluster.forward_secret,
                                 [this](std::string_view op) { return _backend.serve(op); });
            }
        }
        if (req.target() == k_strong_forward_path && _strong_ops &&
            !_config.strong.forward_secret.empty()) {
            return forwarded(req, _config.strong.forward_secret, _strong_ops);
        }
        return to_https_response(_front->handle(to_front_end_request(req, ctx)), req.version());
    }

private:
    /// The worker for the catalog leader's background jobs: the TTL
    /// sweeper, backups and restores, the point-in-time archiver, exports,
    /// imports and bulk puts, the vector index worker, the global table
    /// replicator, and the Kinesis destination publisher (`ttl.hpp`,
    /// `backup.hpp`, `pitr.hpp`, `export_import.hpp`, `bulk_put.hpp`,
    /// `vector_search.hpp`, `global_tables.hpp`, `kinesis_destination.hpp`), a pass of each a
    /// second, on the node that leads the catalog when the backend has one. A long backup or export
    /// delays the others until it ends.
    void sweep_loop() {
        std::unique_lock lock(_sweep_mu);
        while (!_stopping) {
            lock.unlock();
            try {
                bool here = true;
                if constexpr (requires(Backend& b) {
                                  { b.leads_catalog() } -> std::same_as<bool>;
                              }) {
                    here = _backend.leads_catalog();
                }
                if (here) {
                    try {
                        (void)_sweeper.pass();
                    } catch (const std::exception& e) {
                        // A lost leader or a timeout: the next pass starts
                        // over.
                        if (_log) {
                            _log(true, std::string("ttl sweeper: ") + e.what());
                        }
                    }
                    try {
                        // An unfinished backup or restore is taken up again
                        // on the next pass, here or on a new leader.
                        (void)_backup_worker.pass();
                    } catch (const std::exception& e) {
                        if (_log) {
                            _log(true, std::string("backups: ") + e.what());
                        }
                    }
                    try {
                        // The archive resumes from the stored cursors; a
                        // restore is loaded again from the start.
                        (void)_pitr_worker.pass();
                    } catch (const std::exception& e) {
                        if (_log) {
                            _log(true, std::string("point-in-time recovery: ") + e.what());
                        }
                    }
                    try {
                        // An unfinished export or import runs again from the
                        // start on the next pass, here or on a new leader.
                        (void)_job_worker.pass();
                    } catch (const std::exception& e) {
                        if (_log) {
                            _log(true, std::string("exports and imports: ") + e.what());
                        }
                    }
                    try {
                        // A bulk put resumes at its first input object not
                        // done, here or on a new leader.
                        (void)_bulk_worker.pass();
                    } catch (const std::exception& e) {
                        if (_log) {
                            _log(true, std::string("bulk operations: ") + e.what());
                        }
                    }
                    try {
                        // A new vector index turns ACTIVE once this node has
                        // built its graphs; others build theirs on demand.
                        (void)_vector_worker.pass();
                    } catch (const std::exception& e) {
                        if (_log) {
                            _log(true, std::string("vector indexes: ") + e.what());
                        }
                    }
                    try {
                        // Each replica resumes from its stored cursors, here
                        // or on a new leader.
                        (void)_replicator.pass();
                    } catch (const std::exception& e) {
                        if (_log) {
                            _log(true, std::string("global tables: ") + e.what());
                        }
                    }
                    // Unpublished records stay in the change log; the next
                    // pass resumes from the stored cursors.
                    (void)_publisher.pass();
                }
            } catch (const std::exception& e) {
                if (_log) {
                    _log(true, std::string("kinesis destinations: ") + e.what());
                }
            }
            lock.lock();
            _sweep_cv.wait_for(lock, std::chrono::seconds{1}, [this] { return _stopping; });
        }
    }

    /// A `raft_backend`, which runs ops peers forward to this node.
    static constexpr bool serves_ops = requires(Backend& b, std::string_view op) {
        { b.serve(op) } -> std::same_as<std::string>;
    };

    /// A peer's op: refused unless it carries a fresh, valid MAC.
    auto forwarded(const https::request& req, const std::string& secret,
                   const std::function<std::string(std::string_view)>& serve) -> https::response {
        namespace http = boost::beast::http;
        const auto now =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        const auto time = req.find(k_forward_time_header);
        const auto mac = req.find(k_forward_mac_header);
        if (req.method() != http::verb::post || time == req.end() || mac == req.end() ||
            !forward_authentic(secret, time->value(), mac->value(), req.body(), now)) {
            https::response res{http::status::forbidden, req.version()};
            res.prepare_payload();
            return res;
        }
        https::response res{http::status::ok, req.version()};
        res.set(http::field::content_type, "application/octet-stream");
        res.body() = serve(req.body());
        res.prepare_payload();
        return res;
    }

    gateway_config _config;
    /// Runs ops strong voters forward to this node (task 27); empty
    /// without a strong host.
    std::function<std::string(std::string_view)> _strong_ops;
    authorizer _auth;
    std::unique_ptr<disk_guard> _guard;
    Backend& _backend;
    table_api<Backend> _tables;
    item_api<Backend> _items;
    query_api<Backend> _queries;
    batch_api<Backend> _batches;
    txn_api<Backend> _transactions;
    ttl_api<Backend> _ttl;
    partiql_api<Backend> _partiql;
    streams_api<Backend> _streams;
    kinesis_destination_api<Backend> _destinations;
    backup_api<Backend> _backups;
    ttl_sweeper<Backend> _sweeper;
    kinesis_publisher<Backend> _publisher;
    backup_worker<Backend> _backup_worker;
    pitr_api<Backend> _pitr;
    pitr_worker<Backend> _pitr_worker;
    export_import_api<Backend> _jobs;
    export_import_worker<Backend> _job_worker;
    bulk_put_api<Backend> _bulk;
    bulk_put_worker<Backend> _bulk_worker;
    resource_policy_api<Backend> _policies;
    insights_api<Backend> _insights;
    vector_search_api<Backend> _vectors;
    vector_index_worker<Backend> _vector_worker;
    std::shared_ptr<peer_set> _peers;
    replication_receiver<Backend> _receiver;
    global_table_replicator<Backend> _replicator;
    std::function<void(bool, std::string_view)> _log;
    std::unique_ptr<front_end<authorizer>> _front;
    std::unique_ptr<https::server> _server;
    std::atomic<bool> _ready{false};
    std::mutex _sweep_mu;
    std::condition_variable _sweep_cv;
    bool _stopping = false;
    std::thread _sweeper_thread;
};

}  // namespace kythira::dynamodb
