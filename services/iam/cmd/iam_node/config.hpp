// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// iam_node's command line (.kiro/specs/iam-compatible-auth/ Requirement 8,
// design Component 7 and "Configuration"). The peer format and the unseal,
// plaintext and node-id rules follow ca_cluster_node's, so one operator
// runbook covers both daemons.

#include <raft/host_node_id.hpp>
#include <raft/tcp_rpc.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace iam_node {

using node_id_type = kythira::host_node_id;

struct peer_info {
    node_id_type node_id;
    std::string rpc_host;
    std::uint16_t rpc_port{0};
    std::string http_address;  // "https://iam-2.internal:8443", for leader redirects
};

struct config {
    node_id_type node_id{};
    std::string rpc_address{"0.0.0.0"};
    std::uint16_t rpc_port{7100};
    std::string http_address{"0.0.0.0"};
    std::uint16_t http_port{8443};
    std::string data_dir{"/var/lib/iam_node"};
    std::vector<peer_info> peers;
    std::string unseal_key_file;

    // The HTTPS listener (IAM, STS and /v1/admin/*).
    std::string tls_cert_path;
    std::string tls_key_path;
    // Bundle that admin client certificates must chain to. Defaults to
    // --rpc-tls-ca: both come from the Kythira CA.
    std::string client_ca_path;
    // URI SANs of the admin certificates /v1/admin/* accepts (repeatable).
    std::set<std::string> admin_uri_sans;
    // Requirement 14.1: plaintext HTTP only behind this flag, for tests.
    bool insecure_plaintext{false};

    // Raft RPC over mutual TLS (Requirement 8.2): this node's certificate
    // from the Kythira CA, which must carry its reserved peer SAN, and the
    // CA root every member's certificate chains to.
    std::string rpc_tls_cert_path;
    std::string rpc_tls_key_path;
    std::string rpc_tls_ca_path;
    bool allow_plaintext_rpc{false};

    // A read replica joins as a Raft learner; `kythira-iam replica-add`
    // registers it with the voters, which --peers lists.
    bool replica{false};
    std::size_t max_replicas{64};
    // A replica that has not held every committed change for this long
    // answers IAM and STS requests ServiceUnavailable (Requirement 7.4).
    std::chrono::seconds max_staleness{60};

    std::string arn_partition{"aws"};
    std::vector<std::string> sts_regions{"us-east-1"};

    // AssumeRoleWithWebIdentity's JWKS fetches (Requirement 11.4): hosts
    // that may be fetched (empty: any, and private addresses allowed), an
    // HTTPS proxy, and a CA bundle to trust instead of the system store
    // (for a test issuer).
    std::vector<std::string> oidc_allowed_hosts;
    std::string oidc_proxy;
    std::string oidc_ca_file;

    // The Kythira CA's OIDC issuer (task 20, Requirement 12): its URL
    // (`https://host/path`), a CA member's JWKS URL to read its keys from
    // directly over the cluster's TLS (--kythira-issuer-local), and
    // whether `POST /v1/iam/credentials` exchanges a client certificate
    // for role credentials.
    std::string kythira_issuer;
    std::string kythira_issuer_local;
    bool credentials_exchange{false};

    std::chrono::milliseconds election_timeout_min{500};
    std::chrono::milliseconds election_timeout_max{1000};
    std::chrono::milliseconds heartbeat_interval{100};
    std::chrono::milliseconds rpc_timeout{1000};

    [[nodiscard]] auto rpc_tls() const -> bool { return !rpc_tls_cert_path.empty(); }

    /// Every voter: this node and its peers, or only the peers for a
    /// replica, which is a learner.
    [[nodiscard]] auto voter_ids() const -> std::vector<node_id_type> {
        std::vector<node_id_type> ids;
        if (!replica) {
            ids.push_back(node_id);
        }
        for (const auto& p : peers) {
            ids.push_back(p.node_id);
        }
        return ids;
    }

    [[nodiscard]] auto http_address_for(const node_id_type& id) const
        -> std::optional<std::string> {
        for (const auto& p : peers) {
            if (p.node_id == id) {
                return p.http_address;
            }
        }
        return std::nullopt;
    }
};

namespace detail {

// "<node_id>:<rpc_host>:<rpc_port>@<http_address>", as ca_cluster_node
// takes it. A textual node id's own colons are part of it
// (kythira::split_leading_node_id).
[[nodiscard]] inline auto parse_peer(const std::string& tok) -> peer_info {
    const auto at = tok.find('@');
    if (at == std::string::npos || at + 1 == tok.size()) {
        throw std::invalid_argument("malformed --peers entry (expected id:host:port@url): " + tok);
    }
    const auto rpc_part = tok.substr(0, at);
    auto split = kythira::split_leading_node_id(rpc_part);
    const auto host_port = split ? std::string(split->second) : std::string{};
    const auto colon = host_port.rfind(':');
    if (!split || colon == std::string::npos || colon == 0) {
        throw std::invalid_argument("malformed --peers entry (expected id:host:port@url): " + tok);
    }
    auto id = kythira::parse_host_peer_node_id(split->first);
    if (!id) {
        throw std::invalid_argument("--peers entry " + tok + ": " + id.error());
    }
    peer_info p;
    p.node_id = std::move(id).value();
    p.rpc_host = host_port.substr(0, colon);
    const auto port = std::stoul(host_port.substr(colon + 1));
    if (port == 0 || port > 65535) {
        throw std::invalid_argument("--peers entry " + tok + ": bad port");
    }
    p.rpc_port = static_cast<std::uint16_t>(port);
    p.http_address = tok.substr(at + 1);
    if (!p.http_address.starts_with("https://") && !p.http_address.starts_with("http://")) {
        throw std::invalid_argument("--peers entry " + tok +
                                    ": the address must start with https:// or http://");
    }
    return p;
}

[[nodiscard]] inline auto split_list(const std::string& raw) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::stringstream ss(raw);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) {
            out.push_back(tok);
        }
    }
    return out;
}

}  // namespace detail

/// The first line of the unseal key file, which must not be empty.
[[nodiscard]] inline auto read_unseal_passphrase(const std::string& path) -> std::string {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("cannot open --unseal-key-file " + path);
    }
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    if (line.empty()) {
        throw std::runtime_error("--unseal-key-file " + path + " is empty");
    }
    return line;
}

inline constexpr const char* k_usage =
    "Usage: iam_node --node-id <id> --data-dir <path> --unseal-key-file <path>\n"
    "                --peers <id>:<rpc_host>:<rpc_port>@<https_url>[,...]\n"
    "                [--rpc-address <addr>] [--rpc-port <n>]\n"
    "                [--http-address <addr>] [--http-port <n>]\n"
    "                --tls-cert <path> --tls-key <path> | --insecure-plaintext\n"
    "                [--client-ca <path>] [--admin-uri-san <uri>]...\n"
    "                --rpc-tls-cert <path> --rpc-tls-key <path> --rpc-tls-ca <path>\n"
    "                  | --allow-plaintext-rpc\n"
    "                [--replica] [--max-replicas <n>] [--iam-max-staleness-secs <n>]\n"
    "                [--arn-partition <aws|aws-cn|aws-us-gov|...>]\n"
    "                [--sts-regions <region>[,...]]\n"
    "                [--oidc-allowed-hosts <host>[,...]] [--oidc-proxy <host:port>]\n"
    "                [--oidc-ca-file <path>]\n"
    "                [--kythira-issuer <https-url>] [--kythira-issuer-local <jwks-url>]\n"
    "                [--iam-credentials-exchange]\n"
    "                [--election-timeout-min-ms <n>] [--election-timeout-max-ms <n>]\n"
    "                [--heartbeat-interval-ms <n>] [--rpc-timeout-ms <n>]\n"
    "\n"
    "Environment: IAM_UNSEAL_KEY_FILE, IAM_DATA_DIR stand in for the flags.\n";

/// Parses the command line. Throws std::invalid_argument with a message for
/// the usage error; `main` prints it with `k_usage`.
[[nodiscard]] inline auto parse_args(int argc, char** argv) -> config {
    config cfg;
    std::string node_id_text;
    bool saw_data_dir = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + arg);
            }
            return argv[++i];
        };
        auto number = [&](long long lo, long long hi) -> long long {
            const auto text = next();
            std::size_t used = 0;
            long long v = 0;
            try {
                v = std::stoll(text, &used);
            } catch (const std::exception&) {
                used = 0;
            }
            if (used != text.size() || v < lo || v > hi) {
                throw std::invalid_argument(arg + " must be a number from " + std::to_string(lo) +
                                            " to " + std::to_string(hi));
            }
            return v;
        };
        if (arg == "--node-id") {
            node_id_text = next();
        } else if (arg == "--rpc-address") {
            cfg.rpc_address = next();
        } else if (arg == "--rpc-port") {
            cfg.rpc_port = static_cast<std::uint16_t>(number(1, 65535));
        } else if (arg == "--http-address") {
            cfg.http_address = next();
        } else if (arg == "--http-port") {
            cfg.http_port = static_cast<std::uint16_t>(number(0, 65535));
        } else if (arg == "--data-dir") {
            cfg.data_dir = next();
            saw_data_dir = true;
        } else if (arg == "--peers") {
            for (const auto& tok : detail::split_list(next())) {
                cfg.peers.push_back(detail::parse_peer(tok));
            }
        } else if (arg == "--unseal-key-file") {
            cfg.unseal_key_file = next();
        } else if (arg == "--tls-cert") {
            cfg.tls_cert_path = next();
        } else if (arg == "--tls-key") {
            cfg.tls_key_path = next();
        } else if (arg == "--client-ca") {
            cfg.client_ca_path = next();
        } else if (arg == "--admin-uri-san") {
            auto uri = next();
            if (uri.find(':') == std::string::npos) {
                throw std::invalid_argument("--admin-uri-san must be an absolute URI: " + uri);
            }
            cfg.admin_uri_sans.insert(std::move(uri));
        } else if (arg == "--insecure-plaintext") {
            cfg.insecure_plaintext = true;
        } else if (arg == "--rpc-tls-cert") {
            cfg.rpc_tls_cert_path = next();
        } else if (arg == "--rpc-tls-key") {
            cfg.rpc_tls_key_path = next();
        } else if (arg == "--rpc-tls-ca") {
            cfg.rpc_tls_ca_path = next();
        } else if (arg == "--allow-plaintext-rpc") {
            cfg.allow_plaintext_rpc = true;
        } else if (arg == "--replica") {
            cfg.replica = true;
        } else if (arg == "--max-replicas") {
            cfg.max_replicas = static_cast<std::size_t>(number(0, 100000));
        } else if (arg == "--iam-max-staleness-secs") {
            cfg.max_staleness = std::chrono::seconds(number(1, 86400));
        } else if (arg == "--arn-partition") {
            cfg.arn_partition = next();
            if (cfg.arn_partition.empty() ||
                cfg.arn_partition.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") !=
                    std::string::npos) {
                throw std::invalid_argument(
                    "--arn-partition must be lower-case letters, digits "
                    "and hyphens");
            }
        } else if (arg == "--sts-regions") {
            cfg.sts_regions = detail::split_list(next());
            if (cfg.sts_regions.empty()) {
                throw std::invalid_argument("--sts-regions needs at least one region");
            }
        } else if (arg == "--oidc-allowed-hosts") {
            cfg.oidc_allowed_hosts = detail::split_list(next());
        } else if (arg == "--oidc-proxy") {
            cfg.oidc_proxy = next();
        } else if (arg == "--oidc-ca-file") {
            cfg.oidc_ca_file = next();
        } else if (arg == "--kythira-issuer") {
            cfg.kythira_issuer = next();
            if (!cfg.kythira_issuer.starts_with("https://")) {
                throw std::invalid_argument("--kythira-issuer must be an https URL");
            }
            while (cfg.kythira_issuer.ends_with('/')) {
                cfg.kythira_issuer.pop_back();
            }
        } else if (arg == "--kythira-issuer-local") {
            cfg.kythira_issuer_local = next();
            if (!cfg.kythira_issuer_local.starts_with("https://")) {
                throw std::invalid_argument("--kythira-issuer-local must be an https URL");
            }
        } else if (arg == "--iam-credentials-exchange") {
            cfg.credentials_exchange = true;
        } else if (arg == "--election-timeout-min-ms") {
            cfg.election_timeout_min = std::chrono::milliseconds(number(10, 600000));
        } else if (arg == "--election-timeout-max-ms") {
            cfg.election_timeout_max = std::chrono::milliseconds(number(10, 600000));
        } else if (arg == "--heartbeat-interval-ms") {
            cfg.heartbeat_interval = std::chrono::milliseconds(number(1, 600000));
        } else if (arg == "--rpc-timeout-ms") {
            cfg.rpc_timeout = std::chrono::milliseconds(number(1, 600000));
        } else if (arg == "-h" || arg == "--help") {
            throw std::invalid_argument("");
        } else {
            throw std::invalid_argument("unrecognized argument: " + arg);
        }
    }

    auto env = [](const char* key) -> std::string {
        const char* v = std::getenv(key);
        return v != nullptr ? v : "";
    };
    if (cfg.unseal_key_file.empty()) {
        cfg.unseal_key_file = env("IAM_UNSEAL_KEY_FILE");
    }
    if (!saw_data_dir && !env("IAM_DATA_DIR").empty()) {
        cfg.data_dir = env("IAM_DATA_DIR");
    }
    if (node_id_text.empty()) {
        throw std::invalid_argument("--node-id is required");
    }
    if (cfg.unseal_key_file.empty()) {
        throw std::invalid_argument("--unseal-key-file (or $IAM_UNSEAL_KEY_FILE) is required");
    }
    if (cfg.peers.empty()) {
        throw std::invalid_argument("--peers is required");
    }
    if (cfg.election_timeout_min >= cfg.election_timeout_max) {
        throw std::invalid_argument(
            "--election-timeout-min-ms must be below --election-timeout-max-ms");
    }
    if (cfg.tls_cert_path.empty() != cfg.tls_key_path.empty()) {
        throw std::invalid_argument("--tls-cert and --tls-key go together");
    }
    if (cfg.tls_cert_path.empty() && !cfg.insecure_plaintext) {
        throw std::invalid_argument(
            "--tls-cert and --tls-key are required (or --insecure-plaintext, for tests only)");
    }
    if (!cfg.tls_cert_path.empty() && cfg.insecure_plaintext) {
        throw std::invalid_argument("--insecure-plaintext and --tls-cert are exclusive");
    }
    const int rpc_tls_given = static_cast<int>(!cfg.rpc_tls_cert_path.empty()) +
                              static_cast<int>(!cfg.rpc_tls_key_path.empty()) +
                              static_cast<int>(!cfg.rpc_tls_ca_path.empty());
    if (rpc_tls_given != 0 && rpc_tls_given != 3) {
        throw std::invalid_argument("--rpc-tls-cert, --rpc-tls-key and --rpc-tls-ca go together");
    }
    if (rpc_tls_given == 0 && !cfg.allow_plaintext_rpc &&
        !kythira::tcp_detail::is_loopback_bind_address(cfg.rpc_address)) {
        throw std::invalid_argument(
            "Raft RPC needs --rpc-tls-cert/--rpc-tls-key/--rpc-tls-ca, or --allow-plaintext-rpc "
            "(or a loopback --rpc-address)");
    }
    if (rpc_tls_given == 3 && cfg.allow_plaintext_rpc) {
        throw std::invalid_argument("--allow-plaintext-rpc and --rpc-tls-cert are exclusive");
    }
    if (cfg.client_ca_path.empty()) {
        cfg.client_ca_path = cfg.rpc_tls_ca_path;
    }
    if (!cfg.admin_uri_sans.empty() && cfg.insecure_plaintext) {
        throw std::invalid_argument(
            "--admin-uri-san needs TLS: admin clients authenticate with a certificate");
    }
    if (!cfg.admin_uri_sans.empty() && cfg.client_ca_path.empty()) {
        throw std::invalid_argument("--admin-uri-san needs --client-ca (or --rpc-tls-ca)");
    }
    for (const auto& p : cfg.peers) {
        if (p.http_address.starts_with("http://") && !cfg.insecure_plaintext) {
            throw std::invalid_argument("peer " + p.http_address +
                                        " is http://; peers serve https:// unless every node runs "
                                        "--insecure-plaintext");
        }
    }

    if ((!cfg.kythira_issuer_local.empty() || cfg.credentials_exchange) &&
        cfg.kythira_issuer.empty()) {
        throw std::invalid_argument(
            "--kythira-issuer-local and --iam-credentials-exchange need --kythira-issuer");
    }
    if (cfg.credentials_exchange && cfg.insecure_plaintext) {
        throw std::invalid_argument(
            "--iam-credentials-exchange needs TLS: the client certificate is the credential");
    }

    auto id = kythira::parse_host_node_id(node_id_text);
    if (!id) {
        throw std::invalid_argument("--node-id: " + id.error());
    }
    cfg.node_id = std::move(id).value();
    for (const auto& p : cfg.peers) {
        if (p.node_id == cfg.node_id) {
            throw std::invalid_argument("--peers lists this node's own id");
        }
    }
    try {
        (void)kythira::tcp_detail::resolve_bind_addresses(cfg.rpc_address, "--rpc-address");
        (void)kythira::tcp_detail::resolve_bind_addresses(cfg.http_address, "--http-address");
    } catch (const std::invalid_argument& e) {
        throw std::invalid_argument(e.what());
    }
    return cfg;
}

}  // namespace iam_node
