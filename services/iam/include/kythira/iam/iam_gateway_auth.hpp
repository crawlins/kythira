// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_gateway_auth.hpp
/// @brief An embedded IAM replica as an AWS-protocol gateway's `iam_hooks`
///        (`.kiro/specs/dynamodb-compatible-api/` task 28), for the
///        DynamoDB and Kinesis gateways' `AUTH=iam|both`.
///
/// A gateway's composition root reads the replica's settings with
/// `gateway_replica_config` (the `KYTHIRA_IAM_*` variables, the same ones
/// `redis_gateway_node` takes), builds a `gateway_auth` with the service's
/// action tables, and hands `hooks()` to `static_or_iam_authorizer`. The
/// gateway's module never includes this header; only its daemon does.

#include <kythira/iam/iam_embedded_replica.hpp>
#include <kythira/iam/iam_service_registry.hpp>
#include <kythira/iam/iam_state_machine.hpp>

#include <raft/host_node_id.hpp>
#include <raft/static_or_iam_authorizer.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kythira::iam {

/// Looks a variable up; nullopt when unset or empty.
using env_lookup = std::function<std::optional<std::string>(const char*)>;

/// The embedded replica's settings from the `KYTHIRA_IAM_*` variables.
/// `when` finishes the message for a missing one, e.g. "with
/// KINESIS_AUTH=iam or both".
/// @throws std::invalid_argument naming a missing or malformed variable.
[[nodiscard]] inline auto gateway_replica_config(const env_lookup& env, std::string_view when)
    -> embedded_replica_config {
    const auto required = [&](const char* name) {
        auto v = env(name);
        if (!v) {
            throw std::invalid_argument(std::string(name) + " is required " + std::string(when));
        }
        return *v;
    };
    const auto number = [&](const char* name, unsigned long fallback,
                            unsigned long max) -> unsigned long {
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
    };

    embedded_replica_config r;
    r.voters = parse_replica_voters(required("KYTHIRA_IAM_VOTERS"));
    auto id = parse_host_peer_node_id(required("KYTHIRA_IAM_REPLICA_NODE_ID"));
    if (!id) {
        throw std::invalid_argument("KYTHIRA_IAM_REPLICA_NODE_ID: " + id.error());
    }
    r.node_id = *id;
    r.rpc_address = env("KYTHIRA_IAM_REPLICA_RPC_BIND").value_or("0.0.0.0");
    (void)required("KYTHIRA_IAM_REPLICA_RPC_PORT");
    r.rpc_port = static_cast<std::uint16_t>(number("KYTHIRA_IAM_REPLICA_RPC_PORT", 0, 65535));
    r.data_dir = required("KYTHIRA_IAM_DATA_DIR");
    r.unseal_key_file = required("KYTHIRA_IAM_UNSEAL_KEY_FILE");
    r.rpc_tls_cert = required("KYTHIRA_IAM_RPC_TLS_CERT");
    r.rpc_tls_key = required("KYTHIRA_IAM_RPC_TLS_KEY");
    r.rpc_tls_ca = required("KYTHIRA_IAM_RPC_TLS_CA");
    r.max_staleness =
        std::chrono::seconds(number("KYTHIRA_IAM_MAX_STALENESS_SECS", 60, 24UL * 60 * 60));
    r.partition = env("KYTHIRA_IAM_ARN_PARTITION").value_or("aws");
    return r;
}

/// The `KYTHIRA_IAM_*` lines of a gateway's `--help`.
[[nodiscard]] inline auto gateway_replica_usage() -> std::string {
    return "  KYTHIRA_IAM_VOTERS               id:host:port,... of the IAM group's voters' Raft\n"
           "                                   RPC\n"
           "  KYTHIRA_IAM_REPLICA_NODE_ID      this node's id in the IAM group, registered with\n"
           "                                   `kythira-iam replica-add`\n"
           "  KYTHIRA_IAM_REPLICA_RPC_BIND     bind address for its RPC              [0.0.0.0]\n"
           "  KYTHIRA_IAM_REPLICA_RPC_PORT     its RPC port\n"
           "  KYTHIRA_IAM_DATA_DIR             where the replica keeps the IAM log\n"
           "  KYTHIRA_IAM_UNSEAL_KEY_FILE      the IAM cluster's unseal passphrase\n"
           "  KYTHIRA_IAM_RPC_TLS_CERT / _KEY / _CA\n"
           "                                   mutual TLS for the replica's RPC\n"
           "  KYTHIRA_IAM_MAX_STALENESS_SECS   refuse requests past this lag          [60]\n"
           "  KYTHIRA_IAM_ARN_PARTITION                                               [aws]\n";
}

/// An embedded IAM replica, started, and IAM's authorizer over it as a
/// gateway's hooks. The hooks refer to this object, which must outlive the
/// gateway that holds them.
class gateway_auth {
public:
    /// `register_actions` adds the service's action tables (and any other
    /// service's whose actions it authorizes, as Kinesis's Lease API does
    /// DynamoDB's).
    /// @throws std::invalid_argument or std::runtime_error on a bad
    /// configuration (see `embedded_replica`).
    gateway_auth(embedded_replica_config cfg,
                 std::function<void(iam_service_registry&)> register_actions)
        : _replica(std::move(cfg), std::move(register_actions)) {
        _replica.start();
    }

    [[nodiscard]] auto hooks() -> aws_auth::iam_hooks {
        aws_auth::iam_hooks h;
        h.authenticate = [this](const sigv4::request& req, const sigv4::service_rules& rules) {
            return _replica.authorizer().authenticate(req, rules);
        };
        h.authorize = [this](const aws_auth::principal_context& who,
                             const aws_auth::authz_request& r) {
            return _replica.authorizer().authorize(who, r);
        };
        h.has_key = [this](std::string_view id) {
            bool found = false;
            _replica.replica().reader()(
                [&](const iam_state_machine& sm) { found = sm.access_key(id).second != nullptr; });
            return found;
        };
        return h;
    }

private:
    embedded_replica _replica;
};

}  // namespace kythira::iam
