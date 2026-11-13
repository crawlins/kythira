// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file iam_auth.hpp
/// @brief dynamodb_gateway_node's IAM side (.kiro/specs/
///        dynamodb-compatible-api/ task 28): built only with
///        CONFIG_DYNAMODB_GATEWAY_IAM, and the only place the daemon meets
///        the IAM module.
///
/// `iam_auth.cpp` holds an embedded read replica of the IAM Raft group and
/// IAM's authorizer over it, so a request is authenticated and authorized
/// with no network call. The gateway sees only `iam_hooks`.

#include <raft/static_or_iam_authorizer.hpp>

#include <memory>
#include <string>

namespace dynamodb_gateway {

class iam_auth {
public:
    iam_auth() = default;
    iam_auth(const iam_auth&) = delete;
    auto operator=(const iam_auth&) -> iam_auth& = delete;
    iam_auth(iam_auth&&) = delete;
    auto operator=(iam_auth&&) -> iam_auth& = delete;
    virtual ~iam_auth() = default;

    /// The gateway's view of IAM. It refers to this object, which must
    /// outlive the gateway.
    [[nodiscard]] virtual auto hooks() -> kythira::aws_auth::iam_hooks = 0;
};

/// Reads the KYTHIRA_IAM_* variables and starts the IAM replica.
/// @throws std::invalid_argument naming a missing or malformed variable.
[[nodiscard]] auto start_iam_auth_from_env() -> std::unique_ptr<iam_auth>;

/// The KYTHIRA_IAM_* section of `--help`.
[[nodiscard]] auto iam_auth_usage() -> std::string;

}  // namespace dynamodb_gateway
