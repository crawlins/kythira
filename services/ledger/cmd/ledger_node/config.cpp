// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "config.hpp"

#include <raft/net_bind.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

namespace kythira::ledger_node {

namespace {

auto fail(const std::string& what) -> std::invalid_argument {
    return std::invalid_argument("ledger_node: " + what);
}

auto get_opt(const char* name, const std::string& def = {}) -> std::string {
    const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read once at start-up
    return v != nullptr && *v != '\0' ? std::string(v) : def;
}

auto get_required(const char* name) -> std::string {
    auto v = get_opt(name);
    if (v.empty()) {
        throw fail(std::string("missing required env var ") + name);
    }
    return v;
}

auto to_u64(const char* name, const std::string& v) -> std::uint64_t {
    try {
        std::size_t used = 0;
        const auto n = std::stoull(v, &used);
        if (used != v.size() || v.starts_with('-')) {
            throw std::invalid_argument(v);
        }
        return n;
    } catch (const std::exception&) {
        throw fail(std::string(name) + " must be a non-negative integer, got '" + v + "'");
    }
}

auto u64_opt(const char* name, std::uint64_t def) -> std::uint64_t {
    const auto v = get_opt(name);
    return v.empty() ? def : to_u64(name, v);
}

auto ms_opt(const char* name, std::chrono::milliseconds def) -> std::chrono::milliseconds {
    return std::chrono::milliseconds{
        static_cast<std::int64_t>(u64_opt(name, static_cast<std::uint64_t>(def.count())))};
}

auto bool_opt(const char* name, bool def) -> bool {
    const auto v = get_opt(name);
    if (v.empty()) {
        return def;
    }
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        return false;
    }
    throw fail(std::string(name) + " must be true or false, got '" + v + "'");
}

auto port_of(const char* name, const std::string& v) -> std::uint16_t {
    const auto n = to_u64(name, v);
    if (n == 0 || n > 65535) {
        throw fail(std::string(name) + " must be a port number, got '" + v + "'");
    }
    return static_cast<std::uint16_t>(n);
}

/// `host:port` or `[v6]:port`.
auto split_listen(const char* name, const std::string& v) -> std::pair<std::string, std::uint16_t> {
    const auto colon = v.rfind(':');
    if (colon == std::string::npos || colon == 0) {
        throw fail(std::string(name) + " must be host:port, got '" + v + "'");
    }
    std::string host = v.substr(0, colon);
    if (host.starts_with('[') && host.ends_with(']')) {
        host = host.substr(1, host.size() - 2);
    }
    return {host, port_of(name, v.substr(colon + 1))};
}

auto peer_id(const char* name, const std::string& v) -> kythira::host_node_id {
    auto id = kythira::parse_host_peer_node_id(v);
    if (!id) {
        throw fail(std::string(name) + ": " + id.error());
    }
    return std::move(id).value();
}

/// `id=value,id=value,...`
auto id_map(const char* name, const std::string& text)
    -> std::map<kythira::host_node_id, std::string> {
    std::map<kythira::host_node_id, std::string> out;
    std::string tok;
    for (const char c : text + ',') {
        if (c != ',') {
            tok += c;
            continue;
        }
        if (tok.empty()) {
            continue;
        }
        const auto eq = tok.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == tok.size()) {
            throw fail(std::string(name) + " expects id=value[,id=value...], got '" + tok + "'");
        }
        out[peer_id(name, tok.substr(0, eq))] = tok.substr(eq + 1);
        tok.clear();
    }
    if (out.empty()) {
        throw fail(std::string(name) + " is empty");
    }
    return out;
}

/// The host of `scheme://host:port[/...]`.
auto url_host(const std::string& url) -> std::string {
    auto s = url;
    if (const auto p = s.find("://"); p != std::string::npos) {
        s = s.substr(p + 3);
    }
    s = s.substr(0, s.find('/'));
    if (s.starts_with('[')) {
        return s.substr(0, s.find(']') + 1);
    }
    return s.substr(0, s.rfind(':'));
}

}  // namespace

auto from_env() -> node_options {
    node_options o;
    if (auto id = kythira::parse_host_node_id(get_required("KYTHIRA_NODE_ID")); id) {
        o.node_id = std::move(id).value();
    } else {
        throw fail("KYTHIRA_NODE_ID: " + id.error());
    }

    // TLS for the API (always) and, by default, for Raft.
    o.tls_cert = get_required("KYTHIRA_LEDGER_TLS_CERT");
    o.tls_key = get_required("KYTHIRA_LEDGER_TLS_KEY");
    o.client_ca = get_required("KYTHIRA_LEDGER_CLIENT_CA");
    o.roles_file = get_required("KYTHIRA_LEDGER_ROLES_FILE");
    o.data_dir = get_required("KYTHIRA_LEDGER_DATA_DIR");

    o.raft_bind = get_opt("KYTHIRA_RAFT_BIND", o.raft_bind);
    o.raft_port = port_of("KYTHIRA_RAFT_PORT", get_opt("KYTHIRA_RAFT_PORT", "7000"));
    o.peers = id_map("KYTHIRA_PEERS", get_required("KYTHIRA_PEERS"));
    if (!o.peers.contains(o.node_id)) {
        throw fail("KYTHIRA_PEERS must include this node's own id " +
                   kythira::host_node_id_text(o.node_id));
    }
    std::size_t https = 0;
    for (const auto& [id, url] : o.peers) {
        if (url.starts_with("https://")) {
            ++https;
        } else if (!url.starts_with("http://")) {
            throw fail("KYTHIRA_PEERS: " + url + " is not an http:// or https:// URL");
        }
    }
    if (https != 0 && https != o.peers.size()) {
        throw fail("KYTHIRA_PEERS mixes https:// and http:// members");
    }
    o.raft_tls = https != 0;
    o.allow_plaintext_raft = bool_opt("KYTHIRA_ALLOW_PLAINTEXT_RAFT", false);
    if (!o.raft_tls && !o.allow_plaintext_raft &&
        !kythira::net_bind::is_loopback_bind_address(o.raft_bind)) {
        throw fail("refusing plaintext, unauthenticated Raft on KYTHIRA_RAFT_BIND=" + o.raft_bind +
                   ": give KYTHIRA_PEERS https:// URLs (mutual TLS), bind Raft to a loopback "
                   "address, or set KYTHIRA_ALLOW_PLAINTEXT_RAFT=true on a network you trust");
    }
    o.raft_cert = get_opt("KYTHIRA_LEDGER_RAFT_CERT", o.tls_cert);
    o.raft_key = get_opt("KYTHIRA_LEDGER_RAFT_KEY", o.tls_key);
    o.raft_ca = get_opt("KYTHIRA_LEDGER_RAFT_CA", o.client_ca);

    const auto api = split_listen("KYTHIRA_LEDGER_API_LISTEN",
                                  get_opt("KYTHIRA_LEDGER_API_LISTEN", "0.0.0.0:8443"));
    o.api_bind = api.first;
    o.api_port = api.second;
    if (const auto urls = get_opt("KYTHIRA_LEDGER_API_URLS"); !urls.empty()) {
        o.api_urls = id_map("KYTHIRA_LEDGER_API_URLS", urls);
    } else {
        // Each member's API on its Raft host, at this node's API port.
        for (const auto& [id, url] : o.peers) {
            o.api_urls[id] = "https://" + url_host(url) + ":" + std::to_string(o.api_port);
        }
    }
    for (const auto& [id, url] : o.peers) {
        if (!o.api_urls.contains(id)) {
            throw fail("KYTHIRA_LEDGER_API_URLS has no URL for member " +
                       kythira::host_node_id_text(id));
        }
    }
    o.max_body_bytes = u64_opt("KYTHIRA_LEDGER_MAX_BODY_BYTES", o.max_body_bytes);

    o.signing_key = get_opt("KYTHIRA_LEDGER_SIGNING_KEY");
    o.signing_chain = get_opt("KYTHIRA_LEDGER_SIGNING_CHAIN");
    if (o.signing_key.empty() != o.signing_chain.empty()) {
        throw fail("KYTHIRA_LEDGER_SIGNING_KEY and KYTHIRA_LEDGER_SIGNING_CHAIN go together");
    }

    o.replication = static_cast<std::size_t>(u64_opt("KYTHIRA_LEDGER_REPLICATION", o.replication));
    if (o.replication == 0) {
        throw fail("KYTHIRA_LEDGER_REPLICATION must be at least 1");
    }
    o.segment_bytes = u64_opt("KYTHIRA_LEDGER_SEGMENT_BYTES", o.segment_bytes);
    if (o.segment_bytes < 4096) {
        throw fail("KYTHIRA_LEDGER_SEGMENT_BYTES must be at least 4096");
    }
    o.snapshot_threshold_bytes = static_cast<std::size_t>(
        u64_opt("KYTHIRA_LEDGER_SNAPSHOT_THRESHOLD_BYTES", o.snapshot_threshold_bytes));
    o.snapshot_disk_margin_bytes =
        u64_opt("KYTHIRA_LEDGER_SNAPSHOT_DISK_MARGIN_BYTES", o.snapshot_disk_margin_bytes);

    o.tick_interval = ms_opt("KYTHIRA_TICK_INTERVAL_MS", o.tick_interval);
    o.election_timeout_min = ms_opt("KYTHIRA_ELECTION_TIMEOUT_MIN_MS", o.election_timeout_min);
    o.election_timeout_max = ms_opt("KYTHIRA_ELECTION_TIMEOUT_MAX_MS", o.election_timeout_max);
    if (o.election_timeout_max <= o.election_timeout_min) {
        throw fail("KYTHIRA_ELECTION_TIMEOUT_MAX_MS must exceed KYTHIRA_ELECTION_TIMEOUT_MIN_MS");
    }
    o.heartbeat_interval = ms_opt("KYTHIRA_HEARTBEAT_INTERVAL_MS", o.heartbeat_interval);
    o.rpc_timeout = ms_opt("KYTHIRA_RPC_TIMEOUT_MS", o.rpc_timeout);
    o.propose_timeout = ms_opt("KYTHIRA_LEDGER_PROPOSE_TIMEOUT_MS", o.propose_timeout);
    o.maintenance_interval =
        ms_opt("KYTHIRA_LEDGER_MAINTENANCE_INTERVAL_MS", o.maintenance_interval);
    if (const auto m = get_opt("KYTHIRA_LEDGER_METRICS_LISTEN"); !m.empty()) {
        const auto listen = split_listen("KYTHIRA_LEDGER_METRICS_LISTEN", m);
        o.metrics_bind = listen.first;
        o.metrics_port = listen.second;
        if (o.metrics_port == 0) {
            throw fail("KYTHIRA_LEDGER_METRICS_LISTEN needs a port other than 0");
        }
    }
    o.metrics_interval = ms_opt("KYTHIRA_LEDGER_METRICS_INTERVAL_MS", o.metrics_interval);
    if (o.metrics_interval.count() <= 0) {
        throw fail("KYTHIRA_LEDGER_METRICS_INTERVAL_MS must be positive");
    }

    o.export_s3_region = get_opt("KYTHIRA_LEDGER_EXPORT_S3_REGION");
    o.export_s3_endpoint = get_opt("KYTHIRA_LEDGER_EXPORT_S3_ENDPOINT");
    o.export_s3_timeout = ms_opt("KYTHIRA_LEDGER_EXPORT_S3_TIMEOUT_MS", o.export_s3_timeout);
    if (!o.export_s3_endpoint.empty() && o.export_s3_region.empty()) {
        throw fail(
            "KYTHIRA_LEDGER_EXPORT_S3_ENDPOINT needs KYTHIRA_LEDGER_EXPORT_S3_REGION "
            "(any region the endpoint accepts in its signatures)");
    }
    if (o.export_s3_timeout.count() <= 0) {
        throw fail("KYTHIRA_LEDGER_EXPORT_S3_TIMEOUT_MS must be positive");
    }
#if !defined(KYTHIRA_LEDGER_NODE_S3)
    if (!o.export_s3_region.empty()) {
        throw fail(
            "KYTHIRA_LEDGER_EXPORT_S3_REGION is set, but this ledger_node was built "
            "without the AWS SDK's S3 client (CONFIG_AWS_S3_PERSISTENCE)");
    }
#endif

    const auto level = get_opt("KYTHIRA_LOG_LEVEL", "info");
    static const std::map<std::string, kythira::log_level> levels{
        {"trace", kythira::log_level::trace}, {"debug", kythira::log_level::debug},
        {"info", kythira::log_level::info},   {"warning", kythira::log_level::warning},
        {"error", kythira::log_level::error}, {"critical", kythira::log_level::critical},
    };
    const auto it = levels.find(level);
    if (it == levels.end()) {
        throw fail("KYTHIRA_LOG_LEVEL must be trace, debug, info, warning, error or critical");
    }
    o.log_level = it->second;
    return o;
}

auto usage_text() -> std::string_view {
    return R"(usage: ledger_node [--help | --self-test]

The Kythira verifiable ledger daemon (.kiro/specs/verifiable-ledger/,
doc/verifiable_ledger.md). Configured from the environment; runs until
SIGINT or SIGTERM.

Required:
  KYTHIRA_NODE_ID              this node's id
  KYTHIRA_PEERS                id=URL,... every member's Raft URL, this node
                               included: https://host:port (mutual TLS) or
                               http://host:port (see KYTHIRA_ALLOW_PLAINTEXT_RAFT)
  KYTHIRA_LEDGER_DATA_DIR      engine/ (RocksDB), raft/<group> (Raft logs),
                               snapshots/ and segments/<ledger id>/ live here
  KYTHIRA_LEDGER_TLS_CERT      the API's certificate chain (PEM, leaf first)
  KYTHIRA_LEDGER_TLS_KEY       its private key
  KYTHIRA_LEDGER_CLIENT_CA     the CA API clients' certificates chain to
  KYTHIRA_LEDGER_ROLES_FILE    one grant per line: <URI SAN> <ledger|*> <read|write|admin>

Optional:
  KYTHIRA_RAFT_BIND            Raft listen address (0.0.0.0)
  KYTHIRA_RAFT_PORT            Raft listen port (7000)
  KYTHIRA_LEDGER_RAFT_CERT     Raft TLS certificate (KYTHIRA_LEDGER_TLS_CERT); it
                               must allow both server and client authentication
  KYTHIRA_LEDGER_RAFT_KEY      its key (KYTHIRA_LEDGER_TLS_KEY)
  KYTHIRA_LEDGER_RAFT_CA       the CA peers' certificates chain to
                               (KYTHIRA_LEDGER_CLIENT_CA); a separate CA keeps
                               API clients off the Raft port
  KYTHIRA_ALLOW_PLAINTEXT_RAFT allow http:// peers off loopback (false)
  KYTHIRA_LEDGER_API_LISTEN    host:port for the API (0.0.0.0:8443)
  KYTHIRA_LEDGER_API_URLS      id=URL,... each member's API base URL, for
                               redirects (https://<Raft host>:<API port>)
  KYTHIRA_LEDGER_MAX_BODY_BYTES largest request body (8388608)
  KYTHIRA_LEDGER_SIGNING_KEY   digest signing key (PEM); with
  KYTHIRA_LEDGER_SIGNING_CHAIN its certificate chain, GetDigest signs
  KYTHIRA_LEDGER_REPLICATION   voters per ledger (3)
  KYTHIRA_LEDGER_SEGMENT_BYTES unsealed journal bytes per sealed segment (67108864)
  KYTHIRA_LEDGER_SNAPSHOT_THRESHOLD_BYTES   Raft log bytes before compaction (67108864)
  KYTHIRA_LEDGER_SNAPSHOT_DISK_MARGIN_BYTES free space kept when staging (1073741824)
  KYTHIRA_LEDGER_PROPOSE_TIMEOUT_MS         commit wait per write (5000)
  KYTHIRA_LEDGER_MAINTENANCE_INTERVAL_MS    index builds, seals, exports (500)
  KYTHIRA_LEDGER_METRICS_LISTEN host:port for a Prometheus GET /metrics
                               endpoint, plaintext and unauthenticated (off)
  KYTHIRA_LEDGER_METRICS_INTERVAL_MS        how often ledger gauges refresh (5000)
  KYTHIRA_LEDGER_EXPORT_S3_REGION   ExportJournal jobs write their objects to
                               S3 in this region (the job names the bucket);
                               unset, they finish as failed. Credentials come
                               from the AWS SDK's default chain
  KYTHIRA_LEDGER_EXPORT_S3_ENDPOINT an S3-compatible endpoint instead of AWS
                               (path-style), e.g. http://minio:9000
  KYTHIRA_LEDGER_EXPORT_S3_TIMEOUT_MS       per S3 request (30000)
  KYTHIRA_TICK_INTERVAL_MS (5), KYTHIRA_ELECTION_TIMEOUT_MIN_MS (300),
  KYTHIRA_ELECTION_TIMEOUT_MAX_MS (600), KYTHIRA_HEARTBEAT_INTERVAL_MS (75),
  KYTHIRA_RPC_TIMEOUT_MS (2000)
  KYTHIRA_LOG_LEVEL            trace|debug|info|warning|error|critical (info)

  --help       print this text
  --self-test  hash a fixed document and fold a two-leaf proof, then exit
)";
}

}  // namespace kythira::ledger_node
