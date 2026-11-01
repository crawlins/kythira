// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "config.hpp"

#include <raft/net_bind.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstring>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace kythira::redis_node {

namespace {

auto get_opt(const char* name, const char* def) -> std::string {
    const char* v = std::getenv(name);
    return v != nullptr ? v : def;
}

auto get_required(const char* name) -> std::string {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        throw std::invalid_argument(std::string("redis_gateway_node: missing required env var ") +
                                    name);
    }
    return v;
}

auto to_bool(const char* name, const std::string& v) -> bool {
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        return false;
    }
    throw std::invalid_argument(std::string("redis_gateway_node: ") + name +
                                " must be true or false, got '" + v + "'");
}

auto to_u64(const char* name, const std::string& v) -> std::uint64_t {
    try {
        std::size_t used = 0;
        auto n = std::stoull(v, &used);
        if (used != v.size()) {
            throw std::invalid_argument(v);
        }
        return n;
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("redis_gateway_node: ") + name +
                                    " must be an integer, got '" + v + "'");
    }
}

auto to_ms(const char* name, const std::string& v) -> std::chrono::milliseconds {
    return std::chrono::milliseconds{static_cast<std::int64_t>(to_u64(name, v))};
}

/// A peer's node id. Canonical composite text holds no `=` or `,`, so the
/// `id=value` split below is unambiguous in either id kind.
auto to_peer_id(const char* name, const std::string& v) -> kythira::host_node_id {
    auto id = kythira::parse_host_peer_node_id(v);
    if (!id) {
        throw std::invalid_argument(std::string("redis_gateway_node: ") + name + ": " + id.error());
    }
    return std::move(id).value();
}

/// `id=value,id=value,...`
auto parse_id_map(const char* name, const std::string& text)
    -> std::map<kythira::host_node_id, std::string> {
    std::map<kythira::host_node_id, std::string> out;
    std::string tok;
    for (char c : text + ',') {
        if (c != ',') {
            tok += c;
            continue;
        }
        if (tok.empty()) {
            continue;
        }
        auto eq = tok.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == tok.size()) {
            throw std::invalid_argument(std::string("redis_gateway_node: ") + name +
                                        " expects id=value[,id=value...], got token '" + tok + "'");
        }
        out[to_peer_id(name, tok.substr(0, eq))] = tok.substr(eq + 1);
        tok.clear();
    }
    if (out.empty()) {
        throw std::invalid_argument(std::string("redis_gateway_node: ") + name + " is empty");
    }
    return out;
}

/// Host portion of `http://host:port/...` or `host:port`.
auto host_of(const std::string& url) -> std::string {
    auto s = url;
    if (auto p = s.find("://"); p != std::string::npos) {
        s = s.substr(p + 3);
    }
    if (auto p = s.find('/'); p != std::string::npos) {
        s = s.substr(0, p);
    }
    if (!s.empty() && s.front() == '[') {
        auto close = s.find(']');
        return close == std::string::npos ? s : s.substr(0, close + 1);
    }
    if (auto p = s.rfind(':'); p != std::string::npos) {
        s = s.substr(0, p);
    }
    return s;
}

auto port_of(const std::string& listen) -> std::string {
    auto p = listen.rfind(':');
    return p == std::string::npos ? "6379" : listen.substr(p + 1);
}

auto is_set(const char* name) -> bool {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0';
}

/// `KYTHIRA_CACHE_*` (.kiro/specs/disk-backed-build-cache/ design
/// "Configuration"). Runs after the gateway's own variables, whose TLS and
/// forwarding choices it follows.
void read_cache_options(node_options& o) {
    auto& c = o._cache;
    const auto mode = get_opt("KYTHIRA_CACHE_PERSISTENCE", "file");
    if (mode == "memory") {
        c._persistence = cache_persistence::memory;
        // Requirement 5.4's old deployment, exactly. Anything that needs a
        // data directory is a sign the operator meant `file`.
        for (const char* name :
             {"KYTHIRA_CACHE_DATA_DIR", "KYTHIRA_CACHE_MAX_DISK_BYTES", "KYTHIRA_CACHE_HTTP_LISTEN",
              "KYTHIRA_CACHE_HTTPS_LISTEN", "KYTHIRA_CACHE_PEER_HTTP"}) {
            if (is_set(name)) {
                throw std::invalid_argument(std::string("redis_gateway_node: ") + name +
                                            " needs KYTHIRA_CACHE_PERSISTENCE=file");
            }
        }
        return;
    }
    if (mode != "file") {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_CACHE_PERSISTENCE must be file or memory, got '" + mode +
            "'");
    }
    c._persistence = cache_persistence::file;
    if (o._serializer != wire_serializer::cbor) {
        // One multi_raft stack per translation unit; the disk-backed one is
        // built for the default wire only.
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_CACHE_PERSISTENCE=file needs "
            "KYTHIRA_WIRE_SERIALIZER=cbor");
    }
    c._data_dir = get_required("KYTHIRA_CACHE_DATA_DIR");
    c._max_disk_bytes =
        to_u64("KYTHIRA_CACHE_MAX_DISK_BYTES", get_required("KYTHIRA_CACHE_MAX_DISK_BYTES"));
    if (c._max_disk_bytes == 0) {
        throw std::invalid_argument("redis_gateway_node: KYTHIRA_CACHE_MAX_DISK_BYTES must be > 0");
    }
    constexpr std::uint64_t max_chunk = std::uint64_t{16} * 1024 * 1024;
    c._chunk_bytes =
        to_u64("KYTHIRA_CACHE_CHUNK_BYTES", get_opt("KYTHIRA_CACHE_CHUNK_BYTES", "4194304"));
    if (c._chunk_bytes == 0 || c._chunk_bytes > max_chunk) {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_CACHE_CHUNK_BYTES must be between 1 and 16777216");
    }
    c._max_value_bytes = to_u64("KYTHIRA_CACHE_MAX_VALUE_BYTES",
                                get_opt("KYTHIRA_CACHE_MAX_VALUE_BYTES", "2147483648"));
    c._min_free_bytes = to_u64("KYTHIRA_CACHE_MIN_FREE_BYTES",
                               get_opt("KYTHIRA_CACHE_MIN_FREE_BYTES", "1073741824"));
    c._min_free_inodes =
        to_u64("KYTHIRA_CACHE_MIN_FREE_INODES", get_opt("KYTHIRA_CACHE_MIN_FREE_INODES", "10000"));
    c._pack_threshold_bytes = to_u64("KYTHIRA_CACHE_PACK_THRESHOLD_BYTES",
                                     get_opt("KYTHIRA_CACHE_PACK_THRESHOLD_BYTES", "65536"));
    if (c._pack_threshold_bytes > max_chunk) {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_CACHE_PACK_THRESHOLD_BYTES must be at most 16777216");
    }
    c._orphan_grace_entries = to_u64("KYTHIRA_CACHE_ORPHAN_GRACE_ENTRIES",
                                     get_opt("KYTHIRA_CACHE_ORPHAN_GRACE_ENTRIES", "100000"));
    c._fetch_bytes_per_sec = to_u64("KYTHIRA_CACHE_FETCH_BYTES_PER_SEC",
                                    get_opt("KYTHIRA_CACHE_FETCH_BYTES_PER_SEC", "50000000"));
    c._snapshot_threshold_bytes =
        to_u64("KYTHIRA_CACHE_SNAPSHOT_THRESHOLD_BYTES",
               get_opt("KYTHIRA_CACHE_SNAPSHOT_THRESHOLD_BYTES", "268435456"));
    c._auth_cache_ttl = std::chrono::seconds{static_cast<std::int64_t>(to_u64(
        "KYTHIRA_CACHE_AUTH_CACHE_SECONDS", get_opt("KYTHIRA_CACHE_AUTH_CACHE_SECONDS", "300")))};

    // A RESP SET is one log entry and lands as one chunk.
    if (o._gateway._max_value_bytes > max_chunk) {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_REDIS_MAX_VALUE_BYTES must be at most 16777216 with "
            "KYTHIRA_CACHE_PERSISTENCE=file (a SET is stored as one chunk); larger values go "
            "over HTTP");
    }

    c._http_listen = get_opt("KYTHIRA_CACHE_HTTP_LISTEN", "");
    c._https_listen = get_opt("KYTHIRA_CACHE_HTTPS_LISTEN", "");
    c._allow_plaintext_http = to_bool("KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP",
                                      get_opt("KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP", "false"));
    if (!c._http_listen.empty() && !c._allow_plaintext_http) {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_CACHE_HTTP_LISTEN serves credentials and values "
            "unencrypted; set KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP=true to allow it, or use "
            "KYTHIRA_CACHE_HTTPS_LISTEN");
    }
    c._tls_cert_path = get_opt("KYTHIRA_CACHE_HTTPS_CERT", o._gateway._tls_cert_path.c_str());
    c._tls_key_path = get_opt("KYTHIRA_CACHE_HTTPS_KEY", o._gateway._tls_key_path.c_str());
    if (!c._https_listen.empty() && (c._tls_cert_path.empty() || c._tls_key_path.empty())) {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_CACHE_HTTPS_LISTEN needs KYTHIRA_CACHE_HTTPS_CERT and "
            "KYTHIRA_CACHE_HTTPS_KEY (or KYTHIRA_REDIS_TLS_CERT and KYTHIRA_REDIS_TLS_KEY)");
    }
    if (auto ph = get_opt("KYTHIRA_CACHE_PEER_HTTP", ""); !ph.empty()) {
        c._peer_http = parse_id_map("KYTHIRA_CACHE_PEER_HTTP", ph);
    } else {
        // Each peer's front end of the kind this node forwards over.
        const auto& listener = o._gateway._forward_tls ? c._https_listen : c._http_listen;
        if (!listener.empty()) {
            for (const auto& [id, url] : o._peers) {
                c._peer_http[id] = host_of(url) + ":" + port_of(listener);
            }
        }
    }
}

}  // namespace

auto is_loopback_address(const std::string& address) -> bool {
    // Same resolver the Raft listener binds with, so "localhost" counts and
    // the check can't disagree with what is actually bound.
    return kythira::net_bind::is_loopback_bind_address(address);
}

auto from_env() -> node_options {
    node_options o;
    // Numeric or canonical composite text by `KYTHIRA_NODE_ID_KIND`; a text
    // build also takes `metadata:<provider>` and asks the instance metadata
    // service.
    if (auto id = kythira::parse_host_node_id(get_required("KYTHIRA_NODE_ID")); id) {
        o._node_id = std::move(id).value();
    } else {
        throw std::invalid_argument("redis_gateway_node: KYTHIRA_NODE_ID: " + id.error());
    }
    o._bind_address = get_opt("KYTHIRA_RAFT_BIND", "0.0.0.0");
    o._raft_port = static_cast<std::uint16_t>(
        to_u64("KYTHIRA_RAFT_PORT", get_opt("KYTHIRA_RAFT_PORT", "7000")));
    // Raft RPC has no TLS here, so anyone who can reach the port can drive
    // every group. Off loopback that has to be asked for explicitly.
    o._allow_plaintext_raft =
        to_bool("KYTHIRA_ALLOW_PLAINTEXT_RAFT", get_opt("KYTHIRA_ALLOW_PLAINTEXT_RAFT", "false"));
    if (!o._allow_plaintext_raft && !is_loopback_address(o._bind_address)) {
        throw std::invalid_argument(
            "redis_gateway_node: refusing to serve plaintext, unauthenticated Raft RPC on "
            "KYTHIRA_RAFT_BIND=" +
            o._bind_address +
            " (set KYTHIRA_RAFT_BIND to a loopback address, or "
            "KYTHIRA_ALLOW_PLAINTEXT_RAFT=true on a network you trust)");
    }
    o._peers = parse_id_map("KYTHIRA_PEERS", get_required("KYTHIRA_PEERS"));
    if (!o._peers.contains(o._node_id)) {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_PEERS must include this node's own id " +
            kythira::host_node_id_text(o._node_id));
    }
    o._tick_interval = to_ms("KYTHIRA_TICK_INTERVAL_MS", get_opt("KYTHIRA_TICK_INTERVAL_MS", "2"));
    o._election_timeout_min =
        to_ms("KYTHIRA_ELECTION_TIMEOUT_MIN_MS", get_opt("KYTHIRA_ELECTION_TIMEOUT_MIN_MS", "150"));
    o._election_timeout_max =
        to_ms("KYTHIRA_ELECTION_TIMEOUT_MAX_MS", get_opt("KYTHIRA_ELECTION_TIMEOUT_MAX_MS", "300"));
    o._heartbeat_interval =
        to_ms("KYTHIRA_HEARTBEAT_INTERVAL_MS", get_opt("KYTHIRA_HEARTBEAT_INTERVAL_MS", "50"));
    o._rpc_timeout = to_ms("KYTHIRA_RPC_TIMEOUT_MS", get_opt("KYTHIRA_RPC_TIMEOUT_MS", "1000"));
    o._policy_interval =
        to_ms("KYTHIRA_POLICY_INTERVAL_MS", get_opt("KYTHIRA_POLICY_INTERVAL_MS", "10000"));
    o._executor_stripes =
        to_u64("KYTHIRA_EXECUTOR_STRIPES", get_opt("KYTHIRA_EXECUTOR_STRIPES", "4"));
    {
        auto s = get_opt("KYTHIRA_WIRE_SERIALIZER", "cbor");
        if (s == "cbor") {
            o._serializer = wire_serializer::cbor;
        } else if (s == "json") {
            o._serializer = wire_serializer::json;
        } else {
            throw std::invalid_argument(
                "redis_gateway_node: KYTHIRA_WIRE_SERIALIZER must be cbor or json, got '" + s +
                "'");
        }
    }
    {
        auto s = get_opt("KYTHIRA_LOG_LEVEL", "info");
        static const std::map<std::string, log_level> levels{
            {"trace", log_level::trace}, {"debug", log_level::debug},
            {"info", log_level::info},   {"warning", log_level::warning},
            {"error", log_level::error}, {"critical", log_level::critical},
        };
        auto it = levels.find(s);
        if (it == levels.end()) {
            throw std::invalid_argument(
                "redis_gateway_node: KYTHIRA_LOG_LEVEL must be "
                "trace|debug|info|warning|error|critical, got '" +
                s + "'");
        }
        o._log_level = it->second;
    }
    {
        auto cuts = get_opt("KYTHIRA_SHARD_CUTS", "");
        std::string tok;
        for (char c : cuts + ',') {
            if (c == ',') {
                if (!tok.empty()) {
                    o._shard_cuts.push_back(tok);
                }
                tok.clear();
            } else {
                tok += c;
            }
        }
    }

    auto& g = o._gateway;
    g._listen = get_opt("KYTHIRA_REDIS_LISTEN", "0.0.0.0:6379");
    g._tls_listen = get_opt("KYTHIRA_REDIS_TLS_LISTEN", "");
    g._tls_cert_path = get_opt("KYTHIRA_REDIS_TLS_CERT", "");
    g._tls_key_path = get_opt("KYTHIRA_REDIS_TLS_KEY", "");
    g._tls_ca_path = get_opt("KYTHIRA_REDIS_TLS_CA", "");
    g._require_client_cert = to_bool("KYTHIRA_REDIS_REQUIRE_CLIENT_CERT",
                                     get_opt("KYTHIRA_REDIS_REQUIRE_CLIENT_CERT", "false"));
    if (!g._tls_listen.empty() && (g._tls_cert_path.empty() || g._tls_key_path.empty())) {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_REDIS_TLS_LISTEN needs KYTHIRA_REDIS_TLS_CERT and "
            "KYTHIRA_REDIS_TLS_KEY");
    }
    if (g._require_client_cert && g._tls_ca_path.empty()) {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_REDIS_REQUIRE_CLIENT_CERT needs KYTHIRA_REDIS_TLS_CA to "
            "verify against");
    }
    g._allow_anonymous =
        to_bool("KYTHIRA_REDIS_ALLOW_ANONYMOUS", get_opt("KYTHIRA_REDIS_ALLOW_ANONYMOUS", "false"));
    // Requirement 10.1: the ACL file is required. The one way out is the
    // explicit anonymous opt-out, which start() warns about every time.
    if (const char* f = std::getenv("KYTHIRA_REDIS_ACL_FILE"); f != nullptr && *f != '\0') {
        o._acl_file = f;
    } else if (!g._allow_anonymous) {
        throw std::invalid_argument(
            "redis_gateway_node: missing required env var KYTHIRA_REDIS_ACL_FILE "
            "(set KYTHIRA_REDIS_ALLOW_ANONYMOUS=true to run an open cache on purpose)");
    }
    {
        auto rc = get_opt("KYTHIRA_REDIS_READ_CONSISTENCY", "leader");
        auto parsed = parse_redis_read_consistency(rc);
        if (!parsed) {
            throw std::invalid_argument(
                "redis_gateway_node: KYTHIRA_REDIS_READ_CONSISTENCY must be leader, any_replica or "
                "linearizable, got '" +
                rc + "'");
        }
        g._read_consistency = *parsed;
    }
    g._max_value_bytes = to_u64("KYTHIRA_REDIS_MAX_VALUE_BYTES",
                                get_opt("KYTHIRA_REDIS_MAX_VALUE_BYTES", "8388608"));
    g._max_clients =
        to_u64("KYTHIRA_REDIS_MAX_CLIENTS", get_opt("KYTHIRA_REDIS_MAX_CLIENTS", "1024"));
    g._idle_timeout =
        to_ms("KYTHIRA_REDIS_IDLE_TIMEOUT_MS", get_opt("KYTHIRA_REDIS_IDLE_TIMEOUT_MS", "300000"));
    g._command_timeout = to_ms("KYTHIRA_REDIS_COMMAND_TIMEOUT_MS",
                               get_opt("KYTHIRA_REDIS_COMMAND_TIMEOUT_MS", "5000"));
    g._max_shard_bytes = to_u64("KYTHIRA_REDIS_MAX_SHARD_BYTES",
                                get_opt("KYTHIRA_REDIS_MAX_SHARD_BYTES", "1073741824"));
    g._sweep_batch =
        to_u64("KYTHIRA_REDIS_SWEEP_BATCH", get_opt("KYTHIRA_REDIS_SWEEP_BATCH", "1024"));
    g._immutable_values = to_bool("KYTHIRA_REDIS_IMMUTABLE_VALUES",
                                  get_opt("KYTHIRA_REDIS_IMMUTABLE_VALUES", "true"));
    g._forwarding =
        to_bool("KYTHIRA_REDIS_FORWARDING", get_opt("KYTHIRA_REDIS_FORWARDING", "true"));
    g._internal_user = get_opt("KYTHIRA_REDIS_INTERNAL_USER", "kythira-internal");
    g._internal_secret = get_opt("KYTHIRA_REDIS_INTERNAL_SECRET", "");
    // Forwarding carries the internal secret, so it follows the listener: a
    // node with a TLS listener forwards to its peers' TLS listeners unless
    // told otherwise. Plaintext forwarding is an explicit opt-in.
    g._forward_tls =
        to_bool("KYTHIRA_REDIS_FORWARD_TLS",
                get_opt("KYTHIRA_REDIS_FORWARD_TLS", g._tls_listen.empty() ? "false" : "true"));
    g._forward_tls_ca_path = get_opt("KYTHIRA_REDIS_FORWARD_TLS_CA", "");
    g._forward_tls_cert_path = get_opt("KYTHIRA_REDIS_FORWARD_TLS_CERT", "");
    g._forward_tls_key_path = get_opt("KYTHIRA_REDIS_FORWARD_TLS_KEY", "");
    g._allow_plaintext_forwarding =
        to_bool("KYTHIRA_REDIS_ALLOW_PLAINTEXT_FORWARDING",
                get_opt("KYTHIRA_REDIS_ALLOW_PLAINTEXT_FORWARDING", "false"));
    if (g._forward_tls && g._forward_tls_ca_path.empty() && g._tls_ca_path.empty()) {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_REDIS_FORWARD_TLS needs KYTHIRA_REDIS_FORWARD_TLS_CA or "
            "KYTHIRA_REDIS_TLS_CA to verify peer gateways against");
    }
    if (g._forward_tls_cert_path.empty() != g._forward_tls_key_path.empty()) {
        throw std::invalid_argument(
            "redis_gateway_node: KYTHIRA_REDIS_FORWARD_TLS_CERT and KYTHIRA_REDIS_FORWARD_TLS_KEY "
            "go together");
    }
    g._io_threads = to_u64("KYTHIRA_REDIS_IO_THREADS", get_opt("KYTHIRA_REDIS_IO_THREADS", "2"));
    g._worker_threads =
        to_u64("KYTHIRA_REDIS_WORKER_THREADS", get_opt("KYTHIRA_REDIS_WORKER_THREADS", "8"));
    g._log_commands =
        to_bool("KYTHIRA_REDIS_LOG_COMMANDS", get_opt("KYTHIRA_REDIS_LOG_COMMANDS", "false"));

    if (auto pg = get_opt("KYTHIRA_REDIS_PEER_GATEWAYS", ""); !pg.empty()) {
        o._peer_gateways = parse_id_map("KYTHIRA_REDIS_PEER_GATEWAYS", pg);
    } else {
        // Each peer's listener of the kind this node forwards over.
        const auto& listener = g._forward_tls ? g._tls_listen : g._listen;
        for (const auto& [id, url] : o._peers) {
            o._peer_gateways[id] = host_of(url) + ":" + port_of(listener);
        }
    }
    read_cache_options(o);
    return o;
}

auto usage() -> std::string {
    std::ostringstream out;
    out << "redis_gateway_node — a Redis-compatible cache front end over Kythira multi-Raft\n"
           "\n"
           "usage: redis_gateway_node               run the daemon (configured by environment)\n"
           "       redis_gateway_node --hash-secret [iterations]\n"
           "                                        read a secret from stdin, print an ACL record\n"
           "       redis_gateway_node --help\n"
           "\n"
           "Raft side (environment):\n"
           "  KYTHIRA_NODE_ID                  this node's id (required): "
        << (kythira::host_node_id_is_text
                ? "decimal, canonical composite text, or metadata:<provider>"
                : "decimal")
        << "\n"
           "  KYTHIRA_PEERS                   id=http://host:port,... for every voter, self "
           "included (required)\n"
           "  KYTHIRA_RAFT_BIND                bind address for Raft RPCs             [0.0.0.0]\n"
           "  KYTHIRA_RAFT_PORT                Raft RPC port                          [7000]\n"
           "  KYTHIRA_ALLOW_PLAINTEXT_RAFT     allow plaintext Raft RPC off loopback  [false]\n"
           "                                   (required unless KYTHIRA_RAFT_BIND is loopback)\n"
           "  KYTHIRA_WIRE_SERIALIZER          cbor | json                            [cbor]\n"
           "  KYTHIRA_LOG_LEVEL                trace|debug|info|warning|error|critical [info]\n"
           "  KYTHIRA_SHARD_CUTS               comma-separated initial shard boundaries [none: one "
           "shard]\n"
           "  KYTHIRA_TICK_INTERVAL_MS, KYTHIRA_ELECTION_TIMEOUT_MIN_MS, "
           "KYTHIRA_ELECTION_TIMEOUT_MAX_MS,\n"
           "  KYTHIRA_HEARTBEAT_INTERVAL_MS, KYTHIRA_RPC_TIMEOUT_MS, KYTHIRA_POLICY_INTERVAL_MS,\n"
           "  KYTHIRA_EXECUTOR_STRIPES\n"
           "\n"
           "Gateway side (environment; design.md Component 9):\n"
           "  KYTHIRA_REDIS_LISTEN             plaintext RESP listener                "
           "[0.0.0.0:6379]\n"
           "  KYTHIRA_REDIS_TLS_LISTEN         TLS listener (needs _TLS_CERT and _TLS_KEY)\n"
           "  KYTHIRA_REDIS_TLS_CERT / _KEY / _CA\n"
           "  KYTHIRA_REDIS_REQUIRE_CLIENT_CERT                                       [false]\n"
           "  KYTHIRA_REDIS_ACL_FILE           ACL file (required unless ALLOW_ANONYMOUS)\n"
           "  KYTHIRA_REDIS_ALLOW_ANONYMOUS    run with no ACL; warns at every start  [false]\n"
           "  KYTHIRA_REDIS_READ_CONSISTENCY   leader | any_replica | linearizable    [leader]\n"
           "  KYTHIRA_REDIS_MAX_VALUE_BYTES                                            [8388608]\n"
           "  KYTHIRA_REDIS_MAX_CLIENTS                                                [1024]\n"
           "  KYTHIRA_REDIS_IDLE_TIMEOUT_MS                                            [300000]\n"
           "  KYTHIRA_REDIS_COMMAND_TIMEOUT_MS                                         [5000]\n"
           "  KYTHIRA_REDIS_MAX_SHARD_BYTES                                            "
           "[1073741824]\n"
           "  KYTHIRA_REDIS_SWEEP_BATCH                                                [1024]\n"
           "  KYTHIRA_REDIS_IMMUTABLE_VALUES                                           [true]\n"
           "  KYTHIRA_REDIS_FORWARDING                                                 [true]\n"
           "  KYTHIRA_REDIS_INTERNAL_USER      ACL user forwarded commands run as     "
           "[kythira-internal]\n"
           "  KYTHIRA_REDIS_INTERNAL_SECRET    that user's secret; empty disables forwarding\n"
           "  KYTHIRA_REDIS_FORWARD_TLS        forward to peers' TLS listeners  [true if "
           "TLS_LISTEN]\n"
           "  KYTHIRA_REDIS_FORWARD_TLS_CA     CA peers must chain to                 [TLS_CA]\n"
           "  KYTHIRA_REDIS_FORWARD_TLS_CERT / _KEY  client cert for peers  [TLS_CERT / TLS_KEY]\n"
           "  KYTHIRA_REDIS_ALLOW_PLAINTEXT_FORWARDING  send the internal secret unencrypted "
           "[false]\n"
           "  KYTHIRA_REDIS_PEER_GATEWAYS      id=host:port,... of every peer's gateway [peer host "
           "+ LISTEN port, or TLS_LISTEN port when forwarding over TLS]\n"
           "  KYTHIRA_REDIS_IO_THREADS, KYTHIRA_REDIS_WORKER_THREADS, KYTHIRA_REDIS_LOG_COMMANDS\n"
           "\n"
           "\n"
           "Disk-backed cache (environment; .kiro/specs/disk-backed-build-cache/):\n"
           "  KYTHIRA_CACHE_PERSISTENCE        file | memory (values and Raft state in RAM) "
           "[file]\n"
           "  KYTHIRA_CACHE_DATA_DIR           chunk store and Raft state (required for file)\n"
           "  KYTHIRA_CACHE_MAX_DISK_BYTES     chunk-store budget per node; evicts above it "
           "(required for file)\n"
           "  KYTHIRA_CACHE_MIN_FREE_BYTES     refuse writes below this free space   "
           "[1073741824]\n"
           "  KYTHIRA_CACHE_MIN_FREE_INODES    refuse writes below this many inodes  [10000]\n"
           "  KYTHIRA_CACHE_CHUNK_BYTES        at most 16777216                      [4194304]\n"
           "  KYTHIRA_CACHE_MAX_VALUE_BYTES    largest value over HTTP               "
           "[2147483648]\n"
           "  KYTHIRA_CACHE_PACK_THRESHOLD_BYTES  smaller chunks go to pack files   [65536]\n"
           "  KYTHIRA_CACHE_ORPHAN_GRACE_ENTRIES  log entries an unbound chunk lives [100000]\n"
           "  KYTHIRA_CACHE_FETCH_BYTES_PER_SEC   peer chunk fetch rate              "
           "[50000000]\n"
           "  KYTHIRA_CACHE_SNAPSHOT_THRESHOLD_BYTES  Raft log bytes per snapshot    "
           "[268435456]\n"
           "  KYTHIRA_CACHE_HTTPS_LISTEN       HTTP/WebDAV front end over TLS\n"
           "  KYTHIRA_CACHE_HTTPS_CERT / _KEY                        [REDIS_TLS_CERT / _KEY]\n"
           "  KYTHIRA_CACHE_HTTP_LISTEN        plaintext front end; needs the opt-in below\n"
           "  KYTHIRA_CACHE_ALLOW_PLAINTEXT_HTTP                                     [false]\n"
           "  KYTHIRA_CACHE_AUTH_CACHE_SECONDS successful-auth cache lifetime         [300]\n"
           "  KYTHIRA_CACHE_PEER_HTTP          id=host:port,... of every peer's front end [peer "
           "host + this node's front-end port]\n"
           "\n"
           "ACL file lines: user <name> <pbkdf2-record|nopass|disabled> "
           "<read_only|read_write|admin> [prefix...|*] "
           "[cert=<subject>]\n";
    return out.str();
}

}  // namespace kythira::redis_node
