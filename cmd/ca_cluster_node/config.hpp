// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef KYTHIRA_HAS_OPENSSL
#include <raft/tls_tcp_rpc.hpp>
#endif
#include <raft/tcp_rpc.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ca_cluster_node {

// Extends chaos_node's plain peer_info with the peer's client-facing HTTP
// address — the two are not derivable from each other, and a node
// redirecting a non-leader request (Requirement 17.7) needs the *client*
// address of whichever node_id is currently leading, not its Raft RPC
// address.
struct ca_cluster_peer_info {
    std::uint64_t node_id;
    std::string rpc_host;
    std::uint16_t rpc_port;
    std::string http_address;  // e.g. "https://ca-node-2.internal:8443"
};

// A peer's client-facing address split into its parts. `host` has no IPv6
// brackets.
struct peer_url {
    std::string scheme;  // "http" or "https"
    std::string host;
    int port{0};
};

struct ca_cluster_node_config {
    std::uint64_t node_id{0};
    std::string rpc_address{"0.0.0.0"};
    std::uint16_t rpc_port{7000};
    std::uint16_t http_port{8443};
    // What the client HTTP API listens on: an IP literal, "*" (IPv4 and
    // IPv6), or an /etc/hosts name of this host. See net_bind.hpp.
    std::string http_bind_address{"0.0.0.0"};
    std::string data_dir{"/var/lib/ca_cluster_node"};
    std::vector<ca_cluster_peer_info> peers;
    std::string unseal_key_file;
    bool bootstrap_ca{false};
    std::string auth_token;
    // Set when the token came from --auth-token, which leaves it readable in
    // /proc/<pid>/cmdline and `ps` output; main() warns about it.
    bool auth_token_from_argv{false};
    std::string tls_cert_path;
    std::string tls_key_path;
    // Explicit opt-in to a plaintext client-facing HTTP API on a
    // non-loopback --http-address, and to http:// peer addresses that are
    // not loopback. The bearer token and every issued certificate cross
    // that API, so without TLS a node refuses to start unless asked.
    bool allow_plaintext_http{false};
    // CA bundle that peers' client-facing HTTPS listeners are verified
    // against when this node calls them (root fetch, enrollment, renewal).
    // Defaults to --tls-cert's own bundle: every node's listener chains to
    // the same operator root (see peer_tls_ca_file()).
    std::string peer_tls_ca_path;
    bool print_root_fingerprint{false};
    // RPC-internal mTLS (.kiro/specs/ca-cluster-rpc-mtls/, Requirement 3.1).
    // Initially points at the operator-provisioned bootstrap credential;
    // ca_cluster_node's own maintenance thread later hot-reloads the live
    // transport onto a CA-issued peer certificate persisted under
    // data_dir (Requirement 7.1) without ever touching these fields again.
    std::string rpc_tls_cert_path;
    std::string rpc_tls_key_path;
    // Explicit opt-in to plain, unauthenticated TCP for Raft RPC on a
    // non-loopback --rpc-address. Without it, a node that has no RPC TLS
    // material refuses to start (see plaintext_rpc_permitted()).
    bool allow_plaintext_rpc{false};
#ifdef KYTHIRA_HAS_OPENSSL
    // Resolved (not CLI-facing) transport config main() actually constructs
    // tls_tcp_rpc_client/server with — populated from rpc_tls_cert_path/
    // rpc_tls_key_path (or a persisted peer certificate, Requirement 7.1)
    // before run_ca_cluster_node() is instantiated.
    kythira::tls_tcp_rpc_config rpc_tls_config;
#endif

    // Resolved (not CLI-facing): HMAC key derived from the unseal passphrase
    // (raft::testing::derive_peer_enrollment_key), authenticating Raft peer
    // enrollment and the root certificate followers fetch from the leader.
    std::string peer_enrollment_key;
    std::chrono::milliseconds election_timeout_min{150};
    std::chrono::milliseconds election_timeout_max{300};
    std::chrono::milliseconds heartbeat_interval{50};
    // Per-RPC-call deadline (kythira::raft_configuration::_rpc_timeout,
    // include/raft/types.hpp) — same 100ms default as that struct itself,
    // so plain-TCP behavior is unchanged unless explicitly overridden.
    // Operators enabling --rpc-tls-cert/--rpc-tls-key SHOULD raise this:
    // every RPC call under RPC TLS pays a full TLS handshake (asymmetric
    // crypto, no session reuse across the per-call-connect transport
    // model — see include/raft/tls_tcp_rpc.hpp), which can plausibly
    // exceed 100ms under real host contention, causing the client to give
    // up on an RPC the server is still legitimately processing.
    std::chrono::milliseconds rpc_timeout{100};

    // All node IDs in the cluster (self + peers) — for set_cluster_configuration().
    [[nodiscard]] auto all_node_ids() const -> std::vector<std::uint64_t> {
        std::vector<std::uint64_t> ids;
        ids.push_back(node_id);
        for (const auto& p : peers) ids.push_back(p.node_id);
        return ids;
    }

    // Client-facing HTTP address of `id`, for building a Requirement 17.7
    // redirect Location header. std::nullopt if `id` names neither this node
    // nor a configured peer.
    [[nodiscard]] auto http_address_for(std::uint64_t id) const -> std::optional<std::string> {
        for (const auto& p : peers) {
            if (p.node_id == id) return p.http_address;
        }
        return std::nullopt;
    }
};

namespace detail {

// Parses one "<node_id>:<rpc_host>:<rpc_port>@<http_address>" token. The '@'
// separator (rather than a 4th ':'-delimited field) is required because
// http_address is itself "scheme://host:port" and already contains colons.
[[nodiscard]] inline auto parse_peer_token(const std::string& tok) -> ca_cluster_peer_info {
    auto at = tok.find('@');
    if (at == std::string::npos) {
        throw std::invalid_argument(
            "ca_cluster_node: malformed --peers token (missing '@<http_address>'): " + tok);
    }
    std::string rpc_part = tok.substr(0, at);
    std::string http_address = tok.substr(at + 1);
    if (http_address.empty()) {
        throw std::invalid_argument(
            "ca_cluster_node: malformed --peers token (empty http_address): " + tok);
    }

    auto c1 = rpc_part.find(':');
    auto c2 = c1 == std::string::npos ? std::string::npos : rpc_part.find(':', c1 + 1);
    if (c1 == std::string::npos || c2 == std::string::npos) {
        throw std::invalid_argument(
            "ca_cluster_node: malformed --peers token (expected id:host:port@http): " + tok);
    }

    ca_cluster_peer_info p;
    p.node_id = std::stoull(rpc_part.substr(0, c1));
    p.rpc_host = rpc_part.substr(c1 + 1, c2 - c1 - 1);
    p.rpc_port = static_cast<std::uint16_t>(std::stoul(rpc_part.substr(c2 + 1)));
    p.http_address = http_address;
    return p;
}

[[nodiscard]] inline auto parse_peers(const std::string& raw) -> std::vector<ca_cluster_peer_info> {
    std::vector<ca_cluster_peer_info> peers;
    std::stringstream ss(raw);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) peers.push_back(parse_peer_token(tok));
    }
    return peers;
}

}  // namespace detail

// Whether a node with no RPC TLS material may fall back to plain TCP.
// Plaintext Raft RPC lets anyone who can reach the port send AppendEntries
// or InstallSnapshot, so it is allowed only when the operator asked for it
// or when the listener is bound to loopback only (127.0.0.0/8, ::1, or a
// name such as "localhost" that /etc/hosts maps only to those) and so unreachable
// off-host.
[[nodiscard]] inline auto plaintext_rpc_permitted(const ca_cluster_node_config& cfg) -> bool {
    return cfg.allow_plaintext_rpc ||
           kythira::tcp_detail::is_loopback_bind_address(cfg.rpc_address);
}

// Whether the client-facing HTTP API may run without TLS: only when the
// operator asked for it, or when it listens on loopback only and so is
// unreachable off-host (the same rule as plaintext_rpc_permitted()).
[[nodiscard]] inline auto plaintext_http_permitted(const ca_cluster_node_config& cfg) -> bool {
    return cfg.allow_plaintext_http ||
           kythira::tcp_detail::is_loopback_bind_address(cfg.http_bind_address);
}

// Splits "http(s)://host:port" (host may be a bracketed IPv6 literal).
// Throws std::invalid_argument for any other shape: a peer address without
// an explicit scheme used to be dialled as plain HTTP whatever it said.
[[nodiscard]] inline auto parse_peer_url(const std::string& url) -> peer_url {
    peer_url out;
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        throw std::invalid_argument("peer http_address '" + url +
                                    "' must start with http:// or https://");
    }
    out.scheme = url.substr(0, scheme_end);
    if (out.scheme != "http" && out.scheme != "https") {
        throw std::invalid_argument("peer http_address '" + url +
                                    "' must start with http:// or https://");
    }
    auto rest = url.substr(scheme_end + 3);
    if (auto slash = rest.find('/'); slash != std::string::npos) rest = rest.substr(0, slash);
    const int default_port = out.scheme == "https" ? 443 : 80;
    try {
        if (rest.starts_with('[')) {
            auto close = rest.find(']');
            if (close == std::string::npos) throw std::invalid_argument("unterminated '['");
            out.host = rest.substr(1, close - 1);
            out.port = close + 1 < rest.size() && rest[close + 1] == ':'
                           ? std::stoi(rest.substr(close + 2))
                           : default_port;
        } else if (auto colon = rest.rfind(':'); colon != std::string::npos) {
            out.host = rest.substr(0, colon);
            out.port = std::stoi(rest.substr(colon + 1));
        } else {
            out.host = rest;
            out.port = default_port;
        }
    } catch (const std::exception&) {
        throw std::invalid_argument("peer http_address '" + url + "' has an invalid host or port");
    }
    if (out.host.empty() || out.port <= 0 || out.port > 65535) {
        throw std::invalid_argument("peer http_address '" + url + "' has an invalid host or port");
    }
    return out;
}

// The CA bundle peers' HTTPS listeners are verified against: --peer-tls-ca
// if given, else this node's own --tls-cert bundle (whose root every node's
// listener certificate shares). Empty when neither exists.
[[nodiscard]] inline auto peer_tls_ca_file(const ca_cluster_node_config& cfg) -> std::string {
    return cfg.peer_tls_ca_path.empty() ? cfg.tls_cert_path : cfg.peer_tls_ca_path;
}

// Checks every peer's client-facing address can be called securely. Peers
// carry enrollment requests and the RPC trust state over that link, and it
// used to be plain HTTP (or HTTPS with verification off) to any address. An
// https:// peer needs a CA bundle to verify it against; an http:// peer is
// allowed only on loopback or with --allow-plaintext-http. Returns the
// first problem, or std::nullopt.
[[nodiscard]] inline auto peer_http_security_error(const ca_cluster_node_config& cfg)
    -> std::optional<std::string> {
    for (const auto& p : cfg.peers) {
        peer_url url;
        try {
            url = parse_peer_url(p.http_address);
        } catch (const std::invalid_argument& e) {
            return e.what();
        }
        if (url.scheme == "https" && peer_tls_ca_file(cfg).empty()) {
            return "peer " + std::to_string(p.node_id) + " is https:// (" + p.http_address +
                   ") but there is no CA bundle to verify it with: pass --tls-cert/--tls-key "
                   "or --peer-tls-ca";
        }
        if (url.scheme == "http" && !cfg.allow_plaintext_http &&
            !kythira::tcp_detail::is_loopback_bind_address(url.host)) {
            return "peer " + std::to_string(p.node_id) + " is plaintext http:// (" +
                   p.http_address +
                   ") off loopback: use https://, or pass --allow-plaintext-http (or set "
                   "CA_CLUSTER_ALLOW_PLAINTEXT_HTTP=1) on a trusted network";
        }
    }
    return std::nullopt;
}

[[noreturn]] inline void usage_error(const std::string& message) {
    std::cerr
        << "ca_cluster_node: " << message << "\n\n"
        << "Usage: ca_cluster_node --node-id <n> --rpc-port <n> --http-port <n>\n"
        << "                       --data-dir <path> --unseal-key-file <path>\n"
        << "                       --peers <id>:<rpc_host>:<rpc_port>@<http_address>[,...]\n"
        << "                       [--rpc-address <addr>] [--http-address <addr>]\n"
        << "                       [--bootstrap-ca]\n"
        << "                       [--tls-cert <path> --tls-key <path>] [--peer-tls-ca <path>]\n"
        << "                       [--rpc-tls-cert <path> --rpc-tls-key <path>]\n"
        << "                       [--allow-plaintext-rpc] [--allow-plaintext-http]\n"
        << "                       [--auth-token <token>]  (prefer $CA_SERVICE_AUTH_TOKEN)\n"
        << "                       [--print-root-fingerprint]\n";
    std::exit(1);
}

[[nodiscard]] inline auto config_from_args(int argc, char** argv) -> ca_cluster_node_config {
    ca_cluster_node_config cfg;
    bool saw_node_id = false;
    bool saw_data_dir = false;
    bool saw_unseal_key_file = false;

    auto env_or = [](const char* key, const char* fallback) -> std::string {
        const char* v = std::getenv(key);
        return (v != nullptr && *v != '\0') ? v : fallback;
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage_error("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--node-id") {
            cfg.node_id = std::stoull(next());
            saw_node_id = true;
        } else if (arg == "--rpc-address") {
            cfg.rpc_address = next();
        } else if (arg == "--rpc-port") {
            cfg.rpc_port = static_cast<std::uint16_t>(std::stoul(next()));
        } else if (arg == "--http-address") {
            cfg.http_bind_address = next();
        } else if (arg == "--http-port") {
            cfg.http_port = static_cast<std::uint16_t>(std::stoul(next()));
        } else if (arg == "--data-dir") {
            cfg.data_dir = next();
            saw_data_dir = true;
        } else if (arg == "--peers") {
            cfg.peers = detail::parse_peers(next());
        } else if (arg == "--unseal-key-file") {
            cfg.unseal_key_file = next();
            saw_unseal_key_file = true;
        } else if (arg == "--bootstrap-ca") {
            cfg.bootstrap_ca = true;
        } else if (arg == "--auth-token") {
            cfg.auth_token = next();
            cfg.auth_token_from_argv = true;
        } else if (arg == "--tls-cert") {
            cfg.tls_cert_path = next();
        } else if (arg == "--tls-key") {
            cfg.tls_key_path = next();
        } else if (arg == "--rpc-tls-cert") {
            cfg.rpc_tls_cert_path = next();
        } else if (arg == "--rpc-tls-key") {
            cfg.rpc_tls_key_path = next();
        } else if (arg == "--allow-plaintext-rpc") {
            cfg.allow_plaintext_rpc = true;
        } else if (arg == "--allow-plaintext-http") {
            cfg.allow_plaintext_http = true;
        } else if (arg == "--peer-tls-ca") {
            cfg.peer_tls_ca_path = next();
        } else if (arg == "--print-root-fingerprint") {
            cfg.print_root_fingerprint = true;
        } else if (arg == "--election-timeout-min-ms") {
            cfg.election_timeout_min = std::chrono::milliseconds(std::stoll(next()));
        } else if (arg == "--election-timeout-max-ms") {
            cfg.election_timeout_max = std::chrono::milliseconds(std::stoll(next()));
        } else if (arg == "--heartbeat-interval-ms") {
            cfg.heartbeat_interval = std::chrono::milliseconds(std::stoll(next()));
        } else if (arg == "--rpc-timeout-ms") {
            cfg.rpc_timeout = std::chrono::milliseconds(std::stoll(next()));
        } else if (arg == "-h" || arg == "--help") {
            usage_error("");
        } else {
            usage_error("unrecognized argument: " + arg);
        }
    }

    if (!cfg.tls_cert_path.empty() != !cfg.tls_key_path.empty()) {
        usage_error("--tls-cert and --tls-key must be given together");
    }
    if (!cfg.rpc_tls_cert_path.empty() != !cfg.rpc_tls_key_path.empty()) {
        usage_error("--rpc-tls-cert and --rpc-tls-key must be given together");
    }
    if (cfg.print_root_fingerprint) {
        // Requirement 19.2: prints the fingerprint and exits without binding
        // any port or touching Raft/CA state — none of node-id/data-dir/
        // unseal-key/auth-token are needed for that.
        if (cfg.tls_cert_path.empty()) {
            usage_error(
                "--print-root-fingerprint requires --tls-cert/--tls-key (nothing to fingerprint "
                "without TLS configured)");
        }
        return cfg;
    }

    if (!saw_node_id) usage_error("--node-id is required");
    try {
        (void)kythira::tcp_detail::resolve_bind_addresses(cfg.rpc_address, "--rpc-address");
        (void)kythira::tcp_detail::resolve_bind_addresses(cfg.http_bind_address, "--http-address");
    } catch (const std::invalid_argument& e) {
        usage_error(e.what());
    }
    if (!cfg.allow_plaintext_rpc) {
        cfg.allow_plaintext_rpc = env_or("CA_CLUSTER_ALLOW_PLAINTEXT_RPC", "") == "1";
    }
    if (!cfg.allow_plaintext_http) {
        cfg.allow_plaintext_http = env_or("CA_CLUSTER_ALLOW_PLAINTEXT_HTTP", "") == "1";
    }
    if (!saw_data_dir) cfg.data_dir = env_or("CA_CLUSTER_DATA_DIR", cfg.data_dir.c_str());
    if (!saw_unseal_key_file) {
        cfg.unseal_key_file = env_or("CA_CLUSTER_UNSEAL_KEY_FILE", "");
    }
    if (cfg.unseal_key_file.empty()) {
        usage_error("--unseal-key-file (or $CA_CLUSTER_UNSEAL_KEY_FILE) is required");
    }
    if (cfg.auth_token.empty()) {
        cfg.auth_token = env_or("CA_SERVICE_AUTH_TOKEN", "");
    }
    if (cfg.auth_token.empty()) {
        usage_error("$CA_SERVICE_AUTH_TOKEN (or --auth-token) is required (fail closed)");
    }
    if (auto err = peer_http_security_error(cfg)) usage_error(*err);

    return cfg;
}

}  // namespace ca_cluster_node
