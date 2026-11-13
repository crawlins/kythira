// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "config.hpp"

#include <kythira/kinesis/kinesis_catalog.hpp>
#include <kythira/kinesis/kinesis_compat_profile.hpp>
#include <kythira/kinesis/kinesis_peer_client.hpp>

#include <raft/net_bind.hpp>

#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace kythira::kinesis_node {

namespace {

constexpr std::string_view k_prog = "kinesis_gateway_node: ";

[[noreturn]] void fail(const std::string& message) {
    throw std::invalid_argument(std::string(k_prog) + message);
}

struct reader {
    const env_lookup& _env;

    [[nodiscard]] auto opt(const char* name, const char* def) const -> std::string {
        const char* v = _env(name);
        return v != nullptr ? v : def;
    }

    [[nodiscard]] auto required(const char* name) const -> std::string {
        const char* v = _env(name);
        if (v == nullptr || *v == '\0') {
            fail(std::string("missing required env var ") + name);
        }
        return v;
    }

    [[nodiscard]] auto set(const char* name) const -> bool {
        const char* v = _env(name);
        return v != nullptr && *v != '\0';
    }

    [[nodiscard]] auto flag(const char* name, const char* def) const -> bool {
        const auto v = opt(name, def);
        if (v == "1" || v == "true" || v == "yes" || v == "on") {
            return true;
        }
        if (v == "0" || v == "false" || v == "no" || v == "off") {
            return false;
        }
        fail(std::string(name) + " must be true or false, got '" + v + "'");
    }

    [[nodiscard]] auto u64(const char* name, const char* def) const -> std::uint64_t {
        const auto v = opt(name, def);
        try {
            std::size_t used = 0;
            const auto n = std::stoull(v, &used);
            if (used == v.size() && !v.empty() && v.front() != '-') {
                return n;
            }
        } catch (const std::exception&) {
        }
        fail(std::string(name) + " must be a non-negative integer, got '" + v + "'");
    }

    [[nodiscard]] auto ms(const char* name, const char* def) const -> std::chrono::milliseconds {
        return std::chrono::milliseconds{static_cast<std::int64_t>(u64(name, def))};
    }
};

auto to_peer_id(const char* name, const std::string& v) -> kythira::host_node_id {
    auto id = kythira::parse_host_peer_node_id(v);
    if (!id) {
        fail(std::string(name) + ": " + id.error());
    }
    return std::move(id).value();
}

/// `id=value,id=value,...`
auto parse_id_map(const char* name, const std::string& text)
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
            fail(std::string(name) + " expects id=value[,id=value...], got token '" + tok + "'");
        }
        out[to_peer_id(name, tok.substr(0, eq))] = tok.substr(eq + 1);
        tok.clear();
    }
    if (out.empty()) {
        fail(std::string(name) + " is empty");
    }
    return out;
}

/// `a,b,c` without empty items.
auto split_list(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::string tok;
    for (const char c : text + ',') {
        if (c == ',') {
            if (!tok.empty()) {
                out.push_back(tok);
            }
            tok.clear();
        } else {
            tok += c;
        }
    }
    return out;
}

/// The host of `http://host:port/...` or `host:port`, brackets kept on an
/// IPv6 literal.
auto host_of(const std::string& url) -> std::string {
    auto s = url;
    if (const auto p = s.find("://"); p != std::string::npos) {
        s = s.substr(p + 3);
    }
    if (const auto p = s.find('/'); p != std::string::npos) {
        s = s.substr(0, p);
    }
    if (!s.empty() && s.front() == '[') {
        const auto close = s.find(']');
        return close == std::string::npos ? s : s.substr(0, close + 1);
    }
    if (const auto p = s.rfind(':'); p != std::string::npos) {
        s = s.substr(0, p);
    }
    return s;
}

auto read_secret_file(const std::string& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fail("cannot read KINESIS_FORWARD_SECRET_FILE " + path);
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    auto s = buf.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

void read_raft_side(const reader& r, node_options& o) {
    if (auto id = kythira::parse_host_node_id(r.required("KYTHIRA_NODE_ID")); id) {
        o._node_id = std::move(id).value();
    } else {
        fail("KYTHIRA_NODE_ID: " + id.error());
    }
    o._bind_address = r.opt("KYTHIRA_RAFT_BIND", "0.0.0.0");
    const auto raft_port = r.u64("KYTHIRA_RAFT_PORT", "7000");
    if (raft_port == 0 || raft_port > 65535) {
        fail("KYTHIRA_RAFT_PORT must be between 1 and 65535");
    }
    o._raft_port = static_cast<std::uint16_t>(raft_port);
    // Raft RPC has no TLS here; off loopback that has to be asked for.
    o._allow_plaintext_raft = r.flag("KYTHIRA_ALLOW_PLAINTEXT_RAFT", "false");
    if (!o._allow_plaintext_raft && !is_loopback_address(o._bind_address)) {
        fail("refusing to serve plaintext, unauthenticated Raft RPC on KYTHIRA_RAFT_BIND=" +
             o._bind_address +
             " (set KYTHIRA_RAFT_BIND to a loopback address, or "
             "KYTHIRA_ALLOW_PLAINTEXT_RAFT=true on a network you trust)");
    }
    o._peers = parse_id_map("KYTHIRA_PEERS", r.required("KYTHIRA_PEERS"));
    if (!o._peers.contains(o._node_id)) {
        fail("KYTHIRA_PEERS must include this node's own id " +
             kythira::host_node_id_text(o._node_id));
    }
    o._shard_cuts = split_list(r.opt("KYTHIRA_SHARD_CUTS", ""));
    o._tick_interval = r.ms("KYTHIRA_TICK_INTERVAL_MS", "2");
    o._election_timeout_min = r.ms("KYTHIRA_ELECTION_TIMEOUT_MIN_MS", "150");
    o._election_timeout_max = r.ms("KYTHIRA_ELECTION_TIMEOUT_MAX_MS", "300");
    if (o._election_timeout_min > o._election_timeout_max) {
        fail("KYTHIRA_ELECTION_TIMEOUT_MIN_MS exceeds KYTHIRA_ELECTION_TIMEOUT_MAX_MS");
    }
    o._heartbeat_interval = r.ms("KYTHIRA_HEARTBEAT_INTERVAL_MS", "50");
    o._rpc_timeout = r.ms("KYTHIRA_RPC_TIMEOUT_MS", "1000");
    o._policy_interval = r.ms("KYTHIRA_POLICY_INTERVAL_MS", "10000");
    o._executor_stripes = r.u64("KYTHIRA_EXECUTOR_STRIPES", "4");
    static const std::map<std::string, log_level> levels{
        {"trace", log_level::trace}, {"debug", log_level::debug},
        {"info", log_level::info},   {"warning", log_level::warning},
        {"error", log_level::error}, {"critical", log_level::critical},
    };
    const auto level = r.opt("KYTHIRA_LOG_LEVEL", "info");
    const auto it = levels.find(level);
    if (it == levels.end()) {
        fail("KYTHIRA_LOG_LEVEL must be trace|debug|info|warning|error|critical, got '" + level +
             "'");
    }
    o._log_level = it->second;
}

void read_kinesis_side(const reader& r, node_options& o) {
    const auto listen = r.opt("KINESIS_LISTEN", "0.0.0.0:4567");
    const auto hp = kinesis::split_host_port(listen);
    if (!hp) {
        fail("KINESIS_LISTEN must be host:port, got '" + listen + "'");
    }
    o._listen_host = hp->first;
    o._listen_port = static_cast<std::uint16_t>(std::stoul(hp->second));
    o._tls_cert_path = r.opt("KINESIS_TLS_CERT", "");
    o._tls_key_path = r.opt("KINESIS_TLS_KEY", "");
    o._allow_plaintext = r.flag("KINESIS_ALLOW_PLAINTEXT", "false");
    if (o._tls_cert_path.empty() != o._tls_key_path.empty()) {
        fail("KINESIS_TLS_CERT and KINESIS_TLS_KEY go together");
    }
    if (o._tls_cert_path.empty() && !o._allow_plaintext) {
        fail(
            "KINESIS_TLS_CERT and KINESIS_TLS_KEY are required: the API carries signed "
            "requests and record data (set KINESIS_ALLOW_PLAINTEXT=true to serve HTTP on a "
            "network you trust)");
    }
    if (!o._tls_cert_path.empty() && o._allow_plaintext) {
        fail("KINESIS_ALLOW_PLAINTEXT and KINESIS_TLS_CERT are exclusive");
    }
    o._region = r.opt("KINESIS_REGION", "us-east-1");
    o._partition = r.opt("KINESIS_PARTITION", "aws");
    if (o._region.empty() || o._partition.empty()) {
        fail("KINESIS_REGION and KINESIS_PARTITION must not be empty");
    }
    using kythira::aws_auth::auth_mode;
    const auto auth = r.opt("KINESIS_AUTH", "static");
    if (auth == "static") {
        o._auth = auth_mode::static_keys;
    } else if (auth == "iam") {
        o._auth = auth_mode::iam;
    } else if (auth == "both") {
        o._auth = auth_mode::both;
    } else {
        fail("KINESIS_AUTH: static, iam or both, not '" + auth + "'");
    }
    if (o._auth == auth_mode::iam) {
        if (r.set("KINESIS_STATIC_KEYS_FILE")) {
            fail("KINESIS_AUTH=iam takes no KINESIS_STATIC_KEYS_FILE: use both, or unset it");
        }
    } else {
        o._static_keys_file = r.required("KINESIS_STATIC_KEYS_FILE");
    }
    o._data_dir = r.required("KINESIS_DATA_DIR");

    if (r.set("KINESIS_FORWARD_SECRET") && r.set("KINESIS_FORWARD_SECRET_FILE")) {
        fail("set KINESIS_FORWARD_SECRET or KINESIS_FORWARD_SECRET_FILE, not both");
    }
    o._forward_secret = r.set("KINESIS_FORWARD_SECRET_FILE")
                            ? read_secret_file(r.required("KINESIS_FORWARD_SECRET_FILE"))
                            : r.opt("KINESIS_FORWARD_SECRET", "");
    if (o._peers.size() > 1 && o._forward_secret.size() < 16) {
        fail(
            "KINESIS_FORWARD_SECRET (or _FILE) of at least 16 bytes is required with more than "
            "one voter: the gateways sign forwarded writes and segment fetches with it");
    }
    o._iterator_keys_file = r.opt("KINESIS_ITERATOR_KEYS_FILE", "");
    if (r.set("KINESIS_PEER_GATEWAYS")) {
        o._peer_gateways =
            parse_id_map("KINESIS_PEER_GATEWAYS", r.required("KINESIS_PEER_GATEWAYS"));
    } else {
        for (const auto& [id, url] : o._peers) {
            o._peer_gateways[id] = host_of(url) + ":" + std::to_string(o._listen_port);
        }
    }
    for (const auto& [id, gw] : o._peer_gateways) {
        if (!kinesis::split_host_port(gw)) {
            fail("KINESIS_PEER_GATEWAYS: '" + gw + "' is not host:port");
        }
    }
    o._peer_ca_path = r.opt("KINESIS_PEER_CA", "");

    for (const auto& item : split_list(r.opt("KINESIS_COMPAT", ""))) {
        const auto eq = item.find('=');
        if (eq == std::string::npos || eq == 0) {
            fail("KINESIS_COMPAT expects k=v[,k=v...], got '" + item + "'");
        }
        o._compat.emplace_back(item.substr(0, eq), item.substr(eq + 1));
    }
    if (auto p = kinesis::cluster_profile(o._compat); !p) {
        fail("KINESIS_COMPAT: " + p.error().message.value_or(p.error().type));
    }
    o._max_body_bytes = r.u64("KINESIS_MAX_BODY_BYTES", "8388608");
    if (o._max_body_bytes < std::uint64_t{1024} * 1024) {
        fail("KINESIS_MAX_BODY_BYTES must be at least 1048576");
    }
    o._command_timeout = r.ms("KINESIS_COMMAND_TIMEOUT_MS", "5000");

    o._disk_floor_bytes = r.u64("KINESIS_DISK_FLOOR_BYTES", "1073741824");
    o._metrics = r.flag("KINESIS_METRICS", "false");
    o._kms = r.opt("KINESIS_KMS", "off");
    if (o._kms != "off" && o._kms != "local" && o._kms != "aws") {
        fail("KINESIS_KMS must be off, local or aws");
    }
    if (r.set("KINESIS_KMS_PASSPHRASE") && r.set("KINESIS_KMS_PASSPHRASE_FILE")) {
        fail("set KINESIS_KMS_PASSPHRASE or KINESIS_KMS_PASSPHRASE_FILE, not both");
    }
    if (o._kms == "local") {
        o._kms_passphrase = r.set("KINESIS_KMS_PASSPHRASE_FILE")
                                ? read_secret_file(r.required("KINESIS_KMS_PASSPHRASE_FILE"))
                                : r.opt("KINESIS_KMS_PASSPHRASE", "");
        if (o._kms_passphrase.size() < 16) {
            fail("KINESIS_KMS=local needs KINESIS_KMS_PASSPHRASE (or _FILE) of at least 16 bytes");
        }
    }
    o._kms_region = r.opt("KINESIS_KMS_REGION", o._region.c_str());
    o._kms_endpoint = r.opt("KINESIS_KMS_ENDPOINT", "");
    if (!o._kms_endpoint.empty() && !kinesis::split_host_port(o._kms_endpoint)) {
        fail("KINESIS_KMS_ENDPOINT: '" + o._kms_endpoint + "' is not host:port");
    }
    o._maintenance_interval = r.ms("KINESIS_MAINTENANCE_INTERVAL_MS", "60000");
    if (o._maintenance_interval.count() < 100) {
        fail("KINESIS_MAINTENANCE_INTERVAL_MS must be at least 100");
    }
    const auto tier_after = r.u64("KINESIS_TIER_AFTER_HOURS", "0");
    if (tier_after > kinesis::k_max_retention_hours) {
        fail("KINESIS_TIER_AFTER_HOURS must be at most 8760");
    }
    constexpr std::uint64_t ms_per_hour = 3600U * 1000U;
    o._tier_after_ms = static_cast<std::int64_t>(tier_after * ms_per_hour);
    // Milliseconds win when set, so a test or demo can watch a segment tier
    // within seconds without a separate build. A second is the floor: below
    // it a segment would tier while it is still being written.
    if (r.set("KINESIS_TIER_AFTER_MS")) {
        const auto ms = r.u64("KINESIS_TIER_AFTER_MS", "0");
        if (ms != 0 && ms < 1000) {
            fail("KINESIS_TIER_AFTER_MS must be 0 or at least 1000");
        }
        if (ms > std::uint64_t{kinesis::k_max_retention_hours} * ms_per_hour) {
            fail("KINESIS_TIER_AFTER_MS must be at most 8760 hours");
        }
        o._tier_after_ms = static_cast<std::int64_t>(ms);
    }
    o._tier_bucket = r.opt("KINESIS_TIER_S3_BUCKET", "");
    o._tier_prefix = r.opt("KINESIS_TIER_S3_PREFIX", "");
    o._tier_region = r.opt("KINESIS_TIER_S3_REGION", o._region.c_str());
    o._tier_endpoint = r.opt("KINESIS_TIER_S3_ENDPOINT", "");
    if (o._tier_after_ms > 0 && o._tier_bucket.empty()) {
        fail("KINESIS_TIER_AFTER_HOURS (or _MS) needs KINESIS_TIER_S3_BUCKET");
    }
    if (!o._tier_bucket.empty() && !s3_tier_available()) {
        fail(
            "KINESIS_TIER_S3_BUCKET is set, but this kinesis_gateway_node was built without "
            "the AWS SDK's s3 component");
    }
}

}  // namespace

auto s3_tier_available() -> bool {
#ifdef KYTHIRA_KINESIS_HAS_S3_TIER
    return true;
#else
    return false;
#endif
}

auto is_loopback_address(const std::string& address) -> bool {
    return kythira::net_bind::is_loopback_bind_address(address);
}

auto from_env(const env_lookup& env) -> node_options {
    node_options o;
    const reader r{env};
    read_raft_side(r, o);
    read_kinesis_side(r, o);
    return o;
}

auto describe(const node_options& o) -> std::string {
    std::ostringstream out;
    out << "node_id=" << kythira::host_node_id_text(o._node_id) << '\n'
        << "raft=" << o._bind_address << ':' << o._raft_port
        << (o._allow_plaintext_raft ? " (plaintext allowed)" : "") << '\n'
        << "voters=" << o._peers.size() << '\n'
        << "shards=" << (o._shard_cuts.size() + 1) << '\n'
        << "listen=" << o._listen_host << ':' << o._listen_port
        << (o._allow_plaintext ? " http" : " https") << '\n'
        << "region=" << o._region << " partition=" << o._partition << '\n'
        << "data_dir=" << o._data_dir.string() << '\n'
        << "auth="
        << (o._auth == kythira::aws_auth::auth_mode::iam    ? "iam"
            : o._auth == kythira::aws_auth::auth_mode::both ? "both"
                                                            : "static")
        << '\n'
        << "static_keys_file=" << o._static_keys_file << '\n'
        << "forward_secret=" << (o._forward_secret.empty() ? "unset" : "set") << '\n'
        << "iterator_keys="
        << (!o._iterator_keys_file.empty() ? o._iterator_keys_file
            : o._forward_secret.empty()    ? std::string("random (this process only)")
                                           : std::string("derived from the forward secret"))
        << '\n';
    for (const auto& [id, gw] : o._peer_gateways) {
        out << "peer_gateway " << kythira::host_node_id_text(id) << '=' << gw << '\n';
    }
    out << "disk_floor_bytes=" << o._disk_floor_bytes << '\n'
        << "maintenance_interval_ms=" << o._maintenance_interval.count() << '\n'
        << "metrics=" << (o._metrics ? "on (GET /metrics)" : "off") << '\n'
        << "kms=" << o._kms
        << (o._kms == "aws" ? " region=" + o._kms_region +
                                  (o._kms_endpoint.empty() ? "" : " endpoint=" + o._kms_endpoint)
                            : std::string())
        << '\n';
    if (o._tier_after_ms > 0) {
        out << "tier=s3://" << o._tier_bucket << '/' << o._tier_prefix << " after "
            << o._tier_after_ms << "ms region=" << o._tier_region
            << (o._tier_endpoint.empty() ? "" : " endpoint=" + o._tier_endpoint) << '\n';
    } else {
        out << "tier=off\n";
    }
    for (const auto& [k, v] : o._compat) {
        out << "compat " << k << '=' << v << '\n';
    }
    return out.str();
}

auto usage() -> std::string {
    return "usage: kinesis_gateway_node [--help | --self-test | --check-config]\n"
           "\n"
           "The Kythira Kinesis Data Streams-compatible daemon. Configured from the\n"
           "environment; stopped by SIGINT or SIGTERM.\n"
           "\n"
           "  --help          print this text\n"
           "  --self-test     check the generated tables, then exit\n"
           "  --check-config  read the environment, print the settings, then exit\n"
           "\n"
           "Raft (as redis_gateway_node):\n"
           "  KYTHIRA_NODE_ID                  this node's id (required)\n"
           "  KYTHIRA_PEERS                    id=http://host:port,... for every voter, self "
           "included (required)\n"
           "  KYTHIRA_RAFT_BIND, KYTHIRA_RAFT_PORT   Raft listener (0.0.0.0, 7000)\n"
           "  KYTHIRA_ALLOW_PLAINTEXT_RAFT     required off loopback: Raft RPC is plaintext\n"
           "  KYTHIRA_SHARD_CUTS               initial group boundaries, comma-separated\n"
           "  KYTHIRA_TICK_INTERVAL_MS, KYTHIRA_ELECTION_TIMEOUT_{MIN,MAX}_MS,\n"
           "  KYTHIRA_HEARTBEAT_INTERVAL_MS, KYTHIRA_RPC_TIMEOUT_MS, KYTHIRA_POLICY_INTERVAL_MS,\n"
           "  KYTHIRA_EXECUTOR_STRIPES, KYTHIRA_LOG_LEVEL\n"
           "\n"
           "Kinesis:\n"
           "  KINESIS_LISTEN                   API listener host:port (0.0.0.0:4567)\n"
           "  KINESIS_TLS_CERT, KINESIS_TLS_KEY     PEM chain and key (required unless\n"
           "  KINESIS_ALLOW_PLAINTEXT=true)\n"
           "  KINESIS_REGION, KINESIS_PARTITION     us-east-1, aws\n"
           "  KINESIS_AUTH                     static, iam or both (static): sign in by\n"
           "                                   the keys file, Kythira IAM (KYTHIRA_IAM_*),\n"
           "                                   or the file's keys first and IAM for the rest\n"
           "  KINESIS_STATIC_KEYS_FILE         access keys: id secret account name role\n"
           "                                   (required with static and both)\n"
           "  KINESIS_DATA_DIR                 Raft state and record segments (required)\n"
           "  KINESIS_FORWARD_SECRET[_FILE]    shared by the gateways; required with >1 voter\n"
           "  KINESIS_ITERATOR_KEYS_FILE       shard-iterator keys: id secret, first signs\n"
           "                                   (default: derived from the forward secret)\n"
           "  KINESIS_PEER_GATEWAYS            id=host:port,... (default: Raft host, own port)\n"
           "  KINESIS_PEER_CA                  CA bundle for peer gateways (system store)\n"
           "  KINESIS_COMPAT                   cluster compatibility settings, k=v,...\n"
           "  KINESIS_MAX_BODY_BYTES           request body limit (8388608)\n"
           "  KINESIS_COMMAND_TIMEOUT_MS       commit timeout per write (5000)\n"
           "  KINESIS_METRICS                  serve Prometheus metrics on GET /metrics,\n"
           "                                   unauthenticated (false)\n"
           "  KINESIS_KMS                      key provider for encrypted streams: off, local\n"
           "                                   or aws (off)\n"
           "  KINESIS_KMS_PASSPHRASE[_FILE]    local: wraps data keys, same on every node\n"
           "  KINESIS_KMS_REGION               aws: KMS region (default: KINESIS_REGION)\n"
           "  KINESIS_KMS_ENDPOINT             aws: KMS host:port (default: AWS)\n"
           "\n"
           "Retention and tiering:\n"
           "  KINESIS_DISK_FLOOR_BYTES         refuse writes below this free space (1073741824;\n"
           "                                   0 = off)\n"
           "  KINESIS_MAINTENANCE_INTERVAL_MS  retention and tiering pass interval (60000)\n"
           "  KINESIS_TIER_AFTER_HOURS         tier segments this old to S3 (0 = never)\n"
           "  KINESIS_TIER_AFTER_MS            the same in milliseconds (>= 1000); wins when set\n"
           "  KINESIS_TIER_S3_BUCKET           bucket for tiered segments\n"
           "  KINESIS_TIER_S3_PREFIX           key prefix in the bucket\n"
           "  KINESIS_TIER_S3_REGION           bucket region (default: KINESIS_REGION)\n"
           "  KINESIS_TIER_S3_ENDPOINT         S3-compatible endpoint (default: AWS)\n";
}

}  // namespace kythira::kinesis_node
