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
#include <kythira/dynamodb/batch_api.hpp>
#include <kythira/dynamodb/compat_profile.hpp>
#include <kythira/dynamodb/disk_guard.hpp>
#include <kythira/dynamodb/dynamodb_actions.hpp>
#include <kythira/dynamodb/forward_link.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_api.hpp>
#include <kythira/dynamodb/partiql.hpp>
#include <kythira/dynamodb/query_api.hpp>
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

    [[nodiscard]] auto enabled() const -> bool { return !peers.empty(); }
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
    "  KYTHIRA_DYNAMODB_LOG_LEVEL            Raft log level: trace, debug, info,\n"
    "                                        warning, error or critical (info)\n"
    "  KYTHIRA_DYNAMODB_COMPAT               switch=value,... the cluster's\n"
    "                                        compatibility profile, e.g.\n"
    "                                        throttling=off (AWS's behaviour)\n";

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
          _sweeper(backend),
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
                return forwarded(req);
            }
        }
        return to_https_response(_front->handle(to_front_end_request(req, ctx)), req.version());
    }

private:
    /// The TTL sweeper's worker (`ttl.hpp`): a pass a second, on the node
    /// that leads the catalog when the backend has one.
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
                    (void)_sweeper.pass();
                }
            } catch (const std::exception& e) {
                // A lost leader or a timeout: the next pass starts over.
                if (_log) {
                    _log(true, std::string("ttl sweeper: ") + e.what());
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
    auto forwarded(const https::request& req) -> https::response {
        namespace http = boost::beast::http;
        const auto now =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        const auto time = req.find(k_forward_time_header);
        const auto mac = req.find(k_forward_mac_header);
        if (req.method() != http::verb::post || time == req.end() || mac == req.end() ||
            !forward_authentic(_config.cluster.forward_secret, time->value(), mac->value(),
                               req.body(), now)) {
            https::response res{http::status::forbidden, req.version()};
            res.prepare_payload();
            return res;
        }
        https::response res{http::status::ok, req.version()};
        res.set(http::field::content_type, "application/octet-stream");
        res.body() = _backend.serve(req.body());
        res.prepare_payload();
        return res;
    }

    gateway_config _config;
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
    ttl_sweeper<Backend> _sweeper;
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
