// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file iam_auth.cpp
/// @brief kinesis_gateway_node's IAM wiring (.kiro/specs/
///        dynamodb-compatible-api/ task 28): an embedded read replica of the
///        IAM Raft group with the Kinesis and lease actions registered, as
///        the gateway's `iam_hooks`. The IAM include here is the
///        module-adapter line in services/kinesis/boundary-allow.txt.

#include "iam_auth.hpp"

#include <kythira/iam/iam_gateway_auth.hpp>
#include <kythira/kinesis/kinesis_actions.hpp>
#include <kythira/kinesis/kinesis_lease_api.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace kythira::kinesis_node {

namespace {

namespace iam = kythira::iam;

class iam_auth_impl final : public iam_auth {
public:
    explicit iam_auth_impl(iam::embedded_replica_config replica)
        : _auth(std::move(replica), [](iam::iam_service_registry& r) {
              r.add(kythira::kinesis::kinesis_actions());
              // The Lease API's DynamoDB actions, as on AWS's lease table.
              r.add(kythira::kinesis::lease_actions());
          }) {}

    auto hooks() -> kythira::aws_auth::iam_hooks override { return _auth.hooks(); }

private:
    iam::gateway_auth _auth;
};

}  // namespace

auto start_iam_auth_from_env() -> std::unique_ptr<iam_auth> {
    auto r = iam::gateway_replica_config(
        [](const char* name) -> std::optional<std::string> {
            const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
            if (v == nullptr || *v == '\0') {
                return std::nullopt;
            }
            return std::string(v);
        },
        "with KINESIS_AUTH=iam or both");
    std::cerr << "[info] kinesis_gateway_node: IAM authentication on: replica "
              << kythira::host_node_id_text(r.node_id) << " of " << r.voters.size()
              << " IAM voters\n";
    return std::make_unique<iam_auth_impl>(std::move(r));
}

auto iam_auth_usage() -> std::string {
    return "\nIAM authentication (KINESIS_AUTH=iam or both; CONFIG_KINESIS_GATEWAY_IAM builds\n"
           "only):\n" +
           kythira::iam::gateway_replica_usage();
}

}  // namespace kythira::kinesis_node
