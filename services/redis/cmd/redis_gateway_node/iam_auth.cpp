// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file iam_auth.cpp
/// @brief redis_gateway_node's IAM wiring (.kiro/specs/iam-compatible-auth/
///        task 21, Requirement 13.6): an embedded read replica of the IAM
///        Raft group and IAM's ElastiCache-token verifier on top of it, so
///        an AUTH makes no network call. These three includes are the
///        module-adapter lines in services/redis/boundary-allow.txt.

#include "iam_auth.hpp"

#include <kythira/iam/iam_embedded_replica.hpp>
#include <kythira/iam/iam_redis_verifier.hpp>
#include <kythira/iam/services/elasticache_actions.hpp>

#include <raft/host_node_id.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace kythira::redis_node {

namespace {

auto env(const char* name) -> std::optional<std::string> {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return std::nullopt;
    }
    return std::string(v);
}

auto required(const char* name) -> std::string {
    auto v = env(name);
    if (!v) {
        throw std::invalid_argument(std::string(name) +
                                    " is required when KYTHIRA_IAM_VOTERS is set");
    }
    return *v;
}

auto number(const char* name, unsigned long fallback, unsigned long max) -> unsigned long {
    const auto v = env(name);
    if (!v) {
        return fallback;
    }
    std::size_t used = 0;
    unsigned long n = 0;
    try {
        n = std::stoul(*v, &used);
    } catch (const std::exception&) {
        used = 0;
    }
    if (used != v->size() || n == 0 || n > max) {
        throw std::invalid_argument(std::string(name) + " must be an integer from 1 to " +
                                    std::to_string(max));
    }
    return n;
}

class iam_auth_impl final : public iam_auth {
public:
    iam_auth_impl(iam::embedded_replica_config replica, iam::redis_verifier_config verifier)
        : _replica(
              std::move(replica),
              [](iam::iam_service_registry& r) { r.add(iam::services::elasticache_actions()); }),
          _verifier(_replica.authorizer(), std::move(verifier)) {
        _replica.start();
    }

    auto verifier() -> credential_verifier_fn override {
        return erase_credential_verifier(_verifier);
    }

private:
    iam::embedded_replica _replica;
    iam::iam_redis_verifier _verifier;
};

}  // namespace

auto start_iam_auth_from_env() -> std::unique_ptr<iam_auth> {
    const auto voters = env("KYTHIRA_IAM_VOTERS");
    if (!voters) {
        return nullptr;
    }
    iam::embedded_replica_config r;
    r.voters = iam::parse_replica_voters(*voters);
    auto id = parse_host_peer_node_id(required("KYTHIRA_IAM_REPLICA_NODE_ID"));
    if (!id) {
        throw std::invalid_argument("KYTHIRA_IAM_REPLICA_NODE_ID: " + id.error());
    }
    r.node_id = *id;
    r.rpc_address = env("KYTHIRA_IAM_REPLICA_RPC_BIND").value_or("0.0.0.0");
    r.rpc_port = static_cast<std::uint16_t>(number("KYTHIRA_IAM_REPLICA_RPC_PORT", 0, 65535));
    if (r.rpc_port == 0) {
        (void)required("KYTHIRA_IAM_REPLICA_RPC_PORT");
    }
    r.data_dir = required("KYTHIRA_IAM_DATA_DIR");
    r.unseal_key_file = required("KYTHIRA_IAM_UNSEAL_KEY_FILE");
    r.rpc_tls_cert = required("KYTHIRA_IAM_RPC_TLS_CERT");
    r.rpc_tls_key = required("KYTHIRA_IAM_RPC_TLS_KEY");
    r.rpc_tls_ca = required("KYTHIRA_IAM_RPC_TLS_CA");
    r.max_staleness =
        std::chrono::seconds(number("KYTHIRA_IAM_MAX_STALENESS_SECS", 60, 24UL * 60 * 60));
    r.partition = env("KYTHIRA_IAM_ARN_PARTITION").value_or("aws");

    iam::redis_verifier_config v;
    v.account = required("KYTHIRA_IAM_ACCOUNT_ID");
    if (v.account.size() != 12 || v.account.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("KYTHIRA_IAM_ACCOUNT_ID must be a 12-digit account id");
    }
    v.region = required("KYTHIRA_IAM_REGION");
    v.cache_name = required("KYTHIRA_IAM_CACHE_NAME");
    v.partition = r.partition;
    std::cerr << "[info] redis_gateway_node: IAM authentication on: replica "
              << host_node_id_text(r.node_id) << " of " << r.voters.size() << " IAM voters, cache "
              << v.cache_name << " in " << v.region << "\n";
    return std::make_unique<iam_auth_impl>(std::move(r), std::move(v));
}

auto iam_auth_usage() -> std::string {
    return "IAM authentication (environment; CONFIG_REDIS_GATEWAY_IAM builds only):\n"
           "  KYTHIRA_IAM_VOTERS               id:host:port,... of the IAM group's voters' Raft\n"
           "                                   RPC; unset: IAM authentication off\n"
           "  KYTHIRA_IAM_REPLICA_NODE_ID      this node's id in the IAM group, registered with\n"
           "                                   `kythira-iam replica-add`\n"
           "  KYTHIRA_IAM_REPLICA_RPC_BIND     bind address for its RPC              [0.0.0.0]\n"
           "  KYTHIRA_IAM_REPLICA_RPC_PORT     its RPC port\n"
           "  KYTHIRA_IAM_DATA_DIR             where the replica keeps the IAM log\n"
           "  KYTHIRA_IAM_UNSEAL_KEY_FILE      the IAM cluster's unseal passphrase\n"
           "  KYTHIRA_IAM_RPC_TLS_CERT / _KEY / _CA\n"
           "                                   mutual TLS for the replica's RPC; the certificate\n"
           "                                   carries the replica id's reserved peer name\n"
           "  KYTHIRA_IAM_MAX_STALENESS_SECS   refuse IAM AUTH past this lag          [60]\n"
           "  KYTHIRA_IAM_ARN_PARTITION                                               [aws]\n"
           "  KYTHIRA_IAM_ACCOUNT_ID, KYTHIRA_IAM_REGION, KYTHIRA_IAM_CACHE_NAME\n"
           "                                   the cache's account, region and name: tokens are\n"
           "                                   checked against elasticache:Connect on its user\n"
           "                                   and replication group ARNs\n"
           "  ACL users marked auth=iam or auth=both AUTH with an ElastiCache IAM auth token.\n";
}

}  // namespace kythira::redis_node
